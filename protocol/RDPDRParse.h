#pragma once
/*
 * RDPDRParse — portable, side-effect-free parsers for the MS-RDPEFS (rdpdr)
 * PDUs the daemon receives from the client, plus the FileFullDirectoryInformation
 * (MS-FSCC §2.4.14) buffer carried by IRP_MJ_DIRECTORY_CONTROL completions.
 *
 * This file deliberately depends on nothing but the C standard library so the
 * parsers can be unit-tested and fuzzed on any platform (see tests/). The
 * protocol *handling* (state machine, channel writes, WebDAV servers) stays in
 * protocol/RDPPeer.c and protocol/RDPWebDAV.c; they call into here to decode
 * bytes and then act on the result.
 *
 * Every parser:
 *   - never reads past `len` bytes of `buf`;
 *   - rejects a PDU whose fixed fields do not fit (RDPDR_PARSE_TRUNCATED);
 *   - rejects a PDU whose embedded length field exceeds the remaining bytes
 *     (RDPDR_PARSE_BAD_LENGTH);
 *   - returns pointers INTO `buf` for variable payloads (no allocation), so the
 *     caller must not use the parsed struct after `buf` is released.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── MS-RDPEFS §2.2.1.1 header constants ───────────────────────────────── */
#define RDPDR_CTYP_CORE                  0x4472
#define RDPDR_CTYP_PRN                   0x5052
#define PAKID_CORE_SERVER_ANNOUNCE       0x496E
#define PAKID_CORE_CLIENTID_CONFIRM      0x4343
#define PAKID_CORE_CLIENT_NAME           0x434E
#define PAKID_CORE_DEVICE_IOREQUEST      0x4952
#define PAKID_CORE_DEVICE_IOCOMPLETION   0x4943
#define PAKID_CORE_CAPABILITY_REQUEST    0x5350
#define PAKID_CORE_CAPABILITY_RESPONSE   0x4350
#define PAKID_CORE_CLIENT_ANNOUNCE_REPLY 0x4352
#define PAKID_CORE_DEVICE_LIST_ANNOUNCE  0x4441
#define PAKID_CORE_DEVICE_REPLY          0x6472

/* MS-RDPEFS §2.2.1.3 DEVICE_ANNOUNCE.DeviceType */
#define RDPDR_DTYP_SERIAL                0x00000001
#define RDPDR_DTYP_PARALLEL              0x00000002
#define RDPDR_DTYP_PRINT                 0x00000004
#define RDPDR_DTYP_FILESYSTEM            0x00000008
#define RDPDR_DTYP_SMARTCARD             0x00000020

/* Size of the fixed-length part of FILE_FULL_DIR_INFORMATION (MS-FSCC §2.4.14):
 * NextEntryOffset(4) FileIndex(4) CreationTime(8) LastAccessTime(8)
 * LastWriteTime(8) ChangeTime(8) EndOfFile(8) AllocationSize(8)
 * FileAttributes(4) FileNameLength(4) EaSize(4) = 68, then FileName. */
#define RDPDR_FFDI_FIXED_SIZE            68
/* FILE_ATTRIBUTE_DIRECTORY */
#define RDPDR_FILE_ATTRIBUTE_DIRECTORY   0x00000010

/* Upper bounds baked into the parsed representation. */
#define RDPDR_PARSE_MAX_DEVICES          16   /* devices decoded from one DEVICE_LIST_ANNOUNCE */
#define RDPDR_PARSE_NAME_MAX             128  /* CLIENT_NAME computer name, incl. NUL */
#define RDPDR_PARSE_FFDI_NAME_MAX        256  /* directory entry name, excl. NUL */
#define RDPDR_PARSE_FFDI_NAME_BYTES_MAX  512  /* FileNameLength cap (UTF-16 bytes) */

typedef enum {
    RDPDR_PARSE_OK = 0,
    RDPDR_PARSE_TRUNCATED,      /* fewer bytes than the header / fixed fields need */
    RDPDR_PARSE_BAD_COMPONENT,  /* Component != RDPDR_CTYP_CORE */
    RDPDR_PARSE_BAD_LENGTH,     /* an embedded length field exceeds the buffer */
} RdpdrParseStatus;

typedef enum {
    RDPDR_PDU_UNKNOWN = 0,          /* CORE component, PacketId not handled here */
    RDPDR_PDU_CLIENT_ANNOUNCE_REPLY,
    RDPDR_PDU_CLIENT_NAME,
    RDPDR_PDU_CAPABILITY_RESPONSE,
    RDPDR_PDU_DEVICE_LIST_ANNOUNCE,
    RDPDR_PDU_DEVICE_IOCOMPLETION,
} RdpdrPduType;

/* One DEVICE_ANNOUNCE entry (MS-RDPEFS §2.2.1.3). */
typedef struct {
    uint32_t       deviceType;
    uint32_t       deviceId;
    char           dosName[9];      /* PreferredDosName, NUL-terminated */
    const uint8_t *deviceData;      /* points into the source buffer; NULL if empty */
    uint32_t       deviceDataLen;
} RdpdrDevice;

typedef struct {
    RdpdrPduType type;
    uint16_t     component;         /* always RDPDR_CTYP_CORE on success */
    uint16_t     packetId;
    union {
        /* PAKID_CORE_CLIENT_ANNOUNCE_REPLY (§2.2.2.3) */
        struct {
            uint16_t versionMajor;
            uint16_t versionMinor;
            uint32_t clientId;
        } announceReply;

        /* PAKID_CORE_CLIENT_NAME (§2.2.2.4). `name` is the computer name
         * converted to ASCII (non-ASCII code units become '?'), truncated to
         * RDPDR_PARSE_NAME_MAX-1 characters. */
        struct {
            uint32_t unicodeFlag;
            uint32_t codePage;
            uint32_t nameLen;       /* ComputerNameLen in bytes, as sent */
            char     name[RDPDR_PARSE_NAME_MAX];
        } clientName;

        /* PAKID_CORE_CAPABILITY_RESPONSE (§2.2.2.8). Capability sets are not decoded. */
        struct {
            uint16_t numCapabilities;
        } capabilityResponse;

        /* PAKID_CORE_DEVICE_LIST_ANNOUNCE (§2.2.2.9). All `deviceCount`
         * entries are validated; the first RDPDR_PARSE_MAX_DEVICES are stored. */
        struct {
            uint32_t    deviceCount;   /* as sent */
            uint32_t    storedCount;   /* entries filled in devices[] */
            RdpdrDevice devices[RDPDR_PARSE_MAX_DEVICES];
        } deviceList;

        /* PAKID_CORE_DEVICE_IOCOMPLETION (§2.2.1.5). */
        struct {
            uint32_t       deviceId;
            uint32_t       completionId;
            uint32_t       ioStatus;
            const uint8_t *payload;    /* bytes after the fixed header; points into buf */
            uint32_t       payloadLen;
        } ioCompletion;
    } u;
} RdpdrPdu;

/*
 * Decode one client-to-server rdpdr PDU. On RDPDR_PARSE_OK, `out->type` says
 * which union member is valid; RDPDR_PDU_UNKNOWN means the header parsed but
 * the PacketId is one this daemon does not handle (caller may log/ignore).
 * On any other status the contents of `out` are unspecified.
 */
RdpdrParseStatus rdpdr_parse_pdu(const uint8_t *buf, size_t len, RdpdrPdu *out);

/* One decoded FILE_FULL_DIR_INFORMATION entry. */
typedef struct {
    char     name[RDPDR_PARSE_FFDI_NAME_MAX + 1]; /* ASCII; non-ASCII -> '_' */
    uint64_t fileSize;      /* EndOfFile */
    uint64_t lastWriteTime; /* FILETIME (100ns since 1601-01-01) */
    uint32_t fileAttributes;
    bool     isDirectory;   /* fileAttributes & FILE_ATTRIBUTE_DIRECTORY */
} RdpdrFfdiEntry;

typedef void (*RdpdrFfdiVisitor)(const RdpdrFfdiEntry *entry, void *userdata);

/*
 * Walk a FILE_FULL_DIR_INFORMATION chain (the payload of an
 * IRP_MN_QUERY_DIRECTORY completion) and call `visit` for each entry whose
 * name fits. Entries with FileNameLength == 0 or > RDPDR_PARSE_FFDI_NAME_BYTES_MAX,
 * or whose name would overrun the buffer, are skipped. The walk stops at
 * NextEntryOffset == 0, at a NextEntryOffset smaller than the fixed header
 * (which could only loop or overlap), or when the next entry does not fit.
 * Returns the number of entries visited.
 */
size_t rdpdr_parse_ffdi(const uint8_t *buf, size_t len,
                        RdpdrFfdiVisitor visit, void *userdata);

#ifdef __cplusplus
}
#endif
