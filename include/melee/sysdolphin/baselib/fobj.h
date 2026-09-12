#pragma once

#include <melee/sysdolphin/baselib/objalloc.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HSD_A_OP_NONE = 0,
    HSD_A_OP_CON = 1,
    HSD_A_OP_LIN = 2,
    HSD_A_OP_SPL0 = 3,
    HSD_A_OP_SPL = 4,
    HSD_A_OP_SLP = 5,
    HSD_A_OP_KEY = 6,
};

typedef struct HSD_FObj {
    struct HSD_FObj* next;
    uint8_t* ad;
    uint8_t* ad_head;
    uint32_t length;
    uint8_t flags;
    uint8_t op;
    uint8_t op_intrp;
    uint8_t obj_type;
    uint8_t frac_value;
    uint8_t frac_slope;
    uint16_t nb_pack;
    int16_t startframe;
    uint16_t fterm;
    float time;
    float p0;
    float p1;
    float d0;
    float d1;
} HSD_FObj;

typedef struct HSD_FObjDesc {
    struct HSD_FObjDesc* next;
    uint32_t length;
    float startframe;
    uint8_t type;
    uint8_t frac_value;
    uint8_t frac_slope;
    uint8_t padding;
    uint8_t* ad;
} HSD_FObjDesc;

HSD_ObjAllocData* HSD_FObjGetAllocData(void);
void HSD_FObjInitAllocData(void);
HSD_FObj* HSD_FObjAlloc(void);
void HSD_FObjFree(HSD_FObj* fobj);
void HSD_FObjRemove(HSD_FObj* fobj);
void HSD_FObjRemoveAll(HSD_FObj* fobj);
uint32_t HSD_FObjSetState(HSD_FObj* fobj, uint32_t state);
uint32_t HSD_FObjGetState(HSD_FObj* fobj);
void HSD_FObjReqAnimAll(HSD_FObj* fobj, float startframe);
HSD_FObj* HSD_FObjLoadDesc(HSD_FObjDesc* description);

#ifdef __cplusplus
}
#endif
