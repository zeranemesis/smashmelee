#pragma once

// The OS arena and heap allocator, which is where every HSD allocation ends.
//
// Upstream's HSD_OSInit is eight SDK calls long and every one of them is an
// allocator call: OSGetArenaLo/Hi to find the free memory the boot code left,
// OSInitAlloc to carve a heap-descriptor array out of the bottom of it,
// OSCreateHeap twice -- once for audio, once for everything else -- and
// OSSetArenaLo to hand back what is left.  Nothing above it runs until those
// return sensible answers, which is why phase 1 of docs/PLAN.md ends here.
//
// The shipping build does not use this file: the runtime links aurora::os,
// whose OSAlloc.cpp is a complete SDK-shaped allocator over host-allocated
// MEM1.  This is the conformance target's allocator, and it exists because
// that target deliberately links no Aurora library -- aurora::core drags in
// SDL, fmt, abseil and sqlite, none of which an offline test suite should
// need in order to check that a heap hands back the block it was given.
//
// It is written to the same observable contract as Aurora's, because the
// point of the conformance target is that upstream's code cannot tell the
// difference: 32-byte cells with a 32-byte header, first-fit from a free list
// held in address order, coalescing on free, and OSCheckHeap returning the
// heap's free bytes or -1 when its structure is inconsistent.  objalloc.c
// branches on that last one, so it has to be honest rather than a stub.
//
// Two things it does that Aurora's does not, both of which the tests need:
//
//   * the whole arena sits in **one** host block whose size is a power of two
//     and whose base is aligned to that size, so the low word of every
//     address in it is distinct.  That is the same construction
//     DescriptorArena uses, for the same reason -- upstream keys its ID table
//     on a truncated pointer -- and putting the arena itself on that footing
//     generalises the fix from joints to every HSD object;
//   * the arena can be torn down and rebuilt between tests, so each case
//     starts from an empty heap rather than inheriting the last one's
//     fragmentation.

#include <cstddef>
#include <cstdint>

extern "C" {
#include <dolphin/os.h>
}

namespace meleeboard::os {

// The retail GameCube's MEM1.  OSGetPhysicalMemSize() reports it.
inline constexpr std::uint32_t kPhysicalMemBytes = 0x01800000u;

// What the boot code keeps below and above the arena on a real console: the
// OS globals and the interrupt vectors at the bottom, the initial stack at
// the top.  The exact values do not matter to anything above; what matters is
// that the arena does not start at the base of physical memory, so code that
// assumes arenaLo is the bottom of the world fails here the way it would
// fail on hardware.
inline constexpr std::uint32_t kSystemReserveLowBytes = 0x3100u;
inline constexpr std::uint32_t kSystemReserveHighBytes = 0x4000u;

// Installs a fresh arena of `physical_bytes` and points OSGetArenaLo/Hi into
// it.  Any heap created against a previous arena is gone.  Called with no
// argument it reproduces the console's geometry.
void reset_arena(std::uint32_t physical_bytes = kPhysicalMemBytes);

// Releases the host block.  Every OS allocator entry point then reports
// failure rather than touching freed memory, which is what a test that forgot
// to call reset_arena() should see.
void release_arena();

// The host block behind the arena, and how much of it the arena spans.  The
// block is the rounded-up power of two; the arena is `physical_bytes` of it.
const void* arena_base();
std::uint32_t arena_bytes();

// True when every address the arena can hand out has a distinct low word --
// the property the single aligned block exists to provide.
bool low_words_are_distinct();

// Stands a private arena up for the length of a scope, and puts the previous
// one back on the way out.
//
// The conformance target boots HSD once before the first case, and everything
// HSD has allocated since lives in the arena that boot created -- including
// the free lists its object pools hold between cases.  A case that wants an
// arena of its own therefore cannot just reset the global one: releasing it
// would free memory those pools still point into, and the next allocation
// would hand back a dangling block.  This saves every piece of allocator
// state, installs a fresh arena over a separate host block, and restores the
// lot afterwards.
struct SavedArena;

class ArenaScope {
public:
    explicit ArenaScope(std::uint32_t physical_bytes = kPhysicalMemBytes);
    ~ArenaScope();

    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    SavedArena* saved_ = nullptr;
};

// How many heaps OSCreateHeap has handed out and not destroyed.  A test reads
// it to check that HSD_CreateMainHeap really did destroy the old one.
int live_heap_count();

} // namespace meleeboard::os
