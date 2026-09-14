#pragma once

// A recording GX surface.
//
// The whole render half of HSD -- jobj, dobj, mobj, pobj, tobj, cobj, lobj,
// tev, texp, state, shadow -- talks to GX and nothing else.  Aurora
// implements GX for real, but that implementation pulls in the window, the
// swapchain and the shader compiler, none of which an offline test wants.
// So this target links a GX that records instead of drawing: every entry
// point appends one line naming itself and its arguments, and a test reads
// the lines back.
//
// Two properties make the trace worth asserting on:
//
//   * it is deterministic.  Pointers are not printed as addresses -- each
//     distinct pointer gets a small index, assigned in the order the trace
//     first sees it -- so the same scene produces the same text on every run,
//     on every host, under any allocator;
//   * it is the complete GX side of a frame.  That is what phase 3 of
//     docs/PLAN.md needs: a golden trace from a known scene, to compare a
//     ported renderer against.
//
// Four GX calls are not recorded but implemented, because HSD reads their
// results back and branches on them: GXGetTexBufferSize (in
// gx_texture_stub.cpp) and the three GXGetTexObj* getters, which answer from
// what GXInitTexObj was given.

#include <cstddef>
#include <string>
#include <vector>

namespace meleeboard::test::gx {

// Clears the trace and the pointer numbering.  Call it at the top of a case:
// the recorder is process-global, exactly as GX is.
void reset();

// One line per call, in call order.
const std::vector<std::string>& trace();

// The trace as one newline-terminated block, for a golden comparison.
std::string joined();

// How many times `name` was called.  The name must match exactly.
std::size_t count(const char* name);

// The `index`th call to `name`, or nullptr past the end.  The returned line
// includes the arguments, so a test can assert on them as text.
const std::string* call(const char* name, std::size_t index = 0);

// The line at `index` in the whole trace, or nullptr past the end.
const std::string* at(std::size_t index);

// The names in the trace, in order, without their arguments.  This is the
// shape of a frame -- useful when the argument values are beside the point.
std::vector<std::string> names();

} // namespace meleeboard::test::gx
