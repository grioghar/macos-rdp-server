/*
 * RDPDRParse — portable MS-RDPEFS / MS-FSCC decoders. See RDPDRParse.h.
 * No dependencies beyond the C standard library; keep it that way so the
 * unit tests and fuzz harness in tests/ build on any host.
 */
#include "protocol/RDPDRParse.h"

#include <string.h>

/* ── Bounds-checked little-endian cursor ───────────────────────────────── */

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} Cursor;

static size_t cur_remaining(const Cursor *c) {
    return c->len - c->pos;
}

static bool cur_u16(Cursor *c, uint16_t *v) {
    if (cur_remaining(c) < 2) return false;
    *v = (uint16_t)(c->buf[c->pos] | ((uint16_t)c->buf[c->pos + 1] << 8));
    c->pos += 2;
    return true;
}

static bool cur_u32(Cursor *c, uint32_t *v) {
    if (cur_remaining(c) < 4) return false;
    *v = (uint32_t)c->buf[c->pos]
       | ((uint32_t)c->buf[c->pos + 1] << 8)
       | ((uint32_t)c->buf[c->pos + 2] << 16)
       | ((uint32_t)c->buf[c->pos + 3] << 24);
    c->pos += 4;
    return true;
}

static bool cur_u64(Cursor *c, uint64_t *v) {
    uint32_t lo, hi;
    if (cur_remaining(c) < 8) return false;
    cur_u32(c, &lo);
    cur_u32(c, &hi);
    *v = (uint64_t)lo | ((uint64_t)hi << 32);
    return true;
}

static bool cur_bytes(Cursor *c, void *dst, size_t n) {
    if (cur_remaining(c) < n) return false;
    memcpy(dst, c->buf + c->pos, n);
    c->pos += n;
    return true;
}

/* ── Per-PDU decoders (header already consumed) ────────────────────────── */

static RdpdrParseStatus parse_announce_reply(Cursor *c, RdpdrPdu *out) {
    if (cur_remaining(c) < 8) return RDPDR_PARSE_TRUNCATED;
    cur_u16(c, &out->u.announceReply.versionMajor);
    cur_u16(c, &out->u.announceReply.versionMinor);
    cur_u32(c, &out->u.announceReply.clientId);
    out->type = RDPDR_PDU_CLIENT_ANNOUNCE_REPLY;
    return RDPDR_PARSE_OK;
}

static RdpdrParseStatus parse_client_name(Cursor *c, RdpdrPdu *out) {
    if (cur_remaining(c) < 12) return RDPDR_PARSE_TRUNCATED;
    uint32_t unicodeFlag, codePage, nameLen;
    cur_u32(c, &unicodeFlag);
    cur_u32(c, &codePage);
    cur_u32(c, &nameLen);
    if ((size_t)nameLen > cur_remaining(c)) return RDPDR_PARSE_BAD_LENGTH;

    out->u.clientName.unicodeFlag = unicodeFlag;
    out->u.clientName.codePage    = codePage;
    out->u.clientName.nameLen     = nameLen;

    char  *name = out->u.clientName.name;
    size_t cap  = RDPDR_PARSE_NAME_MAX - 1;
    size_t n    = 0;
    const uint8_t *p = c->buf + c->pos;

    if (unicodeFlag) {
        size_t chars = nameLen / 2;
        if (chars > cap) chars = cap;
        for (size_t i = 0; i < chars; i++) {
            uint16_t wc = (uint16_t)(p[2 * i] | ((uint16_t)p[2 * i + 1] << 8));
            if (wc == 0) break;
            name[n++] = (wc < 128) ? (char)wc : '?';
        }
    } else {
        size_t chars = nameLen;
        if (chars > cap) chars = cap;
        for (size_t i = 0; i < chars; i++) {
            uint8_t ch = p[i];
            if (ch == 0) break;
            name[n++] = (ch < 128) ? (char)ch : '?';
        }
    }
    name[n] = '\0';
    c->pos += nameLen;
    out->type = RDPDR_PDU_CLIENT_NAME;
    return RDPDR_PARSE_OK;
}

static RdpdrParseStatus parse_capability_response(Cursor *c, RdpdrPdu *out) {
    uint16_t numCaps, pad;
    if (cur_remaining(c) < 4) return RDPDR_PARSE_TRUNCATED;
    cur_u16(c, &numCaps);
    cur_u16(c, &pad);
    (void)pad;
    out->u.capabilityResponse.numCapabilities = numCaps;
    out->type = RDPDR_PDU_CAPABILITY_RESPONSE;
    return RDPDR_PARSE_OK;
}

static RdpdrParseStatus parse_device_list(Cursor *c, RdpdrPdu *out) {
    uint32_t count;
    if (!cur_u32(c, &count)) return RDPDR_PARSE_TRUNCATED;
    out->u.deviceList.deviceCount = count;
    out->u.deviceList.storedCount = 0;

    for (uint32_t i = 0; i < count; i++) {
        RdpdrDevice dev;
        memset(&dev, 0, sizeof(dev));
        /* DeviceType(4) + DeviceId(4) + PreferredDosName(8) + DeviceDataLength(4) */
        if (cur_remaining(c) < 20) return RDPDR_PARSE_TRUNCATED;
        cur_u32(c, &dev.deviceType);
        cur_u32(c, &dev.deviceId);
        cur_bytes(c, dev.dosName, 8);
        dev.dosName[8] = '\0';
        cur_u32(c, &dev.deviceDataLen);
        if ((size_t)dev.deviceDataLen > cur_remaining(c)) return RDPDR_PARSE_BAD_LENGTH;
        dev.deviceData = dev.deviceDataLen ? c->buf + c->pos : NULL;
        c->pos += dev.deviceDataLen;

        if (out->u.deviceList.storedCount < RDPDR_PARSE_MAX_DEVICES)
            out->u.deviceList.devices[out->u.deviceList.storedCount++] = dev;
    }
    out->type = RDPDR_PDU_DEVICE_LIST_ANNOUNCE;
    return RDPDR_PARSE_OK;
}

static RdpdrParseStatus parse_io_completion(Cursor *c, RdpdrPdu *out) {
    if (cur_remaining(c) < 12) return RDPDR_PARSE_TRUNCATED;
    cur_u32(c, &out->u.ioCompletion.deviceId);
    cur_u32(c, &out->u.ioCompletion.completionId);
    cur_u32(c, &out->u.ioCompletion.ioStatus);
    size_t rem = cur_remaining(c);
    out->u.ioCompletion.payload    = rem ? c->buf + c->pos : NULL;
    out->u.ioCompletion.payloadLen = (uint32_t)rem;
    c->pos += rem;
    out->type = RDPDR_PDU_DEVICE_IOCOMPLETION;
    return RDPDR_PARSE_OK;
}

/* ── Public entry points ───────────────────────────────────────────────── */

RdpdrParseStatus rdpdr_parse_pdu(const uint8_t *buf, size_t len, RdpdrPdu *out) {
    if (!out) return RDPDR_PARSE_TRUNCATED;
    memset(out, 0, sizeof(*out));
    if (!buf && len) return RDPDR_PARSE_TRUNCATED;

    Cursor c = { buf, len, 0 };
    if (len < 4) return RDPDR_PARSE_TRUNCATED;
    cur_u16(&c, &out->component);
    cur_u16(&c, &out->packetId);
    if (out->component != RDPDR_CTYP_CORE) return RDPDR_PARSE_BAD_COMPONENT;

    switch (out->packetId) {
    case PAKID_CORE_CLIENT_ANNOUNCE_REPLY: return parse_announce_reply(&c, out);
    case PAKID_CORE_CLIENT_NAME:           return parse_client_name(&c, out);
    case PAKID_CORE_CAPABILITY_RESPONSE:   return parse_capability_response(&c, out);
    case PAKID_CORE_DEVICE_LIST_ANNOUNCE:  return parse_device_list(&c, out);
    case PAKID_CORE_DEVICE_IOCOMPLETION:   return parse_io_completion(&c, out);
    default:
        out->type = RDPDR_PDU_UNKNOWN;
        return RDPDR_PARSE_OK;
    }
}

size_t rdpdr_parse_ffdi(const uint8_t *buf, size_t len,
                        RdpdrFfdiVisitor visit, void *userdata) {
    if (!buf || !visit) return 0;

    size_t visited = 0;
    size_t off     = 0;

    /* All offset arithmetic is in size_t and every candidate offset is compared
     * against `len` before use, so a hostile NextEntryOffset can neither wrap
     * nor point past the buffer. */
    while (off < len && len - off >= RDPDR_FFDI_FIXED_SIZE) {
        Cursor c = { buf, len, off };
        uint32_t nextOff, fileIndex, attrs, nameLen, eaSize;
        uint64_t ctime, atime, wtime, chtime, eof, alloc;

        cur_u32(&c, &nextOff);
        cur_u32(&c, &fileIndex);
        cur_u64(&c, &ctime);
        cur_u64(&c, &atime);
        cur_u64(&c, &wtime);
        cur_u64(&c, &chtime);
        cur_u64(&c, &eof);
        cur_u64(&c, &alloc);
        cur_u32(&c, &attrs);
        cur_u32(&c, &nameLen);
        cur_u32(&c, &eaSize);
        (void)fileIndex; (void)ctime; (void)atime; (void)chtime; (void)alloc; (void)eaSize;

        if (nameLen > 0 && nameLen <= RDPDR_PARSE_FFDI_NAME_BYTES_MAX &&
            (size_t)nameLen <= cur_remaining(&c)) {
            RdpdrFfdiEntry e;
            memset(&e, 0, sizeof(e));
            size_t chars = nameLen / 2;
            if (chars > RDPDR_PARSE_FFDI_NAME_MAX) chars = RDPDR_PARSE_FFDI_NAME_MAX;
            for (size_t i = 0; i < chars; i++) {
                uint16_t wc;
                cur_u16(&c, &wc);
                e.name[i] = (wc > 0 && wc < 128) ? (char)wc : '_';
            }
            e.name[chars]     = '\0';
            e.fileSize        = eof;
            e.lastWriteTime   = wtime;
            e.fileAttributes  = attrs;
            e.isDirectory     = (attrs & RDPDR_FILE_ATTRIBUTE_DIRECTORY) != 0;
            visit(&e, userdata);
            visited++;
        }

        if (nextOff == 0) break;
        /* An entry that claims to be shorter than its own fixed header can only
         * overlap the current one (or loop forever); treat it as end of chain. */
        if (nextOff < RDPDR_FFDI_FIXED_SIZE) break;
        if ((size_t)nextOff > len - off) break;
        off += nextOff;
    }
    return visited;
}
