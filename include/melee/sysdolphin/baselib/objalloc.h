#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HSD_ObjAllocLink {
    struct HSD_ObjAllocLink* next;
} HSD_ObjAllocLink;

typedef struct HSD_HostAllocBlock HSD_HostAllocBlock;

// Host-width version of Melee's HSD object-pool descriptor. The original
// GameCube layout contains 32-bit addresses and must not be used verbatim on
// a 64-bit host.
typedef struct HSD_ObjAllocData {
    uint32_t num_limit_flag;
    uint32_t heap_limit_flag;
    HSD_ObjAllocLink* freehead;
    uint32_t used;
    uint32_t free;
    uint32_t peak;
    uint32_t num_limit;
    size_t heap_limit_size;
    uint32_t heap_limit_num;
    size_t size;
    size_t align;
    struct HSD_ObjAllocData* next;
    HSD_HostAllocBlock* host_blocks;
} HSD_ObjAllocData;

static inline uint32_t HSD_ObjAllocGetUsing(const HSD_ObjAllocData* data)
{
    return data != NULL ? data->used : 0;
}

static inline uint32_t HSD_ObjAllocGetFreed(const HSD_ObjAllocData* data)
{
    return data != NULL ? data->free : 0;
}

static inline uint32_t HSD_ObjAllocGetPeak(const HSD_ObjAllocData* data)
{
    return data != NULL ? data->peak : 0;
}

static inline void HSD_ObjAllocSetNumLimit(HSD_ObjAllocData* data,
                                            uint32_t num_limit)
{
    if (data != NULL) {
        data->num_limit = num_limit;
    }
}

static inline void HSD_ObjAllocEnableNumLimit(HSD_ObjAllocData* data)
{
    if (data != NULL) {
        data->num_limit_flag = 1;
    }
}

static inline void HSD_ObjAllocDisableNumLimit(HSD_ObjAllocData* data)
{
    if (data != NULL) {
        data->num_limit_flag = 0;
    }
}

void HSD_ObjSetHeap(size_t size, void* ptr);
int32_t HSD_ObjAllocAddFree(HSD_ObjAllocData* data, uint32_t num);
void* HSD_ObjAlloc(HSD_ObjAllocData* data);
void HSD_ObjFree(HSD_ObjAllocData* data, void* obj);
void HSD_ObjAllocInit(HSD_ObjAllocData* data, size_t size, size_t align);
void HSD_ObjAllocShutdown(HSD_ObjAllocData* data);
void _HSD_ObjAllocForgetMemory(void* low, void* high);

#ifdef __cplusplus
}
#endif
