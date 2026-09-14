#pragma once

// The threading model, and why it is not a threading model.
//
// docs/PLAN.md expected this to be the port's one genuinely design-sensitive
// decision: cooperative coroutines over `libco` versus preemptive host
// threads, with determinism as the tiebreaker.  Measuring the decompilation
// dissolved the question.
//
// **Melee's shipping code creates no OS threads.**  The only OSCreateThread in
// the whole tree is in debugconsole_main.c, and MetroTRK -- the other place
// OSThread appears -- is the Metrowerks debugger stub.  Neither ships.  The
// SDK is not decompiled at all, so DVD and audio threading is Aurora's
// business, not this port's.
//
// What the game actually uses is three things:
//
//   * **interrupt masking**, 225 calls across eighteen files.  It does not
//     guard against another thread -- there isn't one.  It guards against a
//     *handler*: video.c masks interrupts to swap a retrace callback pointer
//     without the retrace handler seeing a half-written value;
//   * **alarms**, three users, all timers.  lbmemory.c is the clearest: it
//     copies 0x19000 bytes, re-arms a 3 ms alarm, and copies the next chunk
//     -- cooperative time-slicing written by hand, not a thread;
//   * **SDK completion callbacks** for DVD reads and audio, which arrive from
//     libraries this port does not compile.
//
// So the model is one thread, and the only question worth deciding is *when*
// callbacks are allowed to run.  They run at points this scheduler chooses,
// which makes a frame reproducible: the same inputs produce the same sequence
// of handler calls at the same timebase values, on every host, every run.

#include <cstddef>
#include <cstdint>

extern "C" {
#include <dolphin/os.h>
}

namespace meleeboard::os {

// One NTSC field, in timer ticks, exactly.
//
// The console's timebase is the bus clock over four, 40.5 MHz, and NTSC is
// 60000/1001 Hz.  40500000 * 1001 / 60000 is 675675 with no remainder -- the
// frame period is an exact number of ticks, so a fixed step accumulates no
// error at all.  That is worth knowing: it means the port can be bit-exact
// about time without a rational accumulator.
inline constexpr OSTime kTicksPerFrame = 675675;

// Returns the timebase to zero, cancels every alarm and drops every deferred
// callback.  Interrupts come back enabled.
void reset();

// Advances the timebase by exactly one frame, stopping at each alarm that
// comes due to run it at its own fire time -- so a handler reading OSGetTime()
// sees the moment it was scheduled for, not the end of the frame.
//
// An alarm that comes due while interrupts are masked is deferred, exactly as
// a real interrupt would be, and runs when they are restored.
void advance_frame();

// How many alarms are armed, and how many callbacks are waiting for
// interrupts to come back.
std::size_t armed_alarms();
std::size_t deferred_callbacks();
bool interrupts_enabled();

// Set when advance_frame() gives up on an alarm that re-arms in the past --
// which would otherwise spin forever.  A test reads it; a runtime should
// treat it as a bug in the alarm's period.
bool runaway_alarm_detected();

} // namespace meleeboard::os
