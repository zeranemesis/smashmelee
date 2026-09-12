#include <melee/sysdolphin/baselib/list.h>

#include <cstring>

namespace {

HSD_ObjAllocData gSListAllocator{};
HSD_ObjAllocData gDListAllocator{};

} // namespace

extern "C" void HSD_ListInitAllocData(void)
{
    HSD_ObjAllocInit(&gSListAllocator, sizeof(HSD_SList), alignof(HSD_SList));
    HSD_ObjAllocInit(&gDListAllocator, sizeof(HSD_DList), alignof(HSD_DList));
}

extern "C" HSD_ObjAllocData* HSD_SListGetAllocData(void)
{
    return &gSListAllocator;
}

extern "C" HSD_ObjAllocData* HSD_DListGetAllocData(void)
{
    return &gDListAllocator;
}

extern "C" HSD_SList* HSD_SListAlloc(void)
{
    auto* list = static_cast<HSD_SList*>(HSD_ObjAlloc(&gSListAllocator));
    if (list != nullptr) {
        std::memset(list, 0, sizeof(*list));
    }
    return list;
}

extern "C" HSD_SList* HSD_SListAppendList(HSD_SList* list, HSD_SList* next)
{
    if (next == nullptr) {
        return list;
    }
    if (list != nullptr) {
        next->next = list->next;
        list->next = next;
        return list;
    }
    next->next = nullptr;
    return next;
}

extern "C" HSD_SList* HSD_SListPrependList(HSD_SList* list,
                                              HSD_SList* previous)
{
    if (previous == nullptr) {
        return list;
    }
    previous->next = list;
    return previous;
}

extern "C" HSD_SList* HSD_SListAllocAndAppend(HSD_SList* list, void* data)
{
    HSD_SList* entry = HSD_SListAlloc();
    if (entry == nullptr) {
        return list;
    }
    entry->data = data;
    return HSD_SListAppendList(list, entry);
}

extern "C" HSD_SList* HSD_SListAllocAndPrepend(HSD_SList* list, void* data)
{
    HSD_SList* entry = HSD_SListAlloc();
    if (entry == nullptr) {
        return list;
    }
    entry->data = data;
    return HSD_SListPrependList(list, entry);
}

extern "C" HSD_SList* HSD_SListRemove(HSD_SList* list)
{
    if (list == nullptr) {
        return nullptr;
    }
    HSD_SList* next = list->next;
    HSD_ObjFree(&gSListAllocator, list);
    return next;
}
