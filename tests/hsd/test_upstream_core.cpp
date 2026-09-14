#include "harness.hpp"

// Upstream's class model, collections and ID table, held to the assertions the
// port's own units answer in test_class.cpp and test_collections.cpp.
//
// All four of these units are pointer-clean -- they appear nowhere in
// `upstream_native_spike.py --pointer-casts` -- which is why they are the
// first group to move after the allocator.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/class.h>
#include <sysdolphin/baselib/id.h>
#include <sysdolphin/baselib/list.h>
#include <sysdolphin/baselib/object.h>
#include <sysdolphin/baselib/objalloc.h>
}

// hsdClass and hsdObj are process-global and initialize once, so their
// lifetime counters accumulate across cases; every assertion below is a delta.

MELEE_TEST(UpstreamClass, TracksInheritanceAndLifetimeCounters)
{
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);
    const u32 existing = hsdObj.head.nb_exist;

    HSD_Obj* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
    REQUIRE(object != nullptr);
    CHECK(hsdIsDescendantOf(object->parent.class_info, &hsdClass));
    CHECK(hsdIsDescendantOf(object->parent.class_info, &hsdObj));
    // Upstream's hsdSearchClassInfo reads a hash that nothing in the
    // decompilation populates, so it always answers NULL.  The port's
    // tree-walking version is a host addition, not console behavior.
    CHECK(hsdSearchClassInfo("hsd_obj") == nullptr);
    CHECK(hsdSearchClassInfo("not_a_class") == nullptr);
    CHECK_EQ(hsdObj.head.nb_exist, existing + 1);
    CHECK(hsdObj.head.nb_peak >= hsdObj.head.nb_exist);

    const u32 peak = hsdObj.head.nb_peak;
    hsdDelete(object);
    CHECK_EQ(hsdObj.head.nb_exist, existing);
    // The peak is a high-water mark and survives the delete.
    CHECK_EQ(hsdObj.head.nb_peak, peak);
}

MELEE_TEST(UpstreamClass, CountsReferencesIndependently)
{
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);

    HSD_Obj* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
    REQUIRE(object != nullptr);
    // ref_count holds the references beyond the first: a fresh object sits at
    // zero with one owner, so the first release already reports true.  These
    // are the values sysdolphin/baselib/object.h produces.
    CHECK_EQ(ref_CNT(object), 0);
    ref_INC(object);
    ref_INC(object);
    CHECK_EQ(ref_CNT(object), 2);
    CHECK(!ref_DEC(object));
    CHECK_EQ(ref_CNT(object), 1);
    CHECK(!ref_DEC(object));
    CHECK_EQ(ref_CNT(object), 0);
    // The count was already zero, so this is the last release, and the counter
    // wraps past it to HSD_OBJ_NOREF.
    CHECK(ref_DEC(object));
    CHECK_EQ(ref_CNT(object), -1);
    // Every further release reports true without touching the counter.
    CHECK(ref_DEC(object));
    CHECK_EQ(ref_CNT(object), -1);

    // The individual counter is tracked separately from the shared one.
    iref_INC(object);
    CHECK_EQ(iref_CNT(object), 1);
    CHECK(iref_DEC(object));
    CHECK_EQ(iref_CNT(object), 0);

    hsdDelete(object);
}

MELEE_TEST(UpstreamClass, RejectsAnUnrelatedClassAsAParent)
{
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);

    // A standalone class info with no parent is never linked into the global
    // tree, so it cannot be reported as an ancestor of an HSD_Obj.
    HSD_ClassInfo unrelated;
    std::memset(&unrelated, 0, sizeof(unrelated));
    hsdInitClassInfo(&unrelated, nullptr, "melee_port_tests", "unrelated",
                     sizeof(HSD_ClassInfo), sizeof(HSD_Class));

    HSD_Obj* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
    REQUIRE(object != nullptr);
    CHECK(!hsdIsDescendantOf(object->parent.class_info, &unrelated));
    CHECK(!hsdIsDescendantOf(nullptr, &hsdClass));
    hsdDelete(object);
}

MELEE_TEST(UpstreamList, InsertsAnAppendedEntryAfterTheHead)
{
    HSD_ListInitAllocData();
    int first = 1;
    int second = 2;
    int third = 3;

    // HSD_SListAppendList splices the new entry in right after the head rather
    // than walking to the tail, so the visiting order of a list built with
    // repeated appends is head, newest, oldest.
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

MELEE_TEST(UpstreamList, PrependsAheadOfTheHead)
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

MELEE_TEST(UpstreamIDTable, ResolvesCollidingIdentifiers)
{
    HSD_IDInitAllocData();
    HSD_IDSetup();
    int first = 1;
    int second = 2;

    // 42 and 143 land in the same bucket of the 101-entry table, so this also
    // covers the per-bucket chain walk.
    HSD_IDInsertToTable(nullptr, 42, &first);
    HSD_IDInsertToTable(nullptr, 143, &second);

    s32 found = 0;
    CHECK(HSD_IDGetData(42, &found) == &first);
    CHECK_EQ(found, 1);
    CHECK(HSD_IDGetData(143, &found) == &second);
    CHECK_EQ(found, 1);
    CHECK(HSD_IDGetData(7, &found) == nullptr);
    CHECK_EQ(found, 0);

    HSD_IDRemoveByIDFromTable(nullptr, 42);
    CHECK(HSD_IDGetData(42, &found) == nullptr);
    CHECK_EQ(found, 0);
    // Removing one entry leaves the rest of its bucket reachable.
    CHECK(HSD_IDGetData(143, &found) == &second);
    CHECK_EQ(found, 1);
}

MELEE_TEST(UpstreamIDTable, CannotTellApartTwoPointersSharingALowWord)
{
    // Why the descriptor address is the wrong key on a 64-bit host.
    //
    // HSD_JObjLoadJoint registers a joint with HSD_IDInsertToTable(NULL,
    // (u32) joint, jobj), and five places look one back up the same way --
    // jobj.c for a child, pobj.c for a rigid primitive's joint and for each
    // envelope weight, robj.c for a constraint's target and for an rvalue.
    // The key is the low word of a host pointer.
    //
    // Two descriptors whose addresses differ only above bit 31 therefore
    // share a key, and the table answers with whichever was registered.  The
    // symptom would not be a crash: it would be a constraint or a skinning
    // weight quietly following the wrong bone.
    //
    // The two values below are never dereferenced.  They stand for two host
    // allocations four gigabytes apart, which is what a long-lived process
    // with a spread-out heap eventually produces.
    const auto first = reinterpret_cast<void*>(std::uintptr_t{ 0x0000000100002000ULL });
    const auto second = reinterpret_cast<void*>(std::uintptr_t{ 0x0000000200002000ULL });
    REQUIRE(first != second);

    int first_object = 1;
    int second_object = 2;

    HSD_IDInsertToTable(nullptr, static_cast<u32>(
                                     reinterpret_cast<std::uintptr_t>(first)),
                        &first_object);

    // Looking up the *second* descriptor finds the first one's object.
    s32 found = 0;
    void* answer = HSD_IDGetDataFromTable(
        nullptr, static_cast<u32>(reinterpret_cast<std::uintptr_t>(second)),
        &found);
    CHECK_EQ(found, 1);
    CHECK(answer == &first_object);
    CHECK(answer != &second_object);

    // And registering the second silently replaces the first, so the tree
    // that was already built now resolves to the newcomer.
    HSD_IDInsertToTable(nullptr,
                        static_cast<u32>(
                            reinterpret_cast<std::uintptr_t>(second)),
                        &second_object);
    answer = HSD_IDGetDataFromTable(
        nullptr, static_cast<u32>(reinterpret_cast<std::uintptr_t>(first)),
        nullptr);
    CHECK(answer == &second_object);
}
