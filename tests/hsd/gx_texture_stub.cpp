// The HSD scene decoder sizes texture images with GXGetTexBufferSize, whose
// only implementation lives inside Aurora's GX runtime.  Linking that runtime
// would pull in the whole graphics stack, so the offline suite provides the
// same fixed GameCube tiling arithmetic as a test double.  It intentionally
// mirrors extern/aurora/lib/dolphin/gx/GXTexture.cpp; the tile geometry is a
// property of the console's texture units, not of Aurora.

#include <dolphin/gx/GXTexture.h>

extern "C" u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mips,
                                  u8 max_lod)
{
    s32 shift_x = 0;
    s32 shift_y = 0;
    switch (format) {
    case GX_TF_I4:
    case GX_TF_C4:
    case GX_TF_CMPR:
    case GX_CTF_R4:
    case GX_CTF_Z4:
        shift_x = 3;
        shift_y = 3;
        break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_C8:
    case GX_TF_Z8:
    case GX_CTF_RA4:
    case GX_CTF_A8:
    case GX_CTF_R8:
    case GX_CTF_G8:
    case GX_CTF_B8:
    case GX_CTF_Z8M:
    case GX_CTF_Z8L:
        shift_x = 3;
        shift_y = 2;
        break;
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_RGBA8:
    case GX_TF_C14X2:
    case GX_TF_Z16:
    case GX_TF_Z24X8:
    case GX_CTF_RA8:
    case GX_CTF_RG8:
    case GX_CTF_GB8:
    case GX_CTF_Z16L:
        shift_x = 2;
        shift_y = 2;
        break;
    default:
        break;
    }

    const u32 bit_size =
        format == GX_TF_RGBA8 || format == GX_TF_Z24X8 ? 64 : 32;
    u32 length = 0;
    if (mips) {
        while (max_lod != 0) {
            const u32 tiles_x = (width + (1U << shift_x) - 1) >> shift_x;
            const u32 tiles_y = (height + (1U << shift_y) - 1) >> shift_y;
            length += bit_size * tiles_x * tiles_y;
            if (width == 1 && height == 1) {
                return length;
            }
            width = width < 2 ? 1 : width / 2;
            height = height < 2 ? 1 : height / 2;
            --max_lod;
        }
    } else {
        const u32 tiles_x = (width + (1U << shift_x) - 1) >> shift_x;
        const u32 tiles_y = (height + (1U << shift_y) - 1) >> shift_y;
        length = bit_size * tiles_x * tiles_y;
    }
    return length;
}
