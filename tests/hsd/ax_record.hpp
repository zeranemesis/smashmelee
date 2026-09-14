#pragma once

// A recording AX surface.
//
// Melee's audio boundary turned out to be far smaller than docs/PLAN.md
// assumed, and measuring it is what this file is built on.  Three upstream
// units touch the sound SDK at all -- sysdolphin/baselib/axdriver.c,
// sysdolphin/baselib/synth.c and melee/lb/lbaudio_ax.c -- and between them
// they reference 35 entry points: 22 in AX, 13 in AXFX.  The DSP library is
// referenced **nowhere**: the game never queues a DSP task, because AX does
// that on its behalf.  AI is three calls, and the whole ARAM surface the game
// uses -- ARInit, ARAlloc, ARFree, ARGetSize, ARQInit, ARQPostRequest -- is
// already implemented by Aurora.
//
// So this is the same move that unblocked the render half: give the units a
// surface to talk to and let them run.  Every entry point here appends a line
// to a trace, and a test reads the lines back.
//
// It is not only a trace, though, and that matters.  synth.c reads the voice
// parameter block straight out of the AXVPB -- `voice->pb.state`,
// `voice->pb.addr.currentAddressHi`, `voice->pb.itd.flag` -- and writes
// `pb.itd` in place, and it indexes its own node table by `voice->index`.  So
// the recorder keeps a **real** voice pool and applies each setter to the
// block the way the SDK's own setter does.  That state is also what a host
// mixer would read: the AXPB in Melee's bundled <dolphin/ax.h> is the
// complete per-voice DSP state, sample address and format, loop points, ADPCM
// coefficients and predictor, resampling ratio with its four history samples,
// the volume envelope and a twenty-field mixer.  Nothing about it is opaque.
//
// What the recorder does *not* model is the DSP: no sample is fetched, no
// ADPCM block is decoded, nothing is mixed, and the frame callback is never
// driven on its own.  Anything the real SDK setters additionally reset inside
// the block -- the SDK's sources are in neither tree -- is not reproduced
// either.  The consequence is bounded and worth stating: a test can assert
// which voices the game acquired and how it configured them, and cannot
// assert what they would have sounded like.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <dolphin/ax.h>
}

namespace meleeboard::test::ax {

// Clears the trace, the pointer numbering and the voice pool.  The recorder
// is process-global, exactly as AX is.
void reset();

// One line per call, in call order.
const std::vector<std::string>& trace();

// The trace as one newline-terminated block, for a golden comparison.
std::string joined();

// How many times `name` was called, and the `index`th such line.
std::size_t count(const char* name);
const std::string* call(const char* name, std::size_t index = 0);

// The names in the trace, in order, without their arguments.
std::vector<std::string> names();

// How many of the 64 voices are held, and the voice at `index` whether held
// or not -- a test asserts on the parameter block the game left in it.
std::size_t voices_in_use();
const AXVPB* voice(std::size_t index);

// The callbacks AXRegisterCallback and the two aux registrations were given.
// The recorder never drives them: what a frame would do is the mixer's
// business, and there is no mixer yet.
void* frame_callback();
void* aux_a_callback();
void* aux_b_callback();

} // namespace meleeboard::test::ax
