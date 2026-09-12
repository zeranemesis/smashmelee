#include <melee/sysdolphin/baselib/id.h>

#include <cstring>

namespace {

constexpr uint32_t kBucketCount = 101;
HSD_ObjAllocData gIdAllocator{};
HSD_IDTable gDefaultTable{};

uint32_t bucket(uint32_t id)
{
    return id % kBucketCount;
}

HSD_IDTable* resolve_table(HSD_IDTable* table)
{
    return table != nullptr ? table : &gDefaultTable;
}

} // namespace

extern "C" HSD_ObjAllocData* HSD_IDGetAllocData(void)
{
    return &gIdAllocator;
}

extern "C" void HSD_IDInitAllocData(void)
{
    HSD_ObjAllocInit(&gIdAllocator, sizeof(HSD_IDEntry), alignof(HSD_IDEntry));
}

extern "C" void HSD_IDSetup(void)
{
    std::memset(&gDefaultTable, 0, sizeof(gDefaultTable));
}

extern "C" void HSD_IDInsertToTable(HSD_IDTable* table, uint32_t id,
                                      void* data)
{
    HSD_IDTable* destination = resolve_table(table);
    HSD_IDEntry* entry = destination->table[bucket(id)];
    for (; entry != nullptr; entry = entry->next) {
        if (entry->id == id) {
            entry->data = data;
            return;
        }
    }
    entry = static_cast<HSD_IDEntry*>(HSD_ObjAlloc(&gIdAllocator));
    if (entry == nullptr) {
        return;
    }
    entry->id = id;
    entry->data = data;
    entry->next = destination->table[bucket(id)];
    destination->table[bucket(id)] = entry;
}

extern "C" void HSD_IDRemoveByIDFromTable(HSD_IDTable* table, uint32_t id)
{
    HSD_IDTable* destination = resolve_table(table);
    HSD_IDEntry** link = &destination->table[bucket(id)];
    while (*link != nullptr) {
        if ((*link)->id == id) {
            HSD_IDEntry* removed = *link;
            *link = removed->next;
            HSD_ObjFree(&gIdAllocator, removed);
            return;
        }
        link = &(*link)->next;
    }
}

extern "C" void* HSD_IDGetDataFromTable(HSD_IDTable* table, uint32_t id,
                                          int32_t* success)
{
    for (HSD_IDEntry* entry = resolve_table(table)->table[bucket(id)];
         entry != nullptr; entry = entry->next) {
        if (entry->id == id) {
            if (success != nullptr) {
                *success = 1;
            }
            return entry->data;
        }
    }
    if (success != nullptr) {
        *success = 0;
    }
    return nullptr;
}

extern "C" void* HSD_IDGetData(uint32_t id, int32_t* success)
{
    return HSD_IDGetDataFromTable(nullptr, id, success);
}

extern "C" void HSD_IDForgetMemory(void)
{
    HSD_IDSetup();
}
