/*
 * Unit tests for protocol/RDPDRParse.c — the portable MS-RDPEFS / MS-FSCC
 * decoders used by the rdpdr channel handler and the WebDAV PROPFIND handler.
 *
 * Self-contained: no test framework. Every parse call goes through an
 * exactly-sized heap copy of the input so an out-of-bounds read is visible to
 * AddressSanitizer (tests/CMakeLists.txt links ASan when RDP_FUZZ=ON) and, on
 * a plain build, at least cannot be masked by slack in a larger buffer.
 */
#include "protocol/RDPDRParse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Minimal test harness ─────────────────────────────────────────────── */

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond) do {                                                     \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
        g_failures++;                                                        \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                        \
} while (0)

#define CHECK_EQ_U(a, b) do {                                                \
    unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    g_checks++;                                                              \
    if (_a != _b) {                                                          \
        g_failures++;                                                        \
        fprintf(stderr, "  FAIL %s:%d: %s == %s (got %llu, want %llu)\n",   \
                __FILE__, __LINE__, #a, #b, _a, _b);                         \
    }                                                                        \
} while (0)

#define CHECK_STREQ(a, b) do {                                               \
    const char *_a = (a), *_b = (b);                                         \
    g_checks++;                                                              \
    if (strcmp(_a, _b) != 0) {                                               \
        g_failures++;                                                        \
        fprintf(stderr, "  FAIL %s:%d: %s == \"%s\" (got \"%s\")\n",        \
                __FILE__, __LINE__, #a, _b, _a);                             \
    }                                                                        \
} while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { fprintf(stderr, "- %s\n", #name); name(); } while (0)

/* ── Byte builder ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t buf[4096];
    size_t  len;
} Builder;

static void b_reset(Builder *b) { b->len = 0; }
static void b_u8(Builder *b, uint8_t v)  { b->buf[b->len++] = v; }
static void b_u16(Builder *b, uint16_t v) { b_u8(b, (uint8_t)v); b_u8(b, (uint8_t)(v >> 8)); }
static void b_u32(Builder *b, uint32_t v) { b_u16(b, (uint16_t)v); b_u16(b, (uint16_t)(v >> 16)); }
static void b_u64(Builder *b, uint64_t v) { b_u32(b, (uint32_t)v); b_u32(b, (uint32_t)(v >> 32)); }
static void b_bytes(Builder *b, const void *p, size_t n) { memcpy(b->buf + b->len, p, n); b->len += n; }
static void b_utf16(Builder *b, const char *s, int nul) {
    for (; *s; s++) b_u16(b, (uint16_t)(unsigned char)*s);
    if (nul) b_u16(b, 0);
}
static void b_hdr(Builder *b, uint16_t component, uint16_t packetId) {
    b_reset(b); b_u16(b, component); b_u16(b, packetId);
}

/* Parse through an exactly-sized heap copy (see file comment). The returned
 * copy is kept alive until the caller frees it because RdpdrPdu holds
 * pointers into it. */
static uint8_t *g_exact = NULL;
static RdpdrParseStatus parse_exact(const Builder *b, size_t len, RdpdrPdu *out) {
    free(g_exact);
    g_exact = (uint8_t *)malloc(len ? len : 1);
    if (len) memcpy(g_exact, b->buf, len);
    return rdpdr_parse_pdu(g_exact, len, out);
}
static size_t ffdi_exact(const Builder *b, size_t len, RdpdrFfdiVisitor v, void *ud) {
    free(g_exact);
    g_exact = (uint8_t *)malloc(len ? len : 1);
    if (len) memcpy(g_exact, b->buf, len);
    return rdpdr_parse_ffdi(g_exact, len, v, ud);
}

/* ── Header ───────────────────────────────────────────────────────────── */

TEST(header_rejects_short_buffers) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_ANNOUNCE_REPLY);
    CHECK_EQ_U(parse_exact(&b, 0, &p), RDPDR_PARSE_TRUNCATED);
    CHECK_EQ_U(parse_exact(&b, 1, &p), RDPDR_PARSE_TRUNCATED);
    CHECK_EQ_U(parse_exact(&b, 3, &p), RDPDR_PARSE_TRUNCATED);
    CHECK_EQ_U(rdpdr_parse_pdu(NULL, 0, &p), RDPDR_PARSE_TRUNCATED);
    CHECK_EQ_U(rdpdr_parse_pdu(b.buf, 4, NULL), RDPDR_PARSE_TRUNCATED);
}

TEST(header_rejects_non_core_component) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_PRN, PAKID_CORE_CLIENT_ANNOUNCE_REPLY);
    b_u16(&b, 1); b_u16(&b, 12); b_u32(&b, 7);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_BAD_COMPONENT);
    CHECK_EQ_U(p.component, RDPDR_CTYP_PRN);
}

TEST(header_unknown_packet_id_is_ok_but_unknown) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, 0x1234);
    b_u32(&b, 0xDEADBEEF);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_UNKNOWN);
    CHECK_EQ_U(p.packetId, 0x1234);
    /* Server-to-client ids arriving from the client are "unknown" too. */
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_SERVER_ANNOUNCE);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_UNKNOWN);
}

/* ── CLIENT_ANNOUNCE_REPLY ────────────────────────────────────────────── */

TEST(announce_reply_valid) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_ANNOUNCE_REPLY);
    b_u16(&b, 0x0001); b_u16(&b, 0x000C); b_u32(&b, 0x00010007);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_CLIENT_ANNOUNCE_REPLY);
    CHECK_EQ_U(p.u.announceReply.versionMajor, 1);
    CHECK_EQ_U(p.u.announceReply.versionMinor, 12);
    CHECK_EQ_U(p.u.announceReply.clientId, 0x00010007);
}

TEST(announce_reply_truncated) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_ANNOUNCE_REPLY);
    b_u16(&b, 1); b_u16(&b, 12); b_u32(&b, 7);
    for (size_t n = 4; n < b.len; n++)
        CHECK_EQ_U(parse_exact(&b, n, &p), RDPDR_PARSE_TRUNCATED);
}

/* ── CLIENT_NAME ──────────────────────────────────────────────────────── */

TEST(client_name_unicode_valid) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 1); b_u32(&b, 0); b_u32(&b, 10);   /* "PC-1\0" = 5 UTF-16 units */
    b_utf16(&b, "PC-1", 1);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_CLIENT_NAME);
    CHECK_EQ_U(p.u.clientName.unicodeFlag, 1);
    CHECK_EQ_U(p.u.clientName.nameLen, 10);
    CHECK_STREQ(p.u.clientName.name, "PC-1");
}

TEST(client_name_ascii_valid) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 0); b_u32(&b, 437); b_u32(&b, 5);
    b_bytes(&b, "host\0", 5);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.u.clientName.codePage, 437);
    CHECK_STREQ(p.u.clientName.name, "host");
}

TEST(client_name_non_ascii_is_replaced) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 1); b_u32(&b, 0); b_u32(&b, 8);
    b_u16(&b, 'A'); b_u16(&b, 0x00E9); b_u16(&b, 0x4E2D); b_u16(&b, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_STREQ(p.u.clientName.name, "A??");
}

TEST(client_name_long_name_is_truncated_not_overflowed) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 1); b_u32(&b, 0); b_u32(&b, 400 * 2);
    for (int i = 0; i < 400; i++) b_u16(&b, 'x');
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(strlen(p.u.clientName.name), RDPDR_PARSE_NAME_MAX - 1);
    /* Unterminated ASCII name of exactly the buffer size */
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 0); b_u32(&b, 0); b_u32(&b, 300);
    for (int i = 0; i < 300; i++) b_u8(&b, 'y');
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(strlen(p.u.clientName.name), RDPDR_PARSE_NAME_MAX - 1);
}

TEST(client_name_length_exceeding_buffer_is_rejected) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 1); b_u32(&b, 0); b_u32(&b, 11);   /* claims 11, only 10 follow */
    b_utf16(&b, "PC-1", 1);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_BAD_LENGTH);
    /* Absurd length */
    b.buf[12] = 0xFF; b.buf[13] = 0xFF; b.buf[14] = 0xFF; b.buf[15] = 0xFF;
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_BAD_LENGTH);
    /* Fixed fields themselves truncated */
    for (size_t n = 4; n < 16; n++)
        CHECK_EQ_U(parse_exact(&b, n, &p), RDPDR_PARSE_TRUNCATED);
}

TEST(client_name_zero_length) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CLIENT_NAME);
    b_u32(&b, 1); b_u32(&b, 0); b_u32(&b, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_STREQ(p.u.clientName.name, "");
}

/* ── CAPABILITY_RESPONSE ──────────────────────────────────────────────── */

TEST(capability_response) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_CAPABILITY_RESPONSE);
    b_u16(&b, 5); b_u16(&b, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_CAPABILITY_RESPONSE);
    CHECK_EQ_U(p.u.capabilityResponse.numCapabilities, 5);
    CHECK_EQ_U(parse_exact(&b, 6, &p), RDPDR_PARSE_TRUNCATED);
}

/* ── DEVICE_LIST_ANNOUNCE ─────────────────────────────────────────────── */

static void put_device(Builder *b, uint32_t type, uint32_t id,
                       const char *dosName, const void *data, uint32_t dataLen) {
    char name[8] = {0};
    memcpy(name, dosName, strlen(dosName) > 8 ? 8 : strlen(dosName));
    b_u32(b, type); b_u32(b, id); b_bytes(b, name, 8); b_u32(b, dataLen);
    if (dataLen) b_bytes(b, data, dataLen);
}

TEST(device_list_valid) {
    Builder b; RdpdrPdu p;
    static const uint8_t driveData[] = { 'C', ':', 0, 0 };
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_LIST_ANNOUNCE);
    b_u32(&b, 2);
    put_device(&b, RDPDR_DTYP_FILESYSTEM, 1, "C:", driveData, sizeof(driveData));
    put_device(&b, RDPDR_DTYP_PRINT, 2, "PRN1", NULL, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_DEVICE_LIST_ANNOUNCE);
    CHECK_EQ_U(p.u.deviceList.deviceCount, 2);
    CHECK_EQ_U(p.u.deviceList.storedCount, 2);
    const RdpdrDevice *d0 = &p.u.deviceList.devices[0];
    const RdpdrDevice *d1 = &p.u.deviceList.devices[1];
    CHECK_EQ_U(d0->deviceType, RDPDR_DTYP_FILESYSTEM);
    CHECK_EQ_U(d0->deviceId, 1);
    CHECK_STREQ(d0->dosName, "C:");
    CHECK_EQ_U(d0->deviceDataLen, 4);
    CHECK(d0->deviceData == g_exact + 4 + 4 + 20);
    CHECK(memcmp(d0->deviceData, driveData, 4) == 0);
    CHECK_EQ_U(d1->deviceType, RDPDR_DTYP_PRINT);
    CHECK_EQ_U(d1->deviceId, 2);
    CHECK_STREQ(d1->dosName, "PRN1");
    CHECK_EQ_U(d1->deviceDataLen, 0);
    CHECK(d1->deviceData == NULL);
}

TEST(device_list_dos_name_without_nul_is_terminated) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_LIST_ANNOUNCE);
    b_u32(&b, 1);
    put_device(&b, RDPDR_DTYP_FILESYSTEM, 9, "ABCDEFGH", NULL, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_STREQ(p.u.deviceList.devices[0].dosName, "ABCDEFGH");
    CHECK_EQ_U(p.u.deviceList.devices[0].dosName[8], 0);
}

TEST(device_list_empty) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_LIST_ANNOUNCE);
    b_u32(&b, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.u.deviceList.storedCount, 0);
    CHECK_EQ_U(parse_exact(&b, 7, &p), RDPDR_PARSE_TRUNCATED);
}

TEST(device_list_count_exceeding_buffer_is_rejected) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_LIST_ANNOUNCE);
    b_u32(&b, 3);                                  /* claims 3, carries 2 */
    put_device(&b, RDPDR_DTYP_FILESYSTEM, 1, "C:", NULL, 0);
    put_device(&b, RDPDR_DTYP_FILESYSTEM, 2, "D:", NULL, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_TRUNCATED);
    /* Huge count must not loop or over-read */
    b.buf[4] = 0xFF; b.buf[5] = 0xFF; b.buf[6] = 0xFF; b.buf[7] = 0xFF;
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_TRUNCATED);
    /* Partial second device */
    b.buf[4] = 2; b.buf[5] = 0; b.buf[6] = 0; b.buf[7] = 0;
    CHECK_EQ_U(parse_exact(&b, b.len - 1, &p), RDPDR_PARSE_TRUNCATED);
}

TEST(device_list_data_length_exceeding_buffer_is_rejected) {
    Builder b; RdpdrPdu p;
    static const uint8_t data[4] = {1, 2, 3, 4};
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_LIST_ANNOUNCE);
    b_u32(&b, 1);
    put_device(&b, RDPDR_DTYP_FILESYSTEM, 1, "C:", data, 4);
    size_t lenOff = 8 + 16;                        /* DeviceDataLength offset */
    b.buf[lenOff] = 5;                             /* claims 5, carries 4 */
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_BAD_LENGTH);
    b.buf[lenOff] = 0xFF; b.buf[lenOff+1] = 0xFF; b.buf[lenOff+2] = 0xFF; b.buf[lenOff+3] = 0xFF;
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_BAD_LENGTH);
}

TEST(device_list_caps_stored_devices_but_validates_all) {
    Builder b; RdpdrPdu p;
    const uint32_t total = RDPDR_PARSE_MAX_DEVICES + 4;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_LIST_ANNOUNCE);
    b_u32(&b, total);
    for (uint32_t i = 0; i < total; i++)
        put_device(&b, RDPDR_DTYP_FILESYSTEM, 100 + i, "X:", NULL, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.u.deviceList.deviceCount, total);
    CHECK_EQ_U(p.u.deviceList.storedCount, RDPDR_PARSE_MAX_DEVICES);
    CHECK_EQ_U(p.u.deviceList.devices[RDPDR_PARSE_MAX_DEVICES - 1].deviceId,
               100 + RDPDR_PARSE_MAX_DEVICES - 1);
    /* A bad entry past the storage cap is still an error. */
    CHECK_EQ_U(parse_exact(&b, b.len - 1, &p), RDPDR_PARSE_TRUNCATED);
}

/* ── DEVICE_IOCOMPLETION ──────────────────────────────────────────────── */

TEST(io_completion_with_payload) {
    Builder b; RdpdrPdu p;
    static const uint8_t payload[] = { 0xAA, 0xBB, 0xCC };
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_IOCOMPLETION);
    b_u32(&b, 1); b_u32(&b, 42); b_u32(&b, 0xC0000034);
    b_bytes(&b, payload, sizeof(payload));
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.type, RDPDR_PDU_DEVICE_IOCOMPLETION);
    CHECK_EQ_U(p.u.ioCompletion.deviceId, 1);
    CHECK_EQ_U(p.u.ioCompletion.completionId, 42);
    CHECK_EQ_U(p.u.ioCompletion.ioStatus, 0xC0000034);
    CHECK_EQ_U(p.u.ioCompletion.payloadLen, 3);
    CHECK(p.u.ioCompletion.payload == g_exact + 16);
    CHECK(memcmp(p.u.ioCompletion.payload, payload, 3) == 0);
}

TEST(io_completion_without_payload_and_truncated) {
    Builder b; RdpdrPdu p;
    b_hdr(&b, RDPDR_CTYP_CORE, PAKID_CORE_DEVICE_IOCOMPLETION);
    b_u32(&b, 1); b_u32(&b, 42); b_u32(&b, 0);
    CHECK_EQ_U(parse_exact(&b, b.len, &p), RDPDR_PARSE_OK);
    CHECK_EQ_U(p.u.ioCompletion.payloadLen, 0);
    CHECK(p.u.ioCompletion.payload == NULL);
    for (size_t n = 4; n < 16; n++)
        CHECK_EQ_U(parse_exact(&b, n, &p), RDPDR_PARSE_TRUNCATED);
}

/* ── FILE_FULL_DIR_INFORMATION ────────────────────────────────────────── */

typedef struct {
    size_t   count;
    char     names[8][RDPDR_PARSE_FFDI_NAME_MAX + 1];
    uint64_t sizes[8];
    bool     dirs[8];
} FfdiSink;

static void ffdi_collect(const RdpdrFfdiEntry *e, void *ud) {
    FfdiSink *s = (FfdiSink *)ud;
    if (s->count < 8) {
        strcpy(s->names[s->count], e->name);
        s->sizes[s->count] = e->fileSize;
        s->dirs[s->count]  = e->isDirectory;
    }
    s->count++;
}

/* Append one entry; returns the offset of its NextEntryOffset field. */
static size_t put_ffdi(Builder *b, const char *name, uint64_t size,
                       uint32_t attrs, uint32_t nextOff, uint32_t nameLenOverride) {
    size_t start = b->len;
    uint32_t nameLen = nameLenOverride ? nameLenOverride : (uint32_t)(strlen(name) * 2);
    b_u32(b, nextOff);
    b_u32(b, 0);                    /* FileIndex */
    b_u64(b, 1); b_u64(b, 2);       /* CreationTime, LastAccessTime */
    b_u64(b, 0x01DB5C0000000000ULL);/* LastWriteTime */
    b_u64(b, 4);                    /* ChangeTime */
    b_u64(b, size);                 /* EndOfFile */
    b_u64(b, size);                 /* AllocationSize */
    b_u32(b, attrs);
    b_u32(b, nameLen);
    b_u32(b, 0);                    /* EaSize */
    b_utf16(b, name, 0);
    return start;
}

TEST(ffdi_valid_chain) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, ".",     0,    0x10, RDPDR_FFDI_FIXED_SIZE + 2, 0);
    put_ffdi(&b, "docs",  0,    0x10, RDPDR_FFDI_FIXED_SIZE + 8, 0);
    put_ffdi(&b, "a.txt", 1234, 0x20, 0, 0);
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 3);
    CHECK_EQ_U(s.count, 3);
    CHECK_STREQ(s.names[0], ".");
    CHECK(s.dirs[0]);
    CHECK_STREQ(s.names[1], "docs");
    CHECK(s.dirs[1]);
    CHECK_EQ_U(s.sizes[1], 0);
    CHECK_STREQ(s.names[2], "a.txt");
    CHECK(!s.dirs[2]);
    CHECK_EQ_U(s.sizes[2], 1234);
}

TEST(ffdi_entry_with_padding_between_entries) {
    /* Windows aligns entries to 8 bytes: NextEntryOffset > fixed + name. */
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, "abc", 1, 0x20, 80, 0);   /* 68 + 6 = 74, padded to 80 */
    b_u16(&b, 0); b_u16(&b, 0); b_u16(&b, 0);
    put_ffdi(&b, "d", 2, 0x20, 0, 0);
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 2);
    CHECK_STREQ(s.names[0], "abc");
    CHECK_STREQ(s.names[1], "d");
}

TEST(ffdi_truncated_fixed_header_yields_nothing) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, "a.txt", 1, 0x20, 0, 0);
    for (size_t n = 0; n < RDPDR_FFDI_FIXED_SIZE; n++) {
        s.count = 0;
        CHECK_EQ_U(ffdi_exact(&b, n, ffdi_collect, &s), 0);
    }
    CHECK_EQ_U(rdpdr_parse_ffdi(NULL, 100, ffdi_collect, &s), 0);
}

TEST(ffdi_name_length_exceeding_buffer_is_skipped) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, "a.txt", 1, 0x20, 0, 12);    /* claims 12 bytes, has 10 */
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 0);
    /* ... but a later well-formed entry is still reached. */
    b_reset(&b);
    size_t first = put_ffdi(&b, "bad", 1, 0x20, 0, 0);
    (void)first;
    b.buf[60] = 0xFF; b.buf[61] = 0xFF; b.buf[62] = 0xFF; b.buf[63] = 0xFF; /* FileNameLength */
    uint32_t next = (uint32_t)b.len;
    b.buf[0] = (uint8_t)next; b.buf[1] = (uint8_t)(next >> 8); b.buf[2] = 0; b.buf[3] = 0;
    put_ffdi(&b, "good", 7, 0x20, 0, 0);
    s.count = 0;
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
    CHECK_STREQ(s.names[0], "good");
}

TEST(ffdi_name_longer_than_cap_is_skipped_and_long_names_truncate) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    char longName[600];
    memset(longName, 'n', sizeof(longName) - 1); longName[sizeof(longName) - 1] = 0;
    put_ffdi(&b, longName, 1, 0x20, 0, 0);       /* 1198 bytes > 512 cap */
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 0);
    /* 256 UTF-16 units = 512 bytes: allowed, name fills the buffer exactly */
    longName[256] = 0;
    b_reset(&b);
    put_ffdi(&b, longName, 1, 0x20, 0, 0);
    s.count = 0;
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
    CHECK_EQ_U(strlen(s.names[0]), RDPDR_PARSE_FFDI_NAME_MAX);
}

TEST(ffdi_next_offset_beyond_buffer_stops_cleanly) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, "only", 5, 0x20, 0x1000, 0);
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
    CHECK_STREQ(s.names[0], "only");
    /* NextEntryOffset that would wrap 32-bit arithmetic (startOff + next). */
    b.buf[0] = 0xFF; b.buf[1] = 0xFF; b.buf[2] = 0xFF; b.buf[3] = 0xFF;
    s.count = 0;
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
    /* Exactly at the end of the buffer: no further entry, no over-read. */
    uint32_t next = (uint32_t)b.len;
    b.buf[0] = (uint8_t)next; b.buf[1] = (uint8_t)(next >> 8); b.buf[2] = 0; b.buf[3] = 0;
    s.count = 0;
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
}

TEST(ffdi_next_offset_smaller_than_header_does_not_loop) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, "x", 1, 0x20, 8, 0);   /* would re-parse overlapping bytes forever */
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
    b.buf[0] = 1;
    s.count = 0;
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
}

TEST(ffdi_non_ascii_name_chars_are_replaced) {
    Builder b; FfdiSink s = {0};
    b_reset(&b);
    put_ffdi(&b, "ab", 1, 0x20, 0, 0);
    b.buf[60] = 8;                       /* FileNameLength = 4 units */
    b_u16(&b, 0x00FC); b_u16(&b, 0);     /* ü, NUL */
    CHECK_EQ_U(ffdi_exact(&b, b.len, ffdi_collect, &s), 1);
    CHECK_STREQ(s.names[0], "ab__");
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void) {
    RUN(header_rejects_short_buffers);
    RUN(header_rejects_non_core_component);
    RUN(header_unknown_packet_id_is_ok_but_unknown);
    RUN(announce_reply_valid);
    RUN(announce_reply_truncated);
    RUN(client_name_unicode_valid);
    RUN(client_name_ascii_valid);
    RUN(client_name_non_ascii_is_replaced);
    RUN(client_name_long_name_is_truncated_not_overflowed);
    RUN(client_name_length_exceeding_buffer_is_rejected);
    RUN(client_name_zero_length);
    RUN(capability_response);
    RUN(device_list_valid);
    RUN(device_list_dos_name_without_nul_is_terminated);
    RUN(device_list_empty);
    RUN(device_list_count_exceeding_buffer_is_rejected);
    RUN(device_list_data_length_exceeding_buffer_is_rejected);
    RUN(device_list_caps_stored_devices_but_validates_all);
    RUN(io_completion_with_payload);
    RUN(io_completion_without_payload_and_truncated);
    RUN(ffdi_valid_chain);
    RUN(ffdi_entry_with_padding_between_entries);
    RUN(ffdi_truncated_fixed_header_yields_nothing);
    RUN(ffdi_name_length_exceeding_buffer_is_skipped);
    RUN(ffdi_name_longer_than_cap_is_skipped_and_long_names_truncate);
    RUN(ffdi_next_offset_beyond_buffer_stops_cleanly);
    RUN(ffdi_next_offset_smaller_than_header_does_not_loop);
    RUN(ffdi_non_ascii_name_chars_are_replaced);
    free(g_exact);

    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
