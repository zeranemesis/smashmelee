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

// ref_count holds the references beyond the first, so a freshly created
// object sits at zero with one owner.  ref_DEC reports the release when the
// count was already zero and lets the counter wrap to HSD_OBJ_NOREF on the way
// past, which is what makes every later release report true as well.  The NULL
// guard is a host addition; upstream dereferences unconditionally.
static inline bool ref_DEC(void* object)
{
    HSD_Obj* hsd_object = (HSD_Obj*) object;
    if (hsd_object == NULL || hsd_object->ref_count == HSD_OBJ_NOREF) {
        return true;
    }
    return hsd_object->ref_count-- == 0;
}

static inline void ref_INC(void* object)
{
    HSD_Obj* hsd_object = (HSD_Obj*) object;
    if (hsd_object != NULL) {
        // Upstream asserts here that the result is not HSD_OBJ_NOREF, which
        // would take 65,534 references to reach.
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
