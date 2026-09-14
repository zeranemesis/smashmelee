#pragma once

// The auxiliary RAM, and the queue that moves data into it.
//
// ARAM is the GameCube's second memory: 16 MiB the CPU cannot address, reached
// only by DMA.  Melee keeps its sound banks there -- `HSD_SynthInit` carves
// them with ARAlloc and `devcom.c` streams them in from the disc through
// ARQPostRequest -- so upstream's audio layer does not run without it.
//
// Aurora implements this for the shipping build, and models it the same way:
// ARAM "addresses" are offsets into a host buffer, and ARAlloc is a bump
// allocator because that is what the console's is.  This is the conformance
// target's copy, for the same reason os_arena.hpp exists -- that target links
// no Aurora library, and aurora::core would bring SDL, fmt, abseil and sqlite
// with it.
//
// The queue does **not** complete a transfer inside ARQPostRequest, and that
// is not a simplification -- it is a correction the game itself forced.
//
// The first version of this file copied immediately and ran the callback
// before ARQPostRequest returned, on the reasoning that a finished transfer
// is the easiest thing for a test to think about.  Upstream crashed on it:
// `HSD_DevComARAMWakeUp` posts a request and *then* advances its own
// bookkeeping -- `aramDC->dest += xfer_size` on the next line -- and the
// callback had already unlinked `aramDC` and left it null.  On the console
// that cannot happen, because ARQ completes at a DMA interrupt, strictly
// after the call that posted it returns.
//
// So a posted request is queued, and service_aram_queue() performs it and
// delivers its callback -- which is an interrupt, and is therefore held off
// while interrupts are masked, exactly as os_scheduler.hpp holds an alarm
// off.  Every request is recorded either way, so a test can assert what the
// game asked to be moved where.

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include <dolphin/ar.h>
}

namespace meleeboard::os {

// The console's ARAM.
inline constexpr std::uint32_t kAramBytes = 16u * 1024u * 1024u;

// One completed transfer.
struct AramTransfer {
    std::uint32_t type = 0; // ARAM_DIR_MRAM_TO_ARAM or the reverse
    std::uint32_t owner = 0;
    std::uint32_t priority = 0;
    std::uintptr_t source = 0;
    std::uintptr_t dest = 0;
    std::uint32_t length = 0;
};

// Releases the ARAM buffer, the allocation stack, the queue and the transfer
// log.  Every AR entry point then reports failure rather than touching freed
// memory.
void reset_aram();

// Performs every queued transfer and delivers its callback, oldest first.
// Returns how many completed.  Nothing completes while interrupts are masked:
// a DMA completion is an interrupt, and the game's critical sections are
// written expecting to hold one off.
std::size_t service_aram_queue();

// How many transfers are posted and not yet performed.
std::size_t aram_queue_depth();

// The transfers that have been performed, in order.
const std::vector<AramTransfer>& aram_transfers();

// How much of ARAM ARAlloc has handed out.
std::uint32_t aram_used();

// The bytes at an ARAM offset, or null when the range runs past the end.  A
// test reads back what a transfer moved; nothing in the game can do this,
// because on the console the CPU cannot address ARAM at all.
const unsigned char* aram_bytes(std::uint32_t offset, std::uint32_t length);

} // namespace meleeboard::os
