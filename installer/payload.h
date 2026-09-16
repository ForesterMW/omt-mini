// Layout of the payload appended to OMTMini-Setup.exe by scripts/package.sh.
//
// [ setup executable ]
// [ entry 0 ] [ entry 1 ] ... [ entry n-1 ]
// [ Footer ]
//
// Each entry is: name length (u16), UTF-8 name, data length (u64), data.
// Uncompressed on purpose: the payload is mostly libomt.dll, and an
// uncompressed reader is a few lines that cannot go subtly wrong.
#pragma once
#include <stdint.h>

#define OMTMINI_PKG_MAGIC "OMTMPKG1"
#define OMTMINI_PKG_MAGIC_LEN 8

#pragma pack(push, 1)
typedef struct OmtMiniPkgFooter {
    char     magic[OMTMINI_PKG_MAGIC_LEN];
    uint32_t entry_count;
    uint64_t payload_offset;   // byte offset of the first entry
} OmtMiniPkgFooter;
#pragma pack(pop)
