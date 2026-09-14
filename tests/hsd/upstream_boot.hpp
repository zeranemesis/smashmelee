#pragma once

// The console's boot sequence, run against the port.
//
// Melee's main() is fifty calls long, and the first six of them are the SDK
// coming up.  The HSD part is four:
//
//     HSD_SetInitParameter(...) x4
//     HSD_AllocateXFB(2, &GXNtsc480IntDf);
//     HSD_GXSetFifoObj(GXInit(HSD_AllocateFifo(0x40000), 0x40000));
//     HSD_InitComponent();
//
// Every one of those is upstream's own code, and this runs it unchanged.  It
// matters for two reasons.  The first is practical: HSD_GetHeap answers -1
// until HSD_InitComponent has carved a heap out of the arena, so every
// allocation in the conformance target fails until this has run -- which is
// exactly what happens on hardware.  The second is that it is the first time
// the port executes the game's own bring-up rather than a test's
// approximation of it, and what comes out the other end is a recorded GX
// trace of the console's opening frame.

#include <string>
#include <vector>

namespace meleeboard::test {

// Runs the sequence above once.  Idempotent: the second call is a no-op, so a
// case can ask for the world to exist without caring who asked first.
void boot_hsd();

// The GX trace HSD_InitComponent produced, captured before any case ran.
const std::vector<std::string>& boot_gx_trace();

// The external framebuffers HSD_AllocateXFB carved out of the arena, as it
// returned them: HSD_VI_XFB_MAX entries, the unused ones null.
void* const* boot_frame_buffers();

} // namespace meleeboard::test
