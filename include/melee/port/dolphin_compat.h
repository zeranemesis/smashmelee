#ifndef MELEE_PORT_DOLPHIN_COMPAT_H
#define MELEE_PORT_DOLPHIN_COMPAT_H

// Parsed before every upstream translation unit (see cmake/MeleeUpstream.cmake).
//
// Melee's decompiled C is written against the GameCube SDK headers.  Aurora
// supplies host implementations of that SDK, but its headers are not a
// drop-in: they give the integer types their host-correct widths under
// TARGET_PC, which is what makes a 64-bit build correct at all, and in
// exchange they stop short of a handful of SDK spellings the game uses.
// This header supplies exactly those, and nothing else.
//
// The measurements behind this list are in docs/UPSTREAM_NATIVE_SPIKE.md;
// every entry here closed a named compile failure.

// mtx.c, quatlib.c, jobj.c and generator.c use M_PI and M_PI_2.  They are not
// in standard C at all: glibc offers them unless asked for strict ISO, and
// MSVC's <math.h> withholds them until this is defined.  It has to arrive
// before the first <math.h> in the translation unit, because the constants sit
// behind that header's own include guard -- cmake/MeleeUpstream.cmake also
// passes it on the command line, which is the route that cannot be too late.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

// The host declarations have to be seen before anything can macro over them.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// _USE_MATH_DEFINES is MSVC's mechanism and only MSVC's: glibc gates these on
// _DEFAULT_SOURCE instead, so a strict-ISO build would still not have them.
// The platform's own definitions are preferred where they exist; these are the
// literals both MSVC and glibc use, so the fallback cannot change a result.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

// The Metrowerks libc rewrites fabs/fabsf into PowerPC intrinsics that have no
// host definition.  Take the host's.
#undef fabs
#undef fabsf

#include <dolphin/types.h>
#include <dolphin/mtx/GeoTypes.h>

// Upstream's Runtime/platform.h removes the SDK boolean spellings in favour of
// <stdbool.h>, but Aurora's own Dolphin headers still declare with them.
#ifndef BOOL
typedef int BOOL;
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

// Aurora's GeoTypes.h names the three-component vector Vec and stops there;
// the SDK also names these, and the game uses all of them.
#ifndef MELEE_COMPAT_VECTORS
#define MELEE_COMPAT_VECTORS
typedef Vec Vec3;
typedef S16Vec S16Vec3;
typedef Quaternion Vec4;
typedef struct { f32 x, y; } Vec2, *Vec2Ptr, Point2d, *Point2dPtr;
typedef struct { s8 x, y, z; } S8Vec3, S8Vec, *S8Vec3Ptr, *S8VecPtr;
typedef struct { u8 x, y, z, w; } U8Vec4, *U8Vec4Ptr;
typedef struct { int x, y; } IntVec2, *IntVec2Ptr;
typedef struct { int x, y, z; } IntVec3, *IntVec3Ptr;
typedef struct { s32 x, y; } S32Vec2, *S32Vec2Ptr;
typedef struct { s32 x, y, z; } S32Vec, S32Vec3, *S32VecPtr, *S32Vec3Ptr;
#endif

// Aurora's GX headers do not carry the TEV clamp modes, which mobj.h needs in
// a structure field rather than only at a call site.
#ifndef MELEE_COMPAT_GX_TEV_CLAMP
#define MELEE_COMPAT_GX_TEV_CLAMP
typedef enum _GXTevClampMode {
    GX_TC_LINEAR,
    GX_TC_GE,
    GX_TC_EQ,
    GX_TC_LE,
    GX_MAX_TEVCLAMPMODE
} GXTevClampMode;
#endif

// GX comes in whole, and through the umbrella header: several of Aurora's GX
// headers name enums they do not include the declaration of, and only
// <dolphin/gx.h> puts them in the right order.  It is here, rather than left
// to each translation unit, because the adapter below has to be installed
// after Aurora's declaration of GXSetArray has been parsed -- a function-like
// macro would otherwise be applied to the declaration itself.
#include <dolphin/gx.h>

// Upstream calls GXSetArray with the three arguments the GameCube took.
// Aurora's takes five: it writes a 64-bit base pointer into the command
// stream, and the backend then has to copy the array out of guest memory, so
// it needs the length in bytes and the byte order as well.  The console
// needed neither -- the GPU read the array where it lay, big-endian.
//
// Aurora offers GXSETARRAY() as the spelling that works on both, and
// upstream's own SDK header defines it too, so the two call sites in pobj.c
// are an upstream oversight rather than a disagreement.  Until they change,
// this maps three arguments onto five.  Within the replacement list the name
// is not expanded again, so GXSetArray below is Aurora's function.
//
// The two arguments the call site cannot supply are asked of the port rather
// than invented: HSD never records how long a vertex array is, and whether
// its contents are still big-endian depends on whether the archive was
// converted when it was loaded.  Phase 3 of docs/PLAN.md is where a real
// answer is owed; see the definitions for what is answered today.
#ifndef MELEE_COMPAT_GX_SET_ARRAY
#define MELEE_COMPAT_GX_SET_ARRAY
// Defined in C, and this header reaches C++ too, so the linkage is spelled
// out rather than left to whichever language happens to be compiling.
#ifdef __cplusplus
extern "C" {
#endif
u32 melee_gx_array_extent(const void* base);
bool melee_gx_array_is_little_endian(const void* base);
#ifdef __cplusplus
}
#endif
#define GXSetArray(attr, base, stride)                                         \
    GXSetArray((attr), (base), melee_gx_array_extent(base), (stride),          \
               melee_gx_array_is_little_endian(base))
#endif

#endif // MELEE_PORT_DOLPHIN_COMPAT_H
