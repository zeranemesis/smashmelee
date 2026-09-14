#include "harness.hpp"

#include <melee/sysdolphin/baselib/gobj.h>

#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t>* gOrder = nullptr;

struct Probe {
    uint8_t value = 0;
};

void record(HSD_GObj* gobj)
{
    auto* probe = static_cast<Probe*>(HSD_GObjGetUserData(gobj));
    gOrder->push_back(probe->value);
}

HSD_GObj* gSelfDestruct = nullptr;

void destroy_self(HSD_GObj* gobj)
{
    record(gobj);
    // Destroy the target once; the object is freed after this frame's sweep.
    if (gSelfDestruct != nullptr) {
        HSD_GObjDestroy(gSelfDestruct);
        gSelfDestruct = nullptr;
    }
}

} // namespace

MELEE_TEST(GObj, RunsProcessesInPriorityOrder)
{
    std::vector<uint8_t> order;
    gOrder = &order;
    Probe late{ 2 };
    Probe early{ 1 };

    HSD_GObjInit();
    HSD_GObj* later = GObj_Create(2, 0, 20);
    HSD_GObj* earlier = GObj_Create(1, 0, 10);
    REQUIRE(later != nullptr);
    REQUIRE(earlier != nullptr);
    REQUIRE(HSD_GObj_SetupProc(later, record, 0) != nullptr);
    REQUIRE(HSD_GObj_SetupProc(earlier, record, 0) != nullptr);
    later->user_data = &late;
    earlier->user_data = &early;

    HSD_GObjThink();
    CHECK_EQ(order.size(), 2U);
    REQUIRE(order.size() == 2);
    CHECK_EQ(order[0], 1);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(HSD_GObjGetFrameCount(), 1U);

    HSD_GObjShutdown();
    gOrder = nullptr;
}

MELEE_TEST(GObj, RunsLowerProcessPriorityLinksFirst)
{
    std::vector<uint8_t> order;
    gOrder = &order;
    Probe first{ 1 };
    Probe second{ 2 };

    HSD_GObjInit();
    // The object registered later in the p-link list still runs first when its
    // process link has the lower priority, because HSD_GObjThink sweeps every
    // object once per s_link level.
    HSD_GObj* late_object = GObj_Create(1, 0, 10);
    HSD_GObj* early_object = GObj_Create(2, 0, 20);
    REQUIRE(late_object != nullptr);
    REQUIRE(early_object != nullptr);
    late_object->user_data = &second;
    early_object->user_data = &first;
    REQUIRE(HSD_GObj_SetupProc(late_object, record, 2) != nullptr);
    REQUIRE(HSD_GObj_SetupProc(early_object, record, 0) != nullptr);

    HSD_GObjThink();
    REQUIRE(order.size() == 2);
    CHECK_EQ(order[0], 1);
    CHECK_EQ(order[1], 2);

    HSD_GObjShutdown();
    gOrder = nullptr;
}

MELEE_TEST(GObj, DefersDestructionRaisedDuringThink)
{
    std::vector<uint8_t> order;
    gOrder = &order;
    Probe destroyer{ 1 };
    Probe victim{ 2 };

    HSD_GObjInit();
    HSD_GObj* first = GObj_Create(1, 0, 10);
    HSD_GObj* second = GObj_Create(2, 0, 20);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    first->user_data = &destroyer;
    second->user_data = &victim;
    gSelfDestruct = second;
    REQUIRE(HSD_GObj_SetupProc(first, destroy_self, 0) != nullptr);
    REQUIRE(HSD_GObj_SetupProc(second, record, 0) != nullptr);

    // The victim is unlinked only after the sweep finishes, so this frame must
    // not run its process and must not corrupt the list being walked.
    HSD_GObjThink();
    REQUIRE(order.size() == 1);
    CHECK_EQ(order[0], 1);

    order.clear();
    HSD_GObjThink();
    REQUIRE(order.size() == 1);
    CHECK_EQ(order[0], 1);
    CHECK_EQ(HSD_GObjGetFrameCount(), 2U);

    gSelfDestruct = nullptr;
    HSD_GObjShutdown();
    gOrder = nullptr;
}

MELEE_TEST(GObj, RejectsOutOfRangeLinksAndPriorities)
{
    HSD_GObjInit();
    CHECK(GObj_Create(0, 64, 0) == nullptr);
    HSD_GObj* gobj = GObj_Create(0, 0, 0);
    REQUIRE(gobj != nullptr);
    CHECK(HSD_GObj_SetupProc(gobj, nullptr, 0) == nullptr);
    CHECK(HSD_GObj_SetupProc(gobj, record, 3) == nullptr);
    HSD_GObjShutdown();
}

MELEE_TEST(GObj, IgnoresWorkBeforeInitialization)
{
    HSD_GObjShutdown();
    const uint64_t frames = HSD_GObjGetFrameCount();
    CHECK(GObj_Create(0, 0, 0) == nullptr);
    HSD_GObjThink();
    CHECK_EQ(HSD_GObjGetFrameCount(), frames);
}
