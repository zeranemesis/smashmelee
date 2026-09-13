#include "harness.hpp"

// The first upstream translation unit compiled and driven by this repository.
//
// Phase 2 of docs/PLAN.md replaces the hand-written HSD with upstream's, one
// unit at a time, and this is the acceptance test for the first of them: the
// properties asserted here are the same ones tests/hsd/test_objalloc.cpp
// asserts of the port's allocator.  The two APIs are not identical -- the port
// added HSD_ObjAllocShutdown so a host process can return pool memory, and
// upstream takes its pools from an arena set once by HSD_ObjSetHeap -- so the
// behavior is compared, not the signatures.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/objalloc.h>
}

namespace {

// Upstream declares its pools as zero-initialized globals, and
// HSD_ObjAllocInit walks the free list before clearing the record, so a pool
// must start zeroed.
HSD_ObjAllocData gPool;

// HSD_ObjSetHeap is deliberately never called.
//
// objheap stores its arena as four u32 fields, so on a 64-bit host
// `obj_heap.curr = (u32) ptr` truncates the address and the first allocation
// dereferences a bogus pointer.  Leaving the arena unset takes the other path
// in HSD_ObjAllocAddFree, which calls HSD_MemAlloc and keeps a real void*.
// That is the path a host build has to use; see docs/PLAN.md, phase 2.
void reset_pool() { std::memset(&gPool, 0, sizeof(gPool)); }

} // namespace

MELEE_TEST(UpstreamObjAlloc, HonorsAlignmentAndNumberLimit)
{
    reset_pool();
    HSD_ObjAllocInit(&gPool, 48, 32);
    HSD_ObjAllocSetNumLimit(&gPool, 2);
    HSD_ObjAllocEnableNumLimit(&gPool);

    void* first = HSD_ObjAlloc(&gPool);
    void* second = HSD_ObjAlloc(&gPool);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(first) % 32, 0U);
    CHECK_EQ(reinterpret_cast<uintptr_t>(second) % 32, 0U);
    CHECK(HSD_ObjAlloc(&gPool) == nullptr);
    CHECK_EQ(HSD_ObjAllocGetUsing(&gPool), 2U);
    CHECK_EQ(HSD_ObjAllocGetPeak(&gPool), 2U);

    HSD_ObjFree(&gPool, first);
    HSD_ObjFree(&gPool, second);
}

MELEE_TEST(UpstreamObjAlloc, ReusesFreedObjects)
{
    reset_pool();
    HSD_ObjAllocInit(&gPool, 48, 32);
    HSD_ObjAllocSetNumLimit(&gPool, 2);
    HSD_ObjAllocEnableNumLimit(&gPool);

    void* first = HSD_ObjAlloc(&gPool);
    void* second = HSD_ObjAlloc(&gPool);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    HSD_ObjFree(&gPool, first);
    // The free list hands the most recently released object back first, so an
    // object-limited pool never grows past its limit on a recycle.
    CHECK(HSD_ObjAlloc(&gPool) == first);
    CHECK_EQ(HSD_ObjAllocGetUsing(&gPool), 2U);
    CHECK_EQ(HSD_ObjAllocGetPeak(&gPool), 2U);

    HSD_ObjFree(&gPool, first);
    HSD_ObjFree(&gPool, second);
    CHECK_EQ(HSD_ObjAllocGetUsing(&gPool), 0U);
    CHECK_EQ(HSD_ObjAllocGetFreed(&gPool), 2U);
}

MELEE_TEST(UpstreamObjAlloc, GrowsWithoutANumberLimit)
{
    reset_pool();
    HSD_ObjAllocInit(&gPool, 48, 32);

    constexpr uint32_t kCount = 256;
    void* objects[kCount]{};
    for (uint32_t index = 0; index < kCount; ++index) {
        objects[index] = HSD_ObjAlloc(&gPool);
        REQUIRE(objects[index] != nullptr);
    }
    CHECK_EQ(HSD_ObjAllocGetUsing(&gPool), kCount);
    CHECK_EQ(HSD_ObjAllocGetPeak(&gPool), kCount);
    for (uint32_t index = 0; index < kCount; ++index) {
        HSD_ObjFree(&gPool, objects[index]);
    }
    CHECK_EQ(HSD_ObjAllocGetUsing(&gPool), 0U);
}

MELEE_TEST(UpstreamObjAlloc, RoundsObjectSizeToTheAlignment)
{
    // data->size is rounded up to the alignment mask, which is what makes
    // every object in a pool land on an aligned boundary.  The slots are not
    // contiguous, though: HSD_ObjAlloc refills one object at a time
    // (HSD_ObjAllocAddFree(data, 1)), so each one is its own allocation.
    reset_pool();
    HSD_ObjAllocInit(&gPool, 48, 32);
    CHECK_EQ(gPool.size, 64U);
    CHECK_EQ(gPool.align, 31U);

    reset_pool();
    HSD_ObjAllocInit(&gPool, 64, 32);
    CHECK_EQ(gPool.size, 64U);

    reset_pool();
    HSD_ObjAllocInit(&gPool, 12, 4);
    CHECK_EQ(gPool.size, 12U);
    CHECK_EQ(gPool.align, 3U);
}
