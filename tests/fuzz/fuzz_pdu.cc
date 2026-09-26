// libFuzzer harness for the portable rdpdr PDU parser.
//
// Build: CC=clang CXX=clang++ cmake -S tests -B build-fuzz -DRDP_FUZZ=ON
//        cmake --build build-fuzz
// Run:   ./build-fuzz/fuzz_pdu -max_total_time=30
//
// Every input is treated as one PDU as delivered by WTSVirtualChannelRead. If
// it decodes as an IOCOMPLETION, its payload is additionally walked as a
// FILE_FULL_DIR_INFORMATION chain (what the PROPFIND handler does with
// QUERY_DIRECTORY completions), so both parsers see fuzzed bytes.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/RDPDRParse.h"

namespace {

struct Sink {
    size_t   entries;
    uint64_t sizeSum;
};

void visit(const RdpdrFfdiEntry *e, void *ud) {
    auto *s = static_cast<Sink *>(ud);
    s->entries++;
    s->sizeSum += e->fileSize;
    // Touch the whole name so ASan sees any bad write into the fixed buffer.
    volatile size_t n = strlen(e->name);
    (void)n;
}

// Copy the input into an exactly-sized heap buffer so ASan catches any
// one-past-the-end read that a stack/static buffer might silently tolerate.
struct Exact {
    uint8_t *p;
    explicit Exact(const uint8_t *src, size_t n) : p(new uint8_t[n ? n : 1]) {
        if (n) memcpy(p, src, n);
    }
    ~Exact() { delete[] p; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Exact in(data, size);

    RdpdrPdu pdu;
    RdpdrParseStatus st = rdpdr_parse_pdu(in.p, size, &pdu);
    if (st == RDPDR_PARSE_OK) {
        switch (pdu.type) {
        case RDPDR_PDU_CLIENT_NAME: {
            volatile size_t n = strlen(pdu.u.clientName.name);
            (void)n;
            break;
        }
        case RDPDR_PDU_DEVICE_LIST_ANNOUNCE:
            for (uint32_t i = 0; i < pdu.u.deviceList.storedCount; i++) {
                const RdpdrDevice &d = pdu.u.deviceList.devices[i];
                volatile size_t n = strlen(d.dosName);
                (void)n;
                if (d.deviceDataLen) {
                    volatile uint8_t last = d.deviceData[d.deviceDataLen - 1];
                    (void)last;
                }
            }
            break;
        case RDPDR_PDU_DEVICE_IOCOMPLETION: {
            Sink s = {0, 0};
            rdpdr_parse_ffdi(pdu.u.ioCompletion.payload,
                             pdu.u.ioCompletion.payloadLen, visit, &s);
            break;
        }
        default:
            break;
        }
    }

    // Also feed the raw input straight to the FFDI walker: the completion
    // header is only 16 bytes, so this reaches deeper chains sooner.
    Sink s = {0, 0};
    rdpdr_parse_ffdi(in.p, size, visit, &s);
    return 0;
}
