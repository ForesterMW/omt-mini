// Shared memory contract between OMT Mini and the virtual camera filter.
//
// The filter DLL is loaded into other processes (Zoom, Teams, OBS...), so this
// header is deliberately free standing: plain C types, no app headers, and a
// layout that must not change without bumping kVCamVersion.
#pragma once
#include <stdint.h>

#define VCAM_SHM_NAME     L"Local\\OMTMini.VCam.v1"
#define VCAM_MUTEX_NAME   L"Local\\OMTMini.VCam.v1.lock"
#define VCAM_MAGIC        0x314D544FU   // 'OMT1'
#define VCAM_VERSION      1
#define VCAM_BUFFERS      3
#define VCAM_MAX_WIDTH    1920
#define VCAM_MAX_HEIGHT   1080
#define VCAM_BYTES_PER_PX 4

// Frames are stored top-down BGRA. DirectShow RGB32 is bottom-up, so the
// filter flips on delivery; that keeps the producer side trivial.
#pragma pack(push, 8)
typedef struct VCamHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t buffer_count;
    uint32_t buffer_bytes;      // bytes of one frame, width*height*4
    uint32_t producer_pid;
    uint32_t active;            // non zero while a producer is publishing
    uint64_t sequence;          // incremented after each completed write
    uint32_t write_index;       // buffer most recently completed
    uint32_t reserved0;
    uint64_t heartbeat_ms;      // GetTickCount64 of the last write
    uint64_t reserved1[6];
} VCamHeader;
#pragma pack(pop)

#define VCAM_HEADER_BYTES   ((uint32_t)sizeof(VCamHeader))
#define VCAM_FRAME_BYTES    ((uint32_t)(VCAM_MAX_WIDTH * VCAM_MAX_HEIGHT * VCAM_BYTES_PER_PX))
#define VCAM_TOTAL_BYTES    (VCAM_HEADER_BYTES + VCAM_FRAME_BYTES * VCAM_BUFFERS)

// A producer is considered live if it wrote within this many milliseconds.
#define VCAM_STALE_MS       2000
