#pragma once

// What Aurora's GXSetArray needs and the console's did not.
//
// The GameCube's GX read a vertex array where it lay, in the archive, in the
// console's byte order, and needed to be told only the base address and the
// stride.  Aurora's writes a 64-bit base into the command stream and its
// backend copies the array out of guest memory, so it also needs to know how
// many bytes there are and which way round they are.
//
// HSD carries neither.  A vertex array's length is not in the archive at all
// -- nothing bounds it but the end of the data section and the indices that
// address it -- and its byte order depends on whether whoever loaded the
// archive converted it.  Both are therefore facts about the *load*, and this
// is where the load records them.
//
// include/melee/port/dolphin_compat.h routes upstream's three-argument
// GXSetArray calls through the two entry points this file defines.

#include <cstdint>

namespace meleeboard::hsd {

class ArchiveConverter;

// Records every vertex array `converter` built, so that GXSetArray can answer
// for them.  Explicit rather than automatic: this is global state, and a
// converter quietly writing to it would be worse than a caller saying so.
void register_vertex_arrays(const ArchiveConverter& converter);

// Records one array directly, for a loader that is not the converter.
void register_vertex_array(const void* base, uint32_t extent,
                           bool little_endian);

// Drops every registration.  An archive that is unloaded has to do this:
// the addresses belong to its bytes.
void forget_vertex_arrays();

} // namespace meleeboard::hsd
