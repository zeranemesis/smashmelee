#include "harness.hpp"

#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstddef>
#include <cstdint>

namespace {

struct alignas(32) ProbeObject {
    std::byte payload[48];
};

} // namespace

MELEE_TEST(ObjAlloc, HonorsAlignmentAndNumberLimit)
{
    HSD_ObjAllocData pool{};
    HSD_ObjAllocInit(&pool, sizeof(ProbeObject), alignof(ProbeObject));
    HSD_ObjAllocSetNumLimit(&pool, 2);
    HSD_ObjAllocEnableNumLimit(&pool);

    void* first = HSD_ObjAlloc(&pool);
    void* second = HSD_ObjAlloc(&pool);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(first) % alignof(ProbeObject), 0U);
    CHECK_EQ(reinterpret_cast<uintptr_t>(second) % alignof(ProbeObject), 0U);
    CHECK(HSD_ObjAlloc(&pool) == nullptr);
    CHECK_EQ(HSD_ObjAllocGetUsing(&pool), 2U);
    CHECK_EQ(HSD_ObjAllocGetPeak(&pool), 2U);

    HSD_ObjFree(&pool, first);
    HSD_ObjFree(&pool, second);
    HSD_ObjAllocShutdown(&pool);
}

MELEE_TEST(ObjAlloc, ReusesFreedObjects)
{
    HSD_ObjAllocData pool{};
    HSD_ObjAllocInit(&pool, sizeof(ProbeObject), alignof(ProbeObject));
    HSD_ObjAllocSetNumLimit(&pool, 2);
    HSD_ObjAllocEnableNumLimit(&pool);

    void* first = HSD_ObjAlloc(&pool);
    void* second = HSD_ObjAlloc(&pool);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    HSD_ObjFree(&pool, first);
    // The free list hands the most recently released object back first, so an
    // object-limited pool never grows past its limit on a recycle.
    CHECK(HSD_ObjAlloc(&pool) == first);
    CHECK_EQ(HSD_ObjAllocGetUsing(&pool), 2U);
    CHECK_EQ(HSD_ObjAllocGetPeak(&pool), 2U);

    HSD_ObjFree(&pool, first);
    HSD_ObjFree(&pool, second);
    CHECK_EQ(HSD_ObjAllocGetUsing(&pool), 0U);
    CHECK_EQ(HSD_ObjAllocGetFreed(&pool), 2U);
    HSD_ObjAllocShutdown(&pool);
}

MELEE_TEST(ObjAlloc, GrowsWithoutANumberLimit)
{
    HSD_ObjAllocData pool{};
    HSD_ObjAllocInit(&pool, sizeof(ProbeObject), alignof(ProbeObject));

    constexpr uint32_t kCount = 256;
    void* objects[kCount]{};
    for (uint32_t index = 0; index < kCount; ++index) {
        objects[index] = HSD_ObjAlloc(&pool);
        REQUIRE(objects[index] != nullptr);
    }
    CHECK_EQ(HSD_ObjAllocGetUsing(&pool), kCount);
    CHECK_EQ(HSD_ObjAllocGetPeak(&pool), kCount);
    for (uint32_t index = 0; index < kCount; ++index) {
        HSD_ObjFree(&pool, objects[index]);
    }
    CHECK_EQ(HSD_ObjAllocGetUsing(&pool), 0U);
    HSD_ObjAllocShutdown(&pool);
}
