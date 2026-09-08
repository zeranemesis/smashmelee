#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HSD_GObj HSD_GObj;
typedef struct HSD_GObjProc HSD_GObjProc;
typedef void (*HSD_GObjEvent)(HSD_GObj* gobj);

struct HSD_GObjProc {
    HSD_GObjProc* child;
    HSD_GObjProc* next;
    HSD_GObjProc* prev;
    uint8_t s_link;
    uint8_t pending_remove;
    HSD_GObj* gobj;
    HSD_GObjEvent on_invoke;
};

struct HSD_GObj {
    uint16_t classifier;
    uint8_t p_link;
    uint8_t gx_link;
    uint8_t p_priority;
    uint8_t render_priority;
    uint8_t obj_kind;
    uint8_t user_data_kind;
    HSD_GObj* next;
    HSD_GObj* prev;
    HSD_GObjProc* proc;
    void* hsd_obj;
    void* user_data;
    void (*user_data_remove_func)(void* data);
    uint8_t pending_destroy;
};

void HSD_GObjInit(void);
void HSD_GObjShutdown(void);
HSD_GObj* GObj_Create(uint16_t classifier, uint8_t p_link, uint8_t priority);
HSD_GObjProc* HSD_GObj_SetupProc(HSD_GObj* gobj, HSD_GObjEvent func,
                                 uint8_t priority);
void HSD_GObjProcRemove(HSD_GObjProc* proc);
void HSD_GObjDestroy(HSD_GObj* gobj);
void HSD_GObjThink(void);
uint64_t HSD_GObjGetFrameCount(void);

static inline void* HSD_GObjGetUserData(HSD_GObj* gobj)
{
    return gobj != NULL ? gobj->user_data : NULL;
}

static inline uint16_t HSD_GObjGetClassifier(HSD_GObj* gobj)
{
    return gobj != NULL ? gobj->classifier : 0;
}

#ifdef __cplusplus
}
#endif
