#pragma once

#include <dolphin/mtx.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#ifdef __cplusplus
extern "C" {
#endif

void* HSD_VecAlloc(void);
void HSD_VecFree(void* value);
void* HSD_MtxAlloc(void);
void HSD_MtxFree(void* value);
HSD_ObjAllocData* HSD_VecGetAllocData(void);
void HSD_VecInitAllocData(void);
HSD_ObjAllocData* HSD_MtxGetAllocData(void);
void HSD_MtxInitAllocData(void);

#ifdef __cplusplus
}
#endif
