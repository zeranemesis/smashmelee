#include "harness.hpp"

#include <melee/sysdolphin/baselib/class.h>
#include <melee/sysdolphin/baselib/object.h>

// hsdClass/hsdObj are process-global and initialize exactly once, so the
// lifetime counters accumulate across cases.  Every assertion below is
// therefore a delta against the state observed on entry.

MELEE_TEST(Class, TracksInheritanceAndLifetimeCounters)
{
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);
    const uint32_t existing = hsdObj.head.nb_exist;

    auto* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
    REQUIRE(object != nullptr);
    CHECK(hsdObjIsDescendantOf(object, &hsdClass));
    CHECK(hsdObjIsDescendantOf(object, &hsdObj));
    CHECK(hsdSearchClassInfo("hsd_obj") == &hsdObj);
    CHECK(hsdSearchClassInfo("hsd_class") == &hsdClass);
    CHECK(hsdSearchClassInfo("not_a_class") == nullptr);
    CHECK_EQ(hsdObj.head.nb_exist, existing + 1);
    CHECK(hsdObj.head.nb_peak >= hsdObj.head.nb_exist);

    const uint32_t peak = hsdObj.head.nb_peak;
    hsdDelete(object);
    CHECK_EQ(hsdObj.head.nb_exist, existing);
    // The peak is a high-water mark and must survive the delete.
    CHECK_EQ(hsdObj.head.nb_peak, peak);
}

MELEE_TEST(Class, CountsReferencesIndependently)
{
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);

    auto* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
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

    // The individual reference counter is tracked separately from the shared
    // one, as Melee relies on for per-owner joint/animation references.
    iref_INC(object);
    CHECK_EQ(iref_CNT(object), 1);
    CHECK(iref_DEC(object));
    CHECK_EQ(iref_CNT(object), 0);

    hsdDelete(object);
}

MELEE_TEST(Class, RejectsAnUnrelatedClassAsAParent)
{
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);

    // A standalone class info with no parent is never linked into the global
    // tree, so it cannot be reported as an ancestor of an HSD_Obj.
    HSD_ClassInfo unrelated{};
    hsdInitClassInfo(&unrelated, nullptr, "melee_port_tests", "unrelated",
                     sizeof(HSD_ClassInfo), sizeof(HSD_Class));

    auto* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
    REQUIRE(object != nullptr);
    CHECK(!hsdObjIsDescendantOf(object, &unrelated));
    CHECK(!hsdObjIsDescendantOf(nullptr, &hsdClass));
    hsdDelete(object);
}
