#define RDP_LOG_COMPONENT "webdav"
#include "logging/RDPLog.h"
#include "protocol/RDPWebDAV.h"
#include "protocol/RDPPeer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

/* POSIX networking */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <strings.h>    /* strncasecmp on macOS */

/* FreeRDP / WinPR types needed for rdpContext access */
#include <freerdp/freerdp.h>

/* ── Constants ─────────────────────────────────────────────────────────── */

#define WEBDAV_READ_BUF  65536   /* HTTP request read buffer */
#define WEBDAV_MAX_PATH  512     /* Maximum URL path length */
#define IRP_TIMEOUT_SEC  30      /* Maximum seconds to wait for an IRP response */

/* FileFullDirectoryInformation entry minimum size (without FileName). */
#define FFDI_FIXED_SIZE  72

/* ── IRP waiter ─────────────────────────────────────────────────────────
 *
 * WebDAV handler threads block on this struct waiting for the run-loop
 * thread to call the IRP completion callback.
 *
 * Locking protocol:
 *   1. WebDAV thread initialises the waiter, acquires xportLock, sends IRP,
 *      releases xportLock, then waits on the cond var.
 *   2. Run-loop thread receives IOCOMPLETION, calls rdpdr_free_request which
 *      calls irp_waiter_cb, which acquires waiter->mu, sets done=true,
 *      copies payload, signals cond var, and releases waiter->mu.
 *   3. WebDAV thread wakes, reads payload, frees it after use.
 */
/*
 * IrpWaiter is heap-allocated so its lifetime can safely outlive the WebDAV
 * handler thread in the case of a timeout.  The `refcount` field (protected
 * by `mu`) starts at 2: one reference for the waiting thread and one for the
 * pending IRP callback.  Whichever side decrements to 0 frees the struct.
 * This eliminates the use-after-free that would occur if the run-loop fires
 * the callback after the waiter has timed out and the handler has returned.
 */
typedef struct {
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    int              refcount;   /* 2 = both sides live; 1 = one side done */
    bool             done;
    uint32_t         ioStatus;
    uint8_t         *payload;    /* heap copy, freed when refcount reaches 0 */
    uint32_t         payloadLen;
} IrpWaiter;

static IrpWaiter *irp_waiter_alloc(void) {
    IrpWaiter *w = (IrpWaiter *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    w->refcount = 2;  /* one for waiter, one for callback */
    return w;
}

/* Drop one reference.  When the count reaches 0, free the waiter. */
static void irp_waiter_unref(IrpWaiter *w) {
    pthread_mutex_lock(&w->mu);
    int remaining = --w->refcount;
    pthread_mutex_unlock(&w->mu);
    if (remaining == 0) {
        free(w->payload);
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w);
    }
}

/* Callback invoked on the peer run-loop thread. */
static void irp_waiter_cb(uint32_t ioStatus, const uint8_t *payload,
                           uint32_t payloadLen, void *userdata) {
    IrpWaiter *w = (IrpWaiter *)userdata;
    pthread_mutex_lock(&w->mu);
    w->done     = true;
    w->ioStatus = ioStatus;
    if (payloadLen > 0 && payload && !w->payload) {
        w->payload = (uint8_t *)malloc(payloadLen);
        if (w->payload) {
            memcpy(w->payload, payload, payloadLen);
            w->payloadLen = payloadLen;
        }
    }
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    /* Drop the callback's reference — may free w if waiter timed out. */
    irp_waiter_unref(w);
}

/* Wait for the IRP to complete (with timeout).
 * Returns true if done==true (may still have a non-SUCCESS ioStatus).
 * On return, the caller owns the waiter's reference and must call
 * irp_waiter_unref() when finished reading the result. */
static bool irp_waiter_wait(IrpWaiter *w) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += IRP_TIMEOUT_SEC;

    pthread_mutex_lock(&w->mu);
    while (!w->done) {
        int rc = pthread_cond_timedwait(&w->cv, &w->mu, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&w->mu);
            /* Drop our reference; callback will clean up when it fires. */
            irp_waiter_unref(w);
            return false;
        }
    }
    pthread_mutex_unlock(&w->mu);
    return true;
}

/* ── Server struct ──────────────────────────────────────────────────────── */

struct RDPWebDAVServer {
    freerdp_peer   *peer;
    uint32_t        deviceId;
    char            driveName[16];   /* DOS name from DEVICE_LIST_ANNOUNCE */
    uint16_t        port;
    int             listenFd;        /* bound TCP socket */
    pthread_t       acceptThread;
    volatile bool   running;
    char            mountPoint[64];  /* /Volumes/RDP-<driveName> */
    bool            mounted;
};

/* ── HTTP helpers ───────────────────────────────────────────────────────── */

/* Send a complete response string over fd. */
static void http_send(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

/* Send a formatted HTTP response header block + optional body. */
static void http_respond(int fd, int status, const char *statusStr,
                          const char *extraHeaders,
                          const char *body, size_t bodyLen) {
    char hdr[1024];
    int hdrLen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "DAV: 1\r\n"
        "MS-Author-Via: DAV\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "\r\n",
        status, statusStr,
        bodyLen,
        extraHeaders ? extraHeaders : "");
    http_send(fd, hdr, (size_t)hdrLen);
    if (body && bodyLen > 0) http_send(fd, body, bodyLen);
}

/* ── URL / path helpers ─────────────────────────────────────────────────── */

/* Percent-decode a URI path segment in-place. */
static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Convert a URL path (starting with '/') to an RDPDR-style path.
 * The RDPDR path is relative to the drive root, using backslashes,
 * no leading slash.  Returns pointer to static buffer. */
static const char *url_to_rdpdr_path(const char *urlPath) {
    static char rdpPath[WEBDAV_MAX_PATH];
    /* Skip leading '/' */
    const char *p = urlPath;
    if (*p == '/') p++;
    snprintf(rdpPath, sizeof(rdpPath), "%s", p);
    /* Convert forward slashes to backslashes */
    for (char *c = rdpPath; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    return rdpPath;
}

/* ── PROPFIND XML builder ───────────────────────────────────────────────── */

/* Append a DAV:response element for one directory entry.
 * name     — filename (UTF-8)
 * size     — file size in bytes
 * isDir    — true if directory
 * urlBase  — the request URL path (e.g. "/")
 * buf/cap/pos — output buffer / capacity / write position */
static void propfind_append_entry(const char *name, uint64_t size, bool isDir,
                                   const char *urlBase,
                                   char **buf, size_t *cap, size_t *pos) {
    /* Build href: urlBase + "/" + name */
    char href[WEBDAV_MAX_PATH * 2];
    if (strcmp(urlBase, "/") == 0)
        snprintf(href, sizeof(href), "/%s", name);
    else
        snprintf(href, sizeof(href), "%s/%s", urlBase, name);

    const char *rtype = isDir
        ? "<D:resourcetype><D:collection/></D:resourcetype>"
        : "<D:resourcetype/>";

    /* Estimate needed: ~400 chars per entry */
    size_t need = strlen(name) + strlen(href) + 512;
    if (*pos + need > *cap) {
        *cap = (*cap + need) * 2;
        *buf = (char *)realloc(*buf, *cap);
        if (!*buf) return;
    }

    int n = snprintf(*buf + *pos, *cap - *pos,
        "<D:response>"
          "<D:href>%s</D:href>"
          "<D:propstat>"
            "<D:prop>"
              "<D:displayname>%s</D:displayname>"
              "<D:getcontentlength>%llu</D:getcontentlength>"
              "<D:getlastmodified>Wed, 01 Jan 2025 00:00:00 GMT</D:getlastmodified>"
              "%s"
            "</D:prop>"
            "<D:status>HTTP/1.1 200 OK</D:status>"
          "</D:propstat>"
        "</D:response>",
        href, name, (unsigned long long)size, rtype);
    if (n > 0) *pos += (size_t)n;
}

/* ── FileFullDirectoryInformation parser ───────────────────────────────── */

/* Read a little-endian uint32 from a byte buffer at offset, advancing offset. */
static uint32_t le32(const uint8_t *b, uint32_t *off) {
    uint32_t v = (uint32_t)b[*off]
               | ((uint32_t)b[*off+1] << 8)
               | ((uint32_t)b[*off+2] << 16)
               | ((uint32_t)b[*off+3] << 24);
    *off += 4;
    return v;
}
static uint64_t le64(const uint8_t *b, uint32_t *off) {
    uint64_t lo = le32(b, off);
    uint64_t hi = le32(b, off);
    return lo | (hi << 32);
}
static uint16_t le16(const uint8_t *b, uint32_t *off) {
    uint16_t v = (uint16_t)b[*off] | ((uint16_t)b[*off+1] << 8);
    *off += 2;
    return v;
}

/* Parse a FileFullDirectoryInformation (MS-FSCC §2.4.14) buffer.
 * Calls the visitor for each entry; stops at NextEntryOffset==0. */
typedef void (*FfdiVisitor)(const char *name, uint64_t fileSize,
                             uint32_t fileAttrs, void *ud);

/* Reserved for callers that want a callback-driven parse; currently the
 * PROPFIND handler inlines the equivalent logic for locality. */
static void __attribute__((unused))
parse_ffdi(const uint8_t *buf, uint32_t bufLen,
           FfdiVisitor visit, void *ud) {
    uint32_t off = 0;
    while (off + FFDI_FIXED_SIZE <= bufLen) {
        uint32_t startOff = off;
        uint32_t nextOff  = le32(buf, &off);
        (void)le32(buf, &off);       /* FileIndex */
        (void)le64(buf, &off);       /* CreationTime */
        (void)le64(buf, &off);       /* LastAccessTime */
        (void)le64(buf, &off);       /* LastWriteTime */
        (void)le64(buf, &off);       /* ChangeTime */
        uint64_t fileSize  = le64(buf, &off);
        (void)le64(buf, &off);       /* AllocationSize */
        uint32_t fileAttrs = le32(buf, &off);
        uint32_t nameLen   = le32(buf, &off);  /* in bytes (UTF-16LE) */
        (void)le32(buf, &off);       /* EaSize */

        /* Extract UTF-16LE filename, convert to UTF-8 (ASCII range only). */
        if (off + nameLen <= bufLen && nameLen > 0 && nameLen <= 512) {
            char name[257] = {0};
            uint32_t chars = nameLen / 2;
            if (chars > 256) chars = 256;
            for (uint32_t ci = 0; ci < chars; ci++) {
                uint16_t wc = le16(buf, &off);
                name[ci] = (wc < 128 && wc > 0) ? (char)wc : '?';
            }
            name[chars] = '\0';
            visit(name, fileSize, fileAttrs, ud);
        }

        if (nextOff == 0) break;
        off = startOff + nextOff;
    }
}

/* ── IRP round-trip helpers ─────────────────────────────────────────────── */

/* Send an IRP (under xportLock) then block (without the lock) until the
 * IOCOMPLETION arrives.  Returns the IrpWaiter; caller must call
 * irp_waiter_destroy() after consuming the result. */

typedef bool (*IrpSendFn)(void *args);

/* Arguments for rdpdr_send_create_req via generic trampoline. */
typedef struct {
    RDPPeerContext *ctx;
    uint32_t        deviceId;
    const char     *path;
    uint32_t        desiredAccess;
    uint32_t        createDisposition;
    uint32_t        createOptions;
    IrpWaiter      *waiter;
} CreateArgs;
static bool send_create(void *a) {
    CreateArgs *c = (CreateArgs *)a;
    return rdpdr_send_create_req(c->ctx, c->deviceId, c->path,
                                  c->desiredAccess, c->createDisposition,
                                  c->createOptions,
                                  irp_waiter_cb, c->waiter);
}

typedef struct { RDPPeerContext *ctx; uint32_t devId; uint32_t fid; IrpWaiter *w; } CloseArgs;
static bool send_close(void *a) {
    CloseArgs *c = (CloseArgs *)a;
    return rdpdr_send_close_req(c->ctx, c->devId, c->fid, irp_waiter_cb, c->w);
}

typedef struct { RDPPeerContext *ctx; uint32_t devId; uint32_t fid;
                 uint64_t offset; uint32_t length; IrpWaiter *w; } ReadArgs;
static bool send_read(void *a) {
    ReadArgs *r = (ReadArgs *)a;
    return rdpdr_send_read_req(r->ctx, r->devId, r->fid, r->offset, r->length,
                                irp_waiter_cb, r->w);
}

typedef struct { RDPPeerContext *ctx; uint32_t devId; uint32_t fid;
                 uint64_t offset; const uint8_t *data; uint32_t len; IrpWaiter *w; } WriteArgs;
static bool send_write(void *a) {
    WriteArgs *wr = (WriteArgs *)a;
    return rdpdr_send_write_req(wr->ctx, wr->devId, wr->fid, wr->offset,
                                 wr->data, wr->len, irp_waiter_cb, wr->w);
}

typedef struct { RDPPeerContext *ctx; uint32_t devId; uint32_t fid;
                 const char *pattern; IrpWaiter *w; } QueryDirArgs;
static bool send_query_dir(void *a) {
    QueryDirArgs *q = (QueryDirArgs *)a;
    return rdpdr_send_query_dir_req(q->ctx, q->devId, q->fid, q->pattern,
                                     irp_waiter_cb, q->w);
}

typedef struct { RDPPeerContext *ctx; uint32_t devId; uint32_t fid; IrpWaiter *w; } DeleteArgs;
static bool send_delete(void *a) {
    DeleteArgs *d = (DeleteArgs *)a;
    return rdpdr_send_delete_req(d->ctx, d->devId, d->fid, irp_waiter_cb, d->w);
}

/*
 * irp_send_wait — take xportLock, call sendFn(args) with the waiter already
 * embedded in args by the caller, release lock, then block until completion.
 *
 * On success: returns the waiter (caller must call irp_waiter_unref when done).
 * On send failure or timeout: cleans up and returns NULL.
 *
 * w_alloc is the IrpWaiter that was just allocated and placed in args; on
 * send failure (IRP never registered) we need to free both of its refs.
 */
static IrpWaiter *irp_send_wait(RDPPeerContext *ctx,
                                  bool (*sendFn)(void *), void *args,
                                  IrpWaiter *w_alloc) {
    pthread_mutex_lock(&ctx->xportLock);
    bool ok = sendFn(args);
    pthread_mutex_unlock(&ctx->xportLock);

    if (!ok) {
        /* Two possible failure modes:
         *   A) rdpdr_alloc_request returned 0 (table full) — no slot was taken,
         *      the callback ref was never handed to a pending slot, so we own
         *      both refs and must free both.
         *   B) stream-alloc or write failed AFTER alloc — rdpdr_free_request was
         *      called inside the helper and fired the callback (dropping its ref).
         *      w_alloc->done == true.
         * Peek at `done` (under lock) to distinguish. */
        pthread_mutex_lock(&w_alloc->mu);
        bool cb_fired = w_alloc->done;
        pthread_mutex_unlock(&w_alloc->mu);

        if (!cb_fired) {
            /* Case A: drop the phantom callback ref, then our own. */
            irp_waiter_unref(w_alloc); /* drops callback ref (from 2→1) */
        }
        irp_waiter_unref(w_alloc); /* drops our ref (from 1→0, frees) */
        return NULL;
    }

    if (!irp_waiter_wait(w_alloc)) {
        rdp_verbose("webdav: IRP timed out after %d seconds", IRP_TIMEOUT_SEC);
        /* irp_waiter_wait already dropped the caller's reference on timeout. */
        return NULL;
    }
    return w_alloc;  /* caller owns reference; must call irp_waiter_unref() */
}

/* Open a file on the client drive. Returns fileId via *fileId_out on success. */
static bool open_file(RDPPeerContext *ctx, uint32_t devId, const char *path,
                       uint32_t access, uint32_t disposition, uint32_t options,
                       uint32_t *fileId_out) {
    IrpWaiter *w = irp_waiter_alloc();
    if (!w) return false;
    CreateArgs args = { ctx, devId, path, access, disposition, options, w };
    IrpWaiter *ret = irp_send_wait(ctx, send_create, &args, w);
    if (!ret) return false;

    bool ok = (ret->ioStatus == 0 && ret->payloadLen >= 4);
    if (ok) {
        *fileId_out = (uint32_t)ret->payload[0]
                    | ((uint32_t)ret->payload[1] << 8)
                    | ((uint32_t)ret->payload[2] << 16)
                    | ((uint32_t)ret->payload[3] << 24);
    } else {
        rdp_verbose("webdav: open_file \"%s\" failed status=0x%08x",
                    path, ret->ioStatus);
    }
    irp_waiter_unref(ret);
    return ok;
}

static bool close_file(RDPPeerContext *ctx, uint32_t devId, uint32_t fid) {
    IrpWaiter *w = irp_waiter_alloc();
    if (!w) return false;
    CloseArgs args = { ctx, devId, fid, w };
    IrpWaiter *ret = irp_send_wait(ctx, send_close, &args, w);
    if (!ret) return false;
    irp_waiter_unref(ret);
    return true;
}

/* ── HTTP request parser ────────────────────────────────────────────────── */

typedef struct {
    char method[16];
    char path[WEBDAV_MAX_PATH];
    int  depth;             /* Depth: 0/1/infinity — from header */
    long contentLength;
} HttpRequest;

/* Parse the first line + key headers from buf.
 * Returns true if we got at least METHOD + path. */
static bool parse_request(const char *buf, size_t len, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    req->depth         = 1;  /* WebDAV default */
    req->contentLength = -1;

    /* Parse request line */
    const char *end = (const char *)memchr(buf, '\n', len);
    if (!end) return false;

    char line[256];
    size_t lineLen = (size_t)(end - buf);
    if (lineLen >= sizeof(line)) lineLen = sizeof(line) - 1;
    memcpy(line, buf, lineLen);
    line[lineLen] = '\0';
    /* Strip CR */
    if (lineLen > 0 && line[lineLen - 1] == '\r') line[--lineLen] = '\0';

    char pathBuf[WEBDAV_MAX_PATH];
    if (sscanf(line, "%15s %511s", req->method, pathBuf) != 2) return false;
    url_decode(pathBuf);
    snprintf(req->path, sizeof(req->path), "%s", pathBuf);

    /* Scan headers */
    const char *p = end + 1;
    while (p < buf + len) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(buf + len - p));
        if (!nl) break;
        size_t hLen = (size_t)(nl - p);
        char hdr[256];
        if (hLen >= sizeof(hdr)) hLen = sizeof(hdr) - 1;
        memcpy(hdr, p, hLen);
        hdr[hLen] = '\0';
        if (hLen > 0 && hdr[hLen - 1] == '\r') hdr[--hLen] = '\0';

        if (strncasecmp(hdr, "Depth:", 6) == 0) {
            const char *v = hdr + 6;
            while (*v == ' ') v++;
            if (strcmp(v, "0") == 0)        req->depth = 0;
            else if (strcmp(v, "1") == 0)   req->depth = 1;
            else                            req->depth = 99; /* infinity */
        } else if (strncasecmp(hdr, "Content-Length:", 15) == 0) {
            req->contentLength = atol(hdr + 15);
        }

        p = nl + 1;
        /* Empty line = end of headers */
        if (hLen == 0 || (hLen == 1 && hdr[0] == '\r')) break;
    }
    return true;
}

/* Find the body pointer (bytes after the blank line) in buf. */
static const char *find_body(const char *buf, size_t len, size_t *bodyLen) {
    const char *sep = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
            sep = buf + i + 4;
            break;
        }
        if (buf[i]=='\n' && buf[i+1]=='\n') { sep = buf + i + 2; break; }
    }
    if (!sep) { *bodyLen = 0; return buf + len; }
    *bodyLen = (size_t)(buf + len - sep);
    return sep;
}

/* ── Request handlers ───────────────────────────────────────────────────── */

/* Thread-local drive name for the PROPFIND root display name.
 * Set by handle_connection before dispatching. */
static __thread const char *driveName_placeholder = "/";

static void handle_options(int fd) {
    http_respond(fd, 200, "OK",
                 "Allow: OPTIONS, GET, PUT, DELETE, PROPFIND, MKCOL\r\n"
                 "Content-Type: text/plain\r\n",
                 NULL, 0);
}

/* PROPFIND — list directory or stat a single file. */
static void handle_propfind(int fd, RDPPeerContext *ctx, uint32_t devId,
                              const char *urlPath, int depth) {
    const char *rdpPath = url_to_rdpdr_path(urlPath);

    /* Open the path as a directory (try FILE_DIRECTORY_FILE first). */
    uint32_t fid = 0;
    bool isDir = false;
    /* Try opening as directory */
    if (open_file(ctx, devId, rdpPath,
                  0x80000000 /* GENERIC_READ */, 1 /* FILE_OPEN */,
                  0x00000001 /* FILE_DIRECTORY_FILE */, &fid)) {
        isDir = true;
    } else if (open_file(ctx, devId, rdpPath,
                         0x80000000, 1, 0x00000040 /* FILE_NON_DIRECTORY_FILE */, &fid)) {
        isDir = false;
    } else {
        http_respond(fd, 404, "Not Found", "Content-Type: text/plain\r\n",
                     "Not found", 9);
        return;
    }

    /* Build multistatus XML */
    size_t cap = 4096;
    size_t pos = 0;
    char *xml = (char *)malloc(cap);
    if (!xml) { close_file(ctx, devId, fid); http_respond(fd, 500, "Internal Server Error", NULL, NULL, 0); return; }

    /* XML preamble + self entry */
    const char *selfName = strrchr(urlPath, '/');
    selfName = selfName ? selfName + 1 : urlPath;
    if (*selfName == '\0') selfName = driveName_placeholder; /* filled below */

    int preambleLen = snprintf(xml + pos, cap - pos,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">");
    if (preambleLen > 0) pos += (size_t)preambleLen;

    /* Self entry */
    {
        char selfHref[WEBDAV_MAX_PATH];
        snprintf(selfHref, sizeof(selfHref), "%s", urlPath);
        const char *rtype = isDir
            ? "<D:resourcetype><D:collection/></D:resourcetype>"
            : "<D:resourcetype/>";
        int n = snprintf(xml + pos, cap - pos,
            "<D:response>"
              "<D:href>%s</D:href>"
              "<D:propstat><D:prop>"
                "<D:displayname>%s</D:displayname>"
                "<D:getcontentlength>0</D:getcontentlength>"
                "<D:getlastmodified>Wed, 01 Jan 2025 00:00:00 GMT</D:getlastmodified>"
                "%s"
              "</D:prop>"
              "<D:status>HTTP/1.1 200 OK</D:status>"
              "</D:propstat></D:response>",
            selfHref,
            (*selfName ? selfName : "/"),
            rtype);
        if (n > 0) pos += (size_t)n;
    }

    /* If depth >= 1 and it's a directory, enumerate children. */
    if (isDir && depth >= 1) {
        IrpWaiter *qw = irp_waiter_alloc();
        if (qw) {
            QueryDirArgs qargs = { ctx, devId, fid, "*", qw };
            IrpWaiter *qret = irp_send_wait(ctx, send_query_dir, &qargs, qw);
            if (qret && qret->ioStatus == 0 && qret->payload && qret->payloadLen > 0) {
                uint32_t off = 0;
                const uint8_t *fbuf = qret->payload;
                uint32_t flen = qret->payloadLen;
                while (off + FFDI_FIXED_SIZE <= flen) {
                    uint32_t startOff  = off;
                    uint32_t nextOff   = le32(fbuf, &off);
                    (void)le32(fbuf, &off);        /* FileIndex */
                    (void)le64(fbuf, &off);        /* CreationTime */
                    (void)le64(fbuf, &off);        /* LastAccessTime */
                    (void)le64(fbuf, &off);        /* LastWriteTime */
                    (void)le64(fbuf, &off);        /* ChangeTime */
                    uint64_t fsize     = le64(fbuf, &off);
                    (void)le64(fbuf, &off);        /* AllocationSize */
                    uint32_t attrs     = le32(fbuf, &off);
                    uint32_t nameLenB  = le32(fbuf, &off);
                    (void)le32(fbuf, &off);        /* EaSize */

                    if (off + nameLenB <= flen && nameLenB > 0 && nameLenB <= 512) {
                        char name[257] = {0};
                        uint32_t chars = nameLenB / 2;
                        if (chars > 256) chars = 256;
                        for (uint32_t ci = 0; ci < chars; ci++) {
                            uint16_t wc = le16(fbuf, &off);
                            name[ci] = (wc < 128 && wc > 0) ? (char)wc : '_';
                        }
                        name[chars] = '\0';
                        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                            bool entryIsDir = (attrs & 0x10) != 0;
                            propfind_append_entry(name, fsize, entryIsDir,
                                                  urlPath, &xml, &cap, &pos);
                        }
                    }

                    if (nextOff == 0) break;
                    off = startOff + nextOff;
                }
            }
            if (qret) irp_waiter_unref(qret);
        }
    }

    close_file(ctx, devId, fid);

    /* Close multistatus */
    const char *suffix = "</D:multistatus>";
    size_t sufLen = strlen(suffix);
    if (pos + sufLen + 1 > cap) {
        cap = pos + sufLen + 1;
        xml = (char *)realloc(xml, cap);
    }
    if (xml) {
        memcpy(xml + pos, suffix, sufLen);
        pos += sufLen;
        xml[pos] = '\0';
    }

    http_respond(fd, 207, "Multi-Status",
                 "Content-Type: application/xml; charset=\"utf-8\"\r\n",
                 xml, pos);
    free(xml);
}

/* GET — read file content and stream to client. */
static void handle_get(int fd, RDPPeerContext *ctx, uint32_t devId,
                         const char *urlPath) {
    const char *rdpPath = url_to_rdpdr_path(urlPath);

    uint32_t fid = 0;
    if (!open_file(ctx, devId, rdpPath, 0x80000000, 1, 0x00000040, &fid)) {
        http_respond(fd, 404, "Not Found", "Content-Type: text/plain\r\n",
                     "Not found", 9);
        return;
    }

    /* Send 200 with chunked transfer (we don't know size ahead of time). */
    const char *respHdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    http_send(fd, respHdr, strlen(respHdr));

    /* Read in 64KB chunks. */
    const uint32_t chunkSize = 65536;
    uint64_t offset = 0;
    bool eof = false;
    while (!eof) {
        IrpWaiter *rw = irp_waiter_alloc();
        if (!rw) break;
        ReadArgs rargs = { ctx, devId, fid, offset, chunkSize, rw };
        IrpWaiter *rret = irp_send_wait(ctx, send_read, &rargs, rw);
        if (!rret) break;

        if (rret->ioStatus == 0 && rret->payload && rret->payloadLen > 0) {
            char chunkHdr[32];
            int hLen = snprintf(chunkHdr, sizeof(chunkHdr), "%x\r\n",
                                (unsigned)rret->payloadLen);
            http_send(fd, chunkHdr, (size_t)hLen);
            http_send(fd, (const char *)rret->payload, rret->payloadLen);
            http_send(fd, "\r\n", 2);
            offset += rret->payloadLen;
            if (rret->payloadLen < chunkSize) eof = true;
        } else {
            eof = true;
        }
        irp_waiter_unref(rret);
    }

    /* Terminating chunk */
    http_send(fd, "0\r\n\r\n", 5);
    close_file(ctx, devId, fid);
}

/* PUT — write request body to the file on the client drive. */
static void handle_put(int fd, RDPPeerContext *ctx, uint32_t devId,
                         const char *urlPath, const uint8_t *body, size_t bodyLen) {
    const char *rdpPath = url_to_rdpdr_path(urlPath);

    uint32_t fid = 0;
    /* Open with FILE_OVERWRITE_IF so it creates if missing, truncates if existing. */
    if (!open_file(ctx, devId, rdpPath,
                   0xC0000000 /* GENERIC_READ|WRITE */,
                   5 /* FILE_OVERWRITE_IF */,
                   0x00000040 /* FILE_NON_DIRECTORY_FILE */, &fid)) {
        http_respond(fd, 500, "Internal Server Error", NULL, "Cannot create file", 18);
        return;
    }

    /* Write body in 64KB pieces. */
    const uint32_t chunk = 65536;
    uint64_t offset = 0;
    bool ok = true;
    while (offset < bodyLen) {
        uint32_t thisChunk = (uint32_t)(bodyLen - offset);
        if (thisChunk > chunk) thisChunk = chunk;
        IrpWaiter *ww = irp_waiter_alloc();
        if (!ww) { ok = false; break; }
        WriteArgs wargs = { ctx, devId, fid, offset,
                            body + offset, thisChunk, ww };
        IrpWaiter *wret = irp_send_wait(ctx, send_write, &wargs, ww);
        if (!wret || wret->ioStatus != 0) {
            if (wret) irp_waiter_unref(wret);
            ok = false;
            break;
        }
        irp_waiter_unref(wret);
        offset += thisChunk;
    }

    close_file(ctx, devId, fid);
    if (ok) http_respond(fd, 204, "No Content", NULL, NULL, 0);
    else    http_respond(fd, 500, "Internal Server Error", NULL, "Write failed", 12);
}

/* DELETE — delete a file or empty directory. */
static void handle_delete(int fd, RDPPeerContext *ctx, uint32_t devId,
                            const char *urlPath) {
    const char *rdpPath = url_to_rdpdr_path(urlPath);

    /* Open with DELETE_ON_CLOSE flag. */
    uint32_t fid = 0;
    if (!open_file(ctx, devId, rdpPath,
                   0x00010000 /* DELETE access */,
                   1 /* FILE_OPEN */,
                   0x00001000 /* FILE_DELETE_ON_CLOSE */, &fid)) {
        http_respond(fd, 404, "Not Found", NULL, "Not found", 9);
        return;
    }

    /* Mark for deletion via SetInformation. */
    IrpWaiter *dw = irp_waiter_alloc();
    bool delOk = false;
    if (dw) {
        DeleteArgs dargs = { ctx, devId, fid, dw };
        IrpWaiter *dret = irp_send_wait(ctx, send_delete, &dargs, dw);
        delOk = dret && (dret->ioStatus == 0);
        if (dret) irp_waiter_unref(dret);
    }

    close_file(ctx, devId, fid);
    if (delOk) http_respond(fd, 204, "No Content", NULL, NULL, 0);
    else        http_respond(fd, 500, "Internal Server Error", NULL, "Delete failed", 13);
}

/* MKCOL — create a directory. */
static void handle_mkcol(int fd, RDPPeerContext *ctx, uint32_t devId,
                           const char *urlPath) {
    const char *rdpPath = url_to_rdpdr_path(urlPath);

    uint32_t fid = 0;
    if (!open_file(ctx, devId, rdpPath,
                   0xC0000000 /* GENERIC_READ|WRITE */,
                   2 /* FILE_CREATE */,
                   0x00000001 /* FILE_DIRECTORY_FILE */, &fid)) {
        http_respond(fd, 405, "Method Not Allowed", NULL, "Cannot create directory", 23);
        return;
    }
    close_file(ctx, devId, fid);
    http_respond(fd, 201, "Created", NULL, NULL, 0);
}

/* ── Connection handler ─────────────────────────────────────────────────── */

static void handle_connection(RDPWebDAVServer *srv, int clientFd) {
    /* Set the thread-local drive name for PROPFIND root display. */
    driveName_placeholder = srv->driveName;

    RDPPeerContext *ctx = (RDPPeerContext *)srv->peer->context;

    /* Read the full HTTP request (headers + body) into a heap buffer. */
    size_t   bufCap  = WEBDAV_READ_BUF;
    size_t   bufLen  = 0;
    char    *buf     = (char *)malloc(bufCap);
    if (!buf) { close(clientFd); return; }

    /* Read until we have a complete request (headers + Content-Length body). */
    for (;;) {
        if (bufLen >= bufCap) {
            if (bufCap >= 16 * 1024 * 1024) break; /* 16MB hard limit */
            bufCap *= 2;
            char *nb = (char *)realloc(buf, bufCap);
            if (!nb) break;
            buf = nb;
        }
        ssize_t n = read(clientFd, buf + bufLen, bufCap - bufLen);
        if (n <= 0) break;
        bufLen += (size_t)n;

        /* Check if we've read all headers (look for CRLF CRLF). */
        bool hdrsComplete = false;
        for (size_t i = 0; i + 3 < bufLen; i++) {
            if ((buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') ||
                (buf[i]=='\n'&&buf[i+1]=='\n')) {
                hdrsComplete = true;
                break;
            }
        }
        if (!hdrsComplete) continue;

        HttpRequest req;
        if (!parse_request(buf, bufLen, &req)) break;

        /* If there's a body, make sure we've read it all. */
        if (req.contentLength > 0) {
            size_t bodyStart = 0;
            const char *body = find_body(buf, bufLen, &bodyStart);
            (void)body;
            long needed = req.contentLength - (long)(bufLen - (size_t)(body - buf));
            if (needed > 0) continue; /* need more data */
        }
        break; /* headers (and body if any) are complete */
    }

    HttpRequest req;
    if (!parse_request(buf, bufLen, &req)) {
        free(buf);
        close(clientFd);
        return;
    }

    size_t bodyLen = 0;
    const char *body = find_body(buf, bufLen, &bodyLen);

    rdp_verbose("webdav: %s %s (Depth:%d body:%zu)",
                req.method, req.path, req.depth, bodyLen);

    if (strcmp(req.method, "OPTIONS") == 0) {
        handle_options(clientFd);
    } else if (strcmp(req.method, "PROPFIND") == 0) {
        handle_propfind(clientFd, ctx, srv->deviceId, req.path, req.depth);
    } else if (strcmp(req.method, "GET") == 0 ||
               strcmp(req.method, "HEAD") == 0) {
        handle_get(clientFd, ctx, srv->deviceId, req.path);
    } else if (strcmp(req.method, "PUT") == 0) {
        handle_put(clientFd, ctx, srv->deviceId, req.path,
                   (const uint8_t *)body, bodyLen);
    } else if (strcmp(req.method, "DELETE") == 0) {
        handle_delete(clientFd, ctx, srv->deviceId, req.path);
    } else if (strcmp(req.method, "MKCOL") == 0) {
        handle_mkcol(clientFd, ctx, srv->deviceId, req.path);
    } else {
        http_respond(clientFd, 405, "Method Not Allowed",
                     "Content-Type: text/plain\r\n",
                     "Method not supported", 20);
    }

    free(buf);
    close(clientFd);
}

/* ── Accept thread ──────────────────────────────────────────────────────── */

static void *accept_thread(void *arg) {
    RDPWebDAVServer *srv = (RDPWebDAVServer *)arg;
    rdp_info("webdav: accept loop started on port %u for drive \"%s\"",
             (unsigned)srv->port, srv->driveName);

    while (srv->running) {
        struct sockaddr_in client;
        socklen_t clientLen = sizeof(client);
        int clientFd = accept(srv->listenFd, (struct sockaddr *)&client, &clientLen);
        if (clientFd < 0) {
            if (!srv->running) break;
            if (errno == EINTR || errno == EAGAIN) continue;
            rdp_verbose("webdav: accept error: %s", strerror(errno));
            break;
        }
        handle_connection(srv, clientFd);
        /* clientFd is closed inside handle_connection. */
    }

    rdp_verbose("webdav: accept loop exiting for drive \"%s\"", srv->driveName);
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

RDPWebDAVServer *rdp_webdav_server_create(freerdp_peer *peer,
                                           uint32_t deviceId,
                                           const char *driveName,
                                           uint16_t port) {
    RDPWebDAVServer *srv = (RDPWebDAVServer *)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->peer     = peer;
    srv->deviceId = deviceId;
    srv->port     = port;
    snprintf(srv->driveName, sizeof(srv->driveName), "%s",
             driveName ? driveName : "DRIVE");
    snprintf(srv->mountPoint, sizeof(srv->mountPoint),
             "/Volumes/RDP-%s", srv->driveName);
    /* Strip trailing spaces from DOS name */
    for (int i = (int)strlen(srv->mountPoint) - 1; i >= 0 && srv->mountPoint[i] == ' '; i--)
        srv->mountPoint[i] = '\0';

    /* Bind the listen socket on 127.0.0.1:<port>. */
    srv->listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listenFd < 0) {
        rdp_error("webdav: socket() failed: %s", strerror(errno));
        free(srv);
        return NULL;
    }

    int optval = 1;
    setsockopt(srv->listenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv->listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        rdp_error("webdav: bind(%u) failed: %s", (unsigned)port, strerror(errno));
        close(srv->listenFd);
        free(srv);
        return NULL;
    }

    if (listen(srv->listenFd, 16) < 0) {
        rdp_error("webdav: listen() failed: %s", strerror(errno));
        close(srv->listenFd);
        free(srv);
        return NULL;
    }

    srv->running = true;
    if (pthread_create(&srv->acceptThread, NULL, accept_thread, srv) != 0) {
        rdp_error("webdav: pthread_create failed: %s", strerror(errno));
        srv->running = false;
        close(srv->listenFd);
        free(srv);
        return NULL;
    }

    rdp_info("webdav: server created for drive \"%s\" on port %u (mount at %s)",
             srv->driveName, (unsigned)port, srv->mountPoint);
    return srv;
}

void rdp_webdav_server_destroy(RDPWebDAVServer *srv) {
    if (!srv) return;
    srv->running = false;
    /* Closing the listen socket will unblock accept() with EBADF/EINVAL. */
    if (srv->listenFd >= 0) {
        close(srv->listenFd);
        srv->listenFd = -1;
    }
    pthread_join(srv->acceptThread, NULL);
    free(srv);
}

bool rdp_webdav_mount(RDPWebDAVServer *srv) {
    if (!srv) return false;

    /* Create the mount point directory if it does not exist. */
    mkdir(srv->mountPoint, 0755);

    /* Build the URL: http://127.0.0.1:<port>/ */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)srv->port);

    /* /sbin/mount_webdav -s <url> <mountpoint>
     * -s = suppress "server not responding" errors in the UI */
    pid_t pid = fork();
    if (pid < 0) {
        rdp_error("webdav: fork() failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        /* Child */
        execl("/sbin/mount_webdav", "mount_webdav", "-s",
              url, srv->mountPoint, (char *)NULL);
        _exit(127);
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    bool ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    if (ok) {
        srv->mounted = true;
        rdp_info("webdav: mounted %s at %s", url, srv->mountPoint);
    } else {
        rdp_error("webdav: mount_webdav exited with status %d",
                  WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
    }
    return ok;
}

void rdp_webdav_unmount(RDPWebDAVServer *srv) {
    if (!srv || !srv->mounted) return;
    srv->mounted = false;

    /* diskutil unmount <mountpoint> */
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/sbin/diskutil", "diskutil", "unmount",
              srv->mountPoint, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }

    rmdir(srv->mountPoint);
    rdp_verbose("webdav: unmounted %s", srv->mountPoint);
}
