#pragma once

#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/object.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AOBJ_REWINDED = 1U << 26,
    AOBJ_FIRST_PLAY = 1U << 27,
    AOBJ_NO_UPDATE = 1U << 28,
    AOBJ_LOOP = 1U << 29,
    AOBJ_NO_ANIM = 1U << 30,
};

typedef struct HSD_AObj {
    uint32_t flags;
    float curr_frame;
    float rewind_frame;
    float end_frame;
    float framerate;
    HSD_FObj* fobj;
    HSD_Obj* hsd_obj;
} HSD_AObj;

// This host descriptor is intentionally only valid once a GameCube object
// reference has been resolved by the archive/object bridge.  obj_id==0 means
// no target object and is safe for the current native animation subset.
typedef struct HSD_AObjDesc {
    uint32_t flags;
    float end_frame;
    HSD_FObjDesc* fobjdesc;
    uint32_t obj_id;
} HSD_AObjDesc;

void HSD_AObjInitAllocData(void);
HSD_ObjAllocData* HSD_AObjGetAllocData(void);
HSD_AObj* HSD_AObjAlloc(void);
void HSD_AObjFree(HSD_AObj* aobj);
void HSD_AObjRemove(HSD_AObj* aobj);
uint32_t HSD_AObjGetFlags(HSD_AObj* aobj);
void HSD_AObjSetFlags(HSD_AObj* aobj, uint32_t flags);
void HSD_AObjClearFlags(HSD_AObj* aobj, uint32_t flags);
void HSD_AObjSetFObj(HSD_AObj* aobj, HSD_FObj* fobj);
void HSD_AObjReqAnim(HSD_AObj* aobj, float frame);
void HSD_AObjSetRate(HSD_AObj* aobj, float rate);
void HSD_AObjSetRewindFrame(HSD_AObj* aobj, float frame);
void HSD_AObjSetEndFrame(HSD_AObj* aobj, float frame);
void HSD_AObjSetCurrentFrame(HSD_AObj* aobj, float frame);
HSD_AObj* HSD_AObjLoadDesc(HSD_AObjDesc* description);
void HSD_AObjForgetMemory(void);

#ifdef __cplusplus
}
#endif
