#pragma once

#include <melee/sysdolphin/baselib/objalloc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HSD_SList {
    struct HSD_SList* next;
    void* data;
} HSD_SList;

typedef struct HSD_DList {
    struct HSD_DList* next;
    struct HSD_DList* prev;
    void* data;
} HSD_DList;

void HSD_ListInitAllocData(void);
HSD_ObjAllocData* HSD_SListGetAllocData(void);
HSD_ObjAllocData* HSD_DListGetAllocData(void);
HSD_SList* HSD_SListAlloc(void);
HSD_SList* HSD_SListAllocAndAppend(HSD_SList* list, void* data);
HSD_SList* HSD_SListAllocAndPrepend(HSD_SList* list, void* data);
HSD_SList* HSD_SListAppendList(HSD_SList* list, HSD_SList* next);
HSD_SList* HSD_SListPrependList(HSD_SList* list, HSD_SList* previous);
HSD_SList* HSD_SListRemove(HSD_SList* list);

#ifdef __cplusplus
}
#endif
