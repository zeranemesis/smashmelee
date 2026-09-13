#include "harness.hpp"

#include <melee/sysdolphin/baselib/id.h>
#include <melee/sysdolphin/baselib/list.h>
#include <melee/sysdolphin/baselib/mtx.h>
#include <melee/sysdolphin/baselib/objalloc.h>

MELEE_TEST(List, AppendsAndPrependsThroughThePool)
{
    HSD_ListInitAllocData();
    int first = 1;
    int second = 2;

    HSD_SList* list = HSD_SListAllocAndAppend(nullptr, &first);
    REQUIRE(list != nullptr);
    list = HSD_SListAllocAndPrepend(list, &second);
    REQUIRE(list != nullptr);
    CHECK(list->data == &second);
    REQUIRE(list->next != nullptr);
    CHECK(list->next->data == &first);
    CHECK(list->next->next == nullptr);

    list = HSD_SListRemove(list);
    REQUIRE(list != nullptr);
    CHECK(list->data == &first);
    list = HSD_SListRemove(list);
    CHECK(list == nullptr);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_SListGetAllocData()), 0U);
}

MELEE_TEST(List, InsertsAnAppendedEntryAfterTheHead)
{
    HSD_ListInitAllocData();
    int first = 1;
    int second = 2;
    int third = 3;

    // Upstream HSD_SListAppendList splices the new entry in right after the
    // head rather than walking to the tail, so the visiting order of a list
    // built with repeated appends is head, newest, oldest.
    HSD_SList* list = HSD_SListAllocAndAppend(nullptr, &first);
    REQUIRE(list != nullptr);
    CHECK(HSD_SListAllocAndAppend(list, &second) == list);
    CHECK(HSD_SListAllocAndAppend(list, &third) == list);
    REQUIRE(list->next != nullptr);
    REQUIRE(list->next->next != nullptr);
    CHECK(list->data == &first);
    CHECK(list->next->data == &third);
    CHECK(list->next->next->data == &second);
    CHECK(list->next->next->next == nullptr);

    while (list != nullptr) {
        list = HSD_SListRemove(list);
    }
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_SListGetAllocData()), 0U);
}

MELEE_TEST(IDTable, ResolvesCollidingIdentifiers)
{
    HSD_IDInitAllocData();
    HSD_IDSetup();
    int first = 1;
    int second = 2;

    // 42 and 143 hash to the same bucket in the 101-entry table, so this also
    // covers the per-bucket chain walk.
    HSD_IDInsertToTable(nullptr, 42, &first);
    HSD_IDInsertToTable(nullptr, 143, &second);

    int32_t found = 0;
    CHECK(HSD_IDGetData(42, &found) == &first);
    CHECK_EQ(found, 1);
    CHECK(HSD_IDGetData(143, &found) == &second);
    CHECK_EQ(found, 1);
    CHECK(HSD_IDGetData(7, &found) == nullptr);
    CHECK_EQ(found, 0);

    HSD_IDRemoveByIDFromTable(nullptr, 42);
    CHECK(HSD_IDGetData(42, &found) == nullptr);
    CHECK_EQ(found, 0);
    // Removing one entry must leave the rest of its bucket reachable.
    CHECK(HSD_IDGetData(143, &found) == &second);
    CHECK_EQ(found, 1);

    HSD_IDForgetMemory();
}

MELEE_TEST(MathPools, AllocatesAndReleasesVectorsAndMatrices)
{
    HSD_VecInitAllocData();
    HSD_MtxInitAllocData();

    void* vector = HSD_VecAlloc();
    void* matrix = HSD_MtxAlloc();
    REQUIRE(vector != nullptr);
    REQUIRE(matrix != nullptr);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_VecGetAllocData()), 1U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()), 1U);

    HSD_VecFree(vector);
    HSD_MtxFree(matrix);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_VecGetAllocData()), 0U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()), 0U);
}
