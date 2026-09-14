#pragma once

// The video interface, and the frame boundary.
//
// On the console VI raises an interrupt at every field, and the SDK calls two
// callbacks around it -- one before the hardware latches its registers, one
// after.  HSD registers both in video.c; the game swaps them at scene
// boundaries.  `VIWaitForRetrace()` blocks until the next one.
//
// That blocking wait is the thing a threaded port would have needed a
// coroutine for, and the reason it does not is the finding in
// os_scheduler.hpp: there is no other thread to yield to.  Melee calls
// `VIWaitForRetrace()` inside loops that spin until a framebuffer frees up --
//
//     while ((idx = HSD_VIGetXFBDrawEnable()) == -1) { VIWaitForRetrace(); }
//
// -- so the wait is not a yield, it is the step that moves the world forward.
// Here it advances the timebase by one field and delivers the retrace.  The
// game's own main loop is therefore the frame pump, which is exactly the
// arrangement the console had.
//
// Retrace goes through the scheduler rather than being called directly,
// because retrace *is* an interrupt: video.c masks interrupts to swap the
// callback pointers, and that critical section has to hold the handler off.

#include <cstdint>

extern "C" {
#include <dolphin/gx.h>
#include <dolphin/vi.h>
}

namespace meleeboard::vi {

// Clears the callbacks, the framebuffers and the retrace count.
void reset();

// How many fields have passed since reset(), which is the argument the
// retrace callbacks are handed.
std::uint32_t retrace_count();

// The framebuffer VISetNextFrameBuffer last named, and the one actually
// being scanned out -- they differ until VIFlush or the next retrace.
const void* pending_frame_buffer();
const void* current_frame_buffer();

// What VIConfigure was last given.  Null until the game configures a mode.
const GXRenderModeObj* configured_mode();

bool is_black();

} // namespace meleeboard::vi
