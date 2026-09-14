#include "harness.hpp"

// The OS arena and the heaps HSD carves out of it.
//
// src/melee_port/upstream/os_arena.hpp says why the conformance target has
// its own allocator rather than Aurora's.  These cases pin the contract the
// two share, because upstream's code is written against it and cannot tell
// which one it got: cells on 32-byte boundaries, first fit, coalescing on
// free, and an OSCheckHeap that reports real free space rather than zero.
//
// The last case is the point of the file: it runs upstream's own
// HSD_InitComponent, which is the console's boot sequence, against this
// arena.

#include <cstdint>
#include <vector>

#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/objalloc.h>
#include <sysdolphin/baselib/synth.h>
}

#include <string>

#include "gx_record.hpp"
#include "upstream_boot.hpp"
#include "os_arena.hpp"
#include "os_scheduler.hpp"

namespace os = meleeboard::os;

namespace {

// A small arena keeps the cases fast and makes exhaustion reachable: one
// mebibyte of physical memory, carved the way the console's is.
constexpr std::uint32_t kTestArenaBytes = 1u * 1024u * 1024u;

// Carves one heap over the whole of the current arena, the way HSD_OSInit
// does minus the audio split.  Returns the heap.
OSHeapHandle open_one_heap()
{
    void* lo = OSInitAlloc(OSGetArenaLo(), OSGetArenaHi(), 4);
    if (lo == nullptr) {
        return -1;
    }
    OSSetArenaLo(lo);
    return OSCreateHeap((void*) OSRoundUp32B(OSGetArenaLo()),
                        (void*) OSRoundDown32B(OSGetArenaHi()));
}

bool is_aligned_32(const void* pointer)
{
    return (reinterpret_cast<std::uintptr_t>(pointer) & 31u) == 0;
}

} // namespace

MELEE_TEST(UpstreamArena, PlacesTheArenaInsideOneSelfAlignedBlock)
{
    const os::ArenaScope arena(kTestArenaBytes);

    // The property the block exists for: no two addresses in it share a low
    // word, so upstream's truncated-pointer ID key stays lossless for every
    // object HSD allocates, not just for the joints DescriptorArena covers.
    CHECK(os::low_words_are_distinct());
    CHECK_EQ(os::arena_bytes(), kTestArenaBytes);

    // And the arena is not the whole of memory: the OS globals sit below it
    // and the boot stack above, exactly as on hardware.
    const auto* base = static_cast<const unsigned char*>(os::arena_base());
    CHECK_EQ(static_cast<const void*>(OSGetArenaLo()),
             static_cast<const void*>(base + os::kSystemReserveLowBytes));
    CHECK_EQ(static_cast<const void*>(OSGetArenaHi()),
             static_cast<const void*>(base + kTestArenaBytes -
                                      os::kSystemReserveHighBytes));

}

MELEE_TEST(UpstreamArena, CarvesTheHeapDescriptorsOutOfTheBottom)
{
    const os::ArenaScope arena(kTestArenaBytes);
    void* const original_lo = OSGetArenaLo();

    void* const new_lo = OSInitAlloc(original_lo, OSGetArenaHi(), 4);
    REQUIRE(new_lo != nullptr);

    // OSInitAlloc consumes arena, it does not move arenaLo itself: upstream's
    // HSD_OSInit takes the answer and calls OSSetArenaLo with it.
    CHECK(new_lo > original_lo);
    CHECK(is_aligned_32(new_lo));
    CHECK_EQ(OSGetArenaLo(), original_lo);

}

MELEE_TEST(UpstreamArena, RefusesAnArenaTooSmallForItsHeapDescriptors)
{
    const os::ArenaScope arena(kTestArenaBytes);
    auto* lo = static_cast<unsigned char*>(OSGetArenaLo());

    // Sixty-four bytes cannot hold four heap descriptors plus a cell.
    CHECK_EQ(OSInitAlloc(lo, lo + 64, 4), nullptr);
    // Neither can a backwards range, or no heaps at all.
    CHECK_EQ(OSInitAlloc(OSGetArenaHi(), OSGetArenaLo(), 4), nullptr);
    CHECK_EQ(OSInitAlloc(OSGetArenaLo(), OSGetArenaHi(), 0), nullptr);

}

MELEE_TEST(UpstreamArena, HandsBackAlignedBlocksAndTakesThemBack)
{
    const os::ArenaScope arena(kTestArenaBytes);
    const OSHeapHandle heap = open_one_heap();
    REQUIRE(heap >= 0);

    void* const first = OSAllocFromHeap(heap, 100);
    void* const second = OSAllocFromHeap(heap, 100);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(is_aligned_32(first));
    CHECK(is_aligned_32(second));
    CHECK(first != second);

    // 100 bytes plus a 32-byte header rounds to 160, so the second block
    // starts exactly that far along.  GX reads straight out of these, which
    // is why the granularity is not negotiable.
    CHECK_EQ(static_cast<unsigned char*>(second) -
                 static_cast<unsigned char*>(first),
             160);
    CHECK_EQ(OSReferentSize(first), 128u);

    OSFreeToHeap(heap, first);
    OSFreeToHeap(heap, second);
    CHECK_EQ(OSReferentSize(first), 0u);

}

MELEE_TEST(UpstreamArena, ReportsFreeSpaceRatherThanZero)
{
    const os::ArenaScope arena(kTestArenaBytes);
    const OSHeapHandle heap = open_one_heap();
    REQUIRE(heap >= 0);

    const s32 empty = OSCheckHeap(heap);
    REQUIRE(empty > 0);

    void* const block = OSAllocFromHeap(heap, 1000);
    REQUIRE(block != nullptr);
    const s32 after = OSCheckHeap(heap);
    REQUIRE(after >= 0);

    // 1000 + 32 rounds up to 1056, all of which leaves the free list.
    CHECK_EQ(empty - after, 1056);

    OSFreeToHeap(heap, block);
    // Freeing the only allocation puts the heap back exactly as it was --
    // which is the coalescing working, not just the accounting.
    CHECK_EQ(OSCheckHeap(heap), empty);

}

MELEE_TEST(UpstreamArena, CoalescesNeighboursOnFree)
{
    const os::ArenaScope arena(kTestArenaBytes);
    const OSHeapHandle heap = open_one_heap();
    REQUIRE(heap >= 0);
    const s32 empty = OSCheckHeap(heap);

    void* const a = OSAllocFromHeap(heap, 256);
    void* const b = OSAllocFromHeap(heap, 256);
    void* const c = OSAllocFromHeap(heap, 256);
    REQUIRE(a != nullptr && b != nullptr && c != nullptr);

    // Freeing the middle one first leaves a hole between two live blocks; the
    // neighbours then merge into it from both sides.
    OSFreeToHeap(heap, b);
    OSFreeToHeap(heap, a);
    OSFreeToHeap(heap, c);
    CHECK_EQ(OSCheckHeap(heap), empty);

    // And the merged run is one cell again: a request the three fragments
    // could not have satisfied now fits where they were.
    void* const whole = OSAllocFromHeap(heap, 3 * 256);
    CHECK_EQ(whole, a);

}

MELEE_TEST(UpstreamArena, RefusesWhatItCannotFitAndStaysConsistent)
{
    const os::ArenaScope arena(kTestArenaBytes);
    const OSHeapHandle heap = open_one_heap();
    REQUIRE(heap >= 0);
    const s32 empty = OSCheckHeap(heap);

    CHECK_EQ(OSAllocFromHeap(heap, kTestArenaBytes * 2), nullptr);
    CHECK_EQ(OSAllocFromHeap(heap, 0), nullptr);
    CHECK_EQ(OSAllocFromHeap(-1, 32), nullptr);
    CHECK_EQ(OSCheckHeap(heap), empty);

    // A pointer that never came from this heap is ignored rather than
    // corrupting the free list.
    int stack_object = 0;
    OSFreeToHeap(heap, &stack_object);
    OSFreeToHeap(heap, nullptr);
    CHECK_EQ(OSCheckHeap(heap), empty);

}

MELEE_TEST(UpstreamArena, DestroyingAHeapReleasesItsSlot)
{
    const os::ArenaScope arena(kTestArenaBytes);
    const OSHeapHandle first = open_one_heap();
    REQUIRE(first >= 0);
    CHECK_EQ(os::live_heap_count(), 1);

    OSSetCurrentHeap(first);
    OSDestroyHeap(first);
    CHECK_EQ(os::live_heap_count(), 0);
    // The current heap cannot be a destroyed one.
    CHECK_EQ(__OSCurrHeap, -1);
    CHECK_EQ(OSCheckHeap(first), -1);

    const OSHeapHandle second =
        OSCreateHeap((void*) OSRoundUp32B(OSGetArenaLo()),
                     (void*) OSRoundDown32B(OSGetArenaHi()));
    CHECK_EQ(second, first);

}

MELEE_TEST(UpstreamArena, EveryAllocationKeepsADistinctLowWord)
{
    const os::ArenaScope arena(kTestArenaBytes);
    const OSHeapHandle heap = open_one_heap();
    REQUIRE(heap >= 0);

    // The ID table keys on the low word of a pointer.  Walk a few hundred
    // allocations and check none of them collides -- on a host whose heap
    // spans more than 4 GiB this is the failure that would show up as a
    // constraint quietly following the wrong bone.
    std::vector<std::uint32_t> low_words;
    for (int i = 0; i < 256; ++i) {
        void* const block = OSAllocFromHeap(heap, 64);
        REQUIRE(block != nullptr);
        low_words.push_back(
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(block)));
    }
    for (std::size_t i = 1; i < low_words.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (low_words[i] == low_words[j]) {
                CHECK(false);
                break;
            }
        }
    }

}

// ---------------------------------------------------------------------------
// The boot itself.
//
// tests/hsd/upstream_boot.cpp runs Melee's own bring-up once, before the
// first case: the four HSD_SetInitParameter calls, HSD_AllocateXFB,
// HSD_AllocateFifo through GXInit, and HSD_InitComponent.  These cases read
// what it left behind.

MELEE_TEST(UpstreamBoot, CarvesTheConsolesHeapsOutOfTheArena)
{
    // Two heaps, in the order HSD_OSInit creates them: audio first, then
    // everything else.  HSD_GetHeap is initialize.c's own accessor, so a
    // sensible answer here means upstream's boot really ran.
    CHECK_EQ(os::live_heap_count(), 2);
    CHECK_EQ(HSD_Synth_804D6018, 0);
    CHECK_EQ(HSD_GetHeap(), 1);

    // The audio heap is the default size, less its own cell header.  The
    // figures are the ones the boot produced rather than the ones still
    // free: the audio driver allocates out of both heaps, and its cases run
    // before this one.
    CHECK_EQ(meleeboard::test::boot_audio_heap_free(),
             (int) HSD_DEFAULT_AUDIO_SIZE - 32);

    // The main heap is everything the framebuffers, the graphics fifo and the
    // audio heap did not take.  24 MiB in, a little over 22 MiB out.
    const int main_free = meleeboard::test::boot_main_heap_free();
    CHECK(main_free > 22 * 1024 * 1024);
    CHECK(main_free < 23 * 1024 * 1024);

    // And both are still consistent after whatever has allocated from them.
    CHECK(OSCheckHeap(HSD_Synth_804D6018) >= 0);
    CHECK(OSCheckHeap(HSD_GetHeap()) >= 0);

    // And the arena is spent: HSD_OSInit's last act is to hand what is left
    // to the heap and set arenaLo to arenaHi.
    CHECK_EQ(OSGetArenaLo(), OSGetArenaHi());
}

MELEE_TEST(UpstreamBoot, PutsTheFramebuffersInTheArena)
{
    void* const* const buffers = meleeboard::test::boot_frame_buffers();
    REQUIRE(buffers != nullptr);

    const auto* base = static_cast<const unsigned char*>(os::arena_base());
    for (int index = 0; index < 2; ++index) {
        REQUIRE(buffers[index] != nullptr);
        CHECK(is_aligned_32(buffers[index]));
        CHECK(static_cast<const unsigned char*>(buffers[index]) > base);
        CHECK(static_cast<const unsigned char*>(buffers[index]) <
              base + os::arena_bytes());
    }

    // 640x480 at two bytes a pixel, and the second buffer starts exactly that
    // far past the first.
    CHECK_EQ(static_cast<const unsigned char*>(buffers[1]) -
                 static_cast<const unsigned char*>(buffers[0]),
             640 * 480 * 2);

    // HSD_VI_XFB_MAX is three; the third was asked for two and stays null.
    CHECK_EQ(buffers[2], nullptr);
}

MELEE_TEST(UpstreamBoot, LeavesHSDAllocatingOutOfTheArena)
{
    // The point of the whole file: HSD_MemAlloc goes through HSD_GetHeap,
    // which answers the heap HSD_OSInit carved.  Before the boot existed it
    // answered -1 and every allocation asserted.
    void* const block = HSD_MemAlloc(64);
    REQUIRE(block != nullptr);
    CHECK(is_aligned_32(block));

    const auto* base = static_cast<const unsigned char*>(os::arena_base());
    CHECK(static_cast<const unsigned char*>(block) > base);
    CHECK(static_cast<const unsigned char*>(block) < base + os::arena_bytes());
    CHECK_EQ(OSReferentSize(block), 64u);

    HSD_Free(block);
}

MELEE_TEST(UpstreamBoot, ProducesTheConsolesOpeningGXSequence)
{
    const std::vector<std::string>& trace = meleeboard::test::boot_gx_trace();
    REQUIRE(!trace.empty());

    // The fifo is the size main() asks for, out of the arena.
    CHECK_EQ(trace[0], std::string("GXInit(p0, 262144)"));

    // The whole of it, in order.  This is upstream's HSD_VIInit and
    // HSD_GXInit talking to GX with nothing of the port's in between: the
    // video mode and its copy filter, one black copy to clear the screen, and
    // eight light objects loaded with the zero light.  A golden trace of a
    // console boot.
    std::string shape;
    for (const std::string& line : trace) {
        if (!shape.empty()) {
            shape += '\n';
        }
        shape += line;
    }
    CHECK_EQ(shape,
             std::string(
                 "GXInit(p0, 262144)\n"
                 "GXSetDrawDoneCallback(p1)\n"
                 "GXSetCopyFilter(0, [6:6 6:6 6:6 6:6 6:6 6:6 6:6 6:6 6:6 "
                 "6:6 6:6 6:6], 1, [0 0 21 22 21 0 0])\n"
                 "GXSetDispCopyGamma(0)\n"
                 "GXSetColorUpdate(1)\n"
                 "GXSetAlphaUpdate(1)\n"
                 "GXSetZMode(1, 3, 1)\n"
                 "GXSetCopyClear(0:0:0:0, 16777215)\n"
                 "GXSetCopyClamp(3)\n"
                 "GXSetDispCopySrc(0, 0, 640, 480)\n"
                 "GXSetDispCopyYScale(1)\n"
                 "GXSetDispCopyDst(640, 480)\n"
                 "GXCopyDisp(p2, 1)\n"
                 "GXPixModeSync()\n"
                 "GXInitLightPos(p3, 1, 0, 0)\n"
                 "GXInitLightDir(p3, 1, 0, 0)\n"
                 "GXInitLightAttn(p3, 1, 0, 0, 1, 0, 0)\n"
                 "GXInitLightColor(p3, 0:0:0:0)\n"
                 "GXLoadLightObjImm(p3, 1)\n"
                 "GXLoadLightObjImm(p3, 2)\n"
                 "GXLoadLightObjImm(p3, 4)\n"
                 "GXLoadLightObjImm(p3, 8)\n"
                 "GXLoadLightObjImm(p3, 16)\n"
                 "GXLoadLightObjImm(p3, 32)\n"
                 "GXLoadLightObjImm(p3, 64)\n"
                 "GXLoadLightObjImm(p3, 128)\n"
                 "GXClearVtxDesc()"));
}
