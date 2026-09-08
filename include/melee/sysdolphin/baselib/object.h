#pragma once

#include <melee/sysdolphin/baselib/class.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HSD_OBJ_NOREF UINT16_MAX

typedef struct HSD_Obj {
    HSD_Class parent;
    uint16_t ref_count;
    uint16_t ref_count_individual;
} HSD_Obj;

typedef struct HSD_ObjInfo {
    HSD_ClassInfo parent;
} HSD_ObjInfo;

extern HSD_ClassInfo hsdObj;

void ObjInfoInit(void);
bool hsdObjIsDescendantOf(const HSD_Obj* object, HSD_ClassInfo* parent);

static inline bool ref_DEC(void* object)
{
    HSD_Obj* hsd_object = (HSD_Obj*) object;
    if (hsd_object == NULL || hsd_object->ref_count == HSD_OBJ_NOREF) {
        return true;
    }
    if (hsd_object->ref_count == 0) {
        return true;
    }
    --hsd_object->ref_count;
    return hsd_object->ref_count == 0;
}

static inline void ref_INC(void* object)
{
    HSD_Obj* hsd_object = (HSD_Obj*) object;
    if (hsd_object != NULL && hsd_object->ref_count != HSD_OBJ_NOREF &&
        hsd_object->ref_count < UINT16_MAX - 1) {
        ++hsd_object->ref_count;
    }
}

static inline int ref_CNT(const void* object)
{
    const HSD_Obj* hsd_object = (const HSD_Obj*) object;
    if (hsd_object == NULL || hsd_object->ref_count == HSD_OBJ_NOREF) {
        return -1;
    }
    return hsd_object->ref_count;
}

static inline int iref_CNT(const void* object)
{
    const HSD_Obj* hsd_object = (const HSD_Obj*) object;
    return hsd_object != NULL ? hsd_object->ref_count_individual : 0;
}

static inline bool iref_DEC(void* object)
{
    HSD_Obj* hsd_object = (HSD_Obj*) object;
    if (hsd_object == NULL || hsd_object->ref_count_individual == 0) {
        return true;
    }
    --hsd_object->ref_count_individual;
    return hsd_object->ref_count_individual == 0;
}

static inline void iref_INC(void* object)
{
    HSD_Obj* hsd_object = (HSD_Obj*) object;
    if (hsd_object != NULL && hsd_object->ref_count_individual < UINT16_MAX) {
        ++hsd_object->ref_count_individual;
    }
}

#ifdef __cplusplus
}
#endif
