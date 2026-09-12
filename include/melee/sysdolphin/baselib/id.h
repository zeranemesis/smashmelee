#pragma once

#include <melee/sysdolphin/baselib/objalloc.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HSD_IDEntry {
    struct HSD_IDEntry* next;
    uint32_t id;
    void* data;
} HSD_IDEntry;

typedef struct HSD_IDTable {
    HSD_IDEntry* table[101];
} HSD_IDTable;

HSD_ObjAllocData* HSD_IDGetAllocData(void);
void HSD_IDInitAllocData(void);
void HSD_IDSetup(void);
void HSD_IDInsertToTable(HSD_IDTable* table, uint32_t id, void* data);
void HSD_IDRemoveByIDFromTable(HSD_IDTable* table, uint32_t id);
void* HSD_IDGetDataFromTable(HSD_IDTable* table, uint32_t id, int32_t* success);
void* HSD_IDGetData(uint32_t id, int32_t* success);
void HSD_IDForgetMemory(void);

#ifdef __cplusplus
}
#endif
