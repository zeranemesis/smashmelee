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

// The host declarations have to be seen before anything can macro over them.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

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

#endif // MELEE_PORT_DOLPHIN_COMPAT_H
