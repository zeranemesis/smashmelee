#include "gx_record.hpp"

// Implementation of the recording GX surface described in gx_record.hpp.
//
// Every definition here is checked against Aurora's own declaration, because
// Aurora's headers are on the include path and these are extern "C": a
// signature that drifts from the real GX does not compile.  That is the point
// of writing the stub against the headers rather than against a document --
// the same reason the *32b converters come from upstream's code.
//
// The bodies are deliberately dumb.  Three exceptions earn their logic:
//
//   * GXInitTexObj and GXInitTexObjCI store the width, height and format
//     inside the GXTexObj, because HSD reads them back through the three
//     GXGetTexObj* getters and branches on them.  They are stored as u32
//     words: GXTexObj carries no alignment beyond u32, so a struct with a
//     pointer in it would be misaligned;
//   * the three matrix entry points record the matrix elements instead of the
//     address of the matrix.  A trace that says a camera loaded *some* matrix
//     is worth nothing; the numbers are the whole content of the call.

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

// The umbrella header, not the individual ones: several of Aurora's GX headers
// name enums they do not include the declaration of, and only <dolphin/gx.h>
// puts them in the right order.
#include <dolphin/gx.h>

// include/melee/port/dolphin_compat.h adapts upstream's three-argument
// GXSetArray onto Aurora's five with a function-like macro.  This file *is*
// Aurora's five-argument function, so it needs the name back -- a macro
// applied to a definition mangles it.  That only bites where the prelude
// reaches C++, which on MSVC is everywhere, because the Visual Studio
// generator does not honour a language gate on the force-include.  It is the
// one file in the project with any business undefining this.
#ifdef GXSetArray
#undef GXSetArray
#endif

namespace {

std::vector<std::string> g_trace;
// Pointers are identified by the order the trace first sees them, so that the
// text does not depend on the allocator.
std::vector<const void*> g_pointers;

std::string pointer_tag(const void* pointer)
{
    if (pointer == nullptr) {
        return "null";
    }
    for (std::size_t i = 0; i < g_pointers.size(); ++i) {
        if (g_pointers[i] == pointer) {
            return "p" + std::to_string(i);
        }
    }
    g_pointers.push_back(pointer);
    return "p" + std::to_string(g_pointers.size() - 1);
}

std::string number(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    // Negative zero and positive zero are the same number and must read the
    // same, or a trace comparison turns on which way a subtraction rounded.
    if (std::strcmp(text, "-0") == 0) {
        return "0";
    }
    return text;
}

template <typename T> std::string field(const T& value)
{
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_pointer_v<T>) {
        return pointer_tag(static_cast<const void*>(value));
    } else if constexpr (std::is_floating_point_v<T>) {
        return number(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "1" : "0";
    } else {
        static_assert(std::is_integral_v<T>, "unhandled GX argument type");
        return std::to_string(static_cast<long long>(value));
    }
}

std::string field(GXColor color)
{
    return std::to_string(color.r) + ":" + std::to_string(color.g) + ":" +
           std::to_string(color.b) + ":" + std::to_string(color.a);
}

std::string field(GXColorS10 color)
{
    return std::to_string(color.r) + ":" + std::to_string(color.g) + ":" +
           std::to_string(color.b) + ":" + std::to_string(color.a);
}

template <typename... Args> void record(const char* name, const Args&... args)
{
    std::string line(name);
    line += '(';
    std::size_t remaining = sizeof...(args);
    auto add = [&](const std::string& piece) {
        line += piece;
        if (--remaining != 0) {
            line += ", ";
        }
    };
    (add(field(args)), ...);
    line += ')';
    g_trace.push_back(std::move(line));
}

// The matrix entry points, whose argument is the matrix and not its address.
std::string matrix_field(const void* matrix, std::size_t elements)
{
    if (matrix == nullptr) {
        return "null";
    }
    const f32* values = static_cast<const f32*>(matrix);
    std::string text = "[";
    for (std::size_t i = 0; i < elements; ++i) {
        if (i != 0) {
            text += ' ';
        }
        text += number(values[i]);
    }
    text += ']';
    return text;
}

// What GXInitTexObj left behind, for the getters to answer from.  Four u32
// words, laid over the front of the object.
constexpr u32 kTexObjMagic = 0x5445584FU; // 'TEXO'

struct TexObjWords {
    u32 magic;
    u32 width;
    u32 height;
    u32 format;
};
static_assert(sizeof(TexObjWords) <= sizeof(GXTexObj),
              "the recorded fields must fit inside a real GXTexObj");

void store_tex_obj(GXTexObj* object, u16 width, u16 height, u32 format)
{
    const TexObjWords words = { kTexObjMagic, width, height, format };
    std::memcpy(object, &words, sizeof words);
}

TexObjWords load_tex_obj(const GXTexObj* object)
{
    TexObjWords words = {};
    if (object != nullptr) {
        std::memcpy(&words, object, sizeof words);
    }
    if (words.magic != kTexObjMagic) {
        // Nothing initialized this object.  Answering zero lets a test see
        // that, which is more useful than reading whatever was in the memory.
        return TexObjWords{};
    }
    return words;
}

} // namespace

namespace meleeboard::test::gx {

void reset()
{
    g_trace.clear();
    g_pointers.clear();
}

const std::vector<std::string>& trace() { return g_trace; }

std::string joined()
{
    std::string text;
    for (const std::string& line : g_trace) {
        text += line;
        text += '\n';
    }
    return text;
}

std::size_t count(const char* name)
{
    const std::size_t length = std::strlen(name);
    std::size_t total = 0;
    for (const std::string& line : g_trace) {
        if (line.size() > length && line.compare(0, length, name) == 0 &&
            line[length] == '(') {
            ++total;
        }
    }
    return total;
}

const std::string* call(const char* name, std::size_t index)
{
    const std::size_t length = std::strlen(name);
    std::size_t seen = 0;
    for (const std::string& line : g_trace) {
        if (line.size() > length && line.compare(0, length, name) == 0 &&
            line[length] == '(') {
            if (seen++ == index) {
                return &line;
            }
        }
    }
    return nullptr;
}

const std::string* at(std::size_t index)
{
    return index < g_trace.size() ? &g_trace[index] : nullptr;
}

std::vector<std::string> names()
{
    std::vector<std::string> result;
    result.reserve(g_trace.size());
    for (const std::string& line : g_trace) {
        result.push_back(line.substr(0, line.find('(')));
    }
    return result;
}

} // namespace meleeboard::test::gx

// ---------------------------------------------------------------------------
// Geometry

extern "C" void GXClearVtxDesc(void) { record("GXClearVtxDesc"); }

extern "C" void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    record("GXSetVtxDesc", attr, type);
}

extern "C" void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt,
                                GXCompType type, u8 frac)
{
    record("GXSetVtxAttrFmt", vtxfmt, attr, cnt, type, frac);
}

extern "C" void GXSetNumTexGens(u8 count) { record("GXSetNumTexGens", count); }

extern "C" void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    record("GXBegin", type, vtxfmt, nverts);
}

extern "C" void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func,
                                  GXTexGenSrc src_param, u32 mtx,
                                  GXBool normalize, u32 postmtx)
{
    record("GXSetTexCoordGen2", dst_coord, func, src_param, mtx, normalize,
           postmtx);
}

extern "C" void GXSetLineWidth(u8 width, GXTexOffset offsets)
{
    record("GXSetLineWidth", width, offsets);
}

extern "C" void GXSetPointSize(u8 size, GXTexOffset offsets)
{
    record("GXSetPointSize", size, offsets);
}

extern "C" void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride,
                           bool le)
{
    record("GXSetArray", attr, data, size, stride, le);
}

extern "C" void GXInvalidateVtxCache(void) { record("GXInvalidateVtxCache"); }

// ---------------------------------------------------------------------------
// Vertices

extern "C" void GXEnd(void) { record("GXEnd"); }

extern "C" void GXPosition2f32(f32 x, f32 y) { record("GXPosition2f32", x, y); }

extern "C" void GXPosition3f32(f32 x, f32 y, f32 z)
{
    record("GXPosition3f32", x, y, z);
}

extern "C" void GXColor4u8(u8 r, u8 g, u8 b, u8 a)
{
    record("GXColor4u8", r, g, b, a);
}

extern "C" void GXTexCoord2f32(f32 s, f32 t) { record("GXTexCoord2f32", s, t); }

extern "C" void GXTexCoord2u8(u8 s, u8 t) { record("GXTexCoord2u8", s, t); }

extern "C" void GXNormal3f32(f32 x, f32 y, f32 z)
{
    record("GXNormal3f32", x, y, z);
}

extern "C" void GXColor3u8(u8 r, u8 g, u8 b) { record("GXColor3u8", r, g, b); }

extern "C" void GXColor1u16(u16 color) { record("GXColor1u16", color); }

// The 1x8 and 1x16 forms send an index into an array set by GXSetArray,
// rather than a value.  pobj's indexed primitives are made of these.
extern "C" void GXColor1x8(u8 index) { record("GXColor1x8", index); }

extern "C" void GXColor1x16(u16 index) { record("GXColor1x16", index); }

extern "C" void GXTexCoord1u8(u8 s) { record("GXTexCoord1u8", s); }

extern "C" void GXTexCoord1x8(u8 index) { record("GXTexCoord1x8", index); }

extern "C" void GXTexCoord1x16(u16 index) { record("GXTexCoord1x16", index); }

// ---------------------------------------------------------------------------
// Display lists
//
// The list is a GX command stream, not data the caller built field by field,
// so its address and length are what a trace can say about it.

extern "C" void GXCallDisplayList(const void* list, u32 nbytes)
{
    record("GXCallDisplayList", list, nbytes);
}

// ---------------------------------------------------------------------------
// Frame buffer

extern "C" void GXCopyTex(void* dest, GXBool clear)
{
    record("GXCopyTex", dest, clear);
}

extern "C" void GXSetPixelFmt(GXPixelFmt pixel_fmt, GXZFmt16 z_fmt)
{
    record("GXSetPixelFmt", pixel_fmt, z_fmt);
}

extern "C" void GXSetTexCopyDst(u16 width, u16 height, GXTexFmt format,
                                GXBool mipmap)
{
    record("GXSetTexCopyDst", width, height, format, mipmap);
}

extern "C" void GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height)
{
    record("GXSetTexCopySrc", left, top, width, height);
}

// ---------------------------------------------------------------------------
// Textures

extern "C" void GXInitTexObj(GXTexObj* object, const void* data, u16 width,
                             u16 height, GXTexFmt format, GXTexWrapMode wrap_s,
                             GXTexWrapMode wrap_t, GXBool mipmap)
{
    record("GXInitTexObj", object, data, width, height, format, wrap_s, wrap_t,
           mipmap);
    store_tex_obj(object, width, height, format);
}

extern "C" void GXInitTexObjCI(GXTexObj* object, const void* data, u16 width,
                               u16 height, GXCITexFmt format,
                               GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                               GXBool mipmap, u32 tlut)
{
    record("GXInitTexObjCI", object, data, width, height, format, wrap_s,
           wrap_t, mipmap, tlut);
    store_tex_obj(object, width, height, format);
}

extern "C" void GXInitTexObjLOD(GXTexObj* object, GXTexFilter min_filter,
                                GXTexFilter mag_filter, f32 min_lod,
                                f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                                GXBool do_edge_lod, GXAnisotropy max_aniso)
{
    record("GXInitTexObjLOD", object, min_filter, mag_filter, min_lod, max_lod,
           lod_bias, bias_clamp, do_edge_lod, max_aniso);
}

extern "C" void GXInitTlutObj(GXTlutObj* object, const void* data,
                              GXTlutFmt format, u16 entries)
{
    record("GXInitTlutObj", object, data, format, entries);
}

extern "C" void GXInvalidateTexAll() { record("GXInvalidateTexAll"); }

extern "C" void GXLoadTexObj(GXTexObj* object, GXTexMapID id)
{
    record("GXLoadTexObj", object, id);
}

extern "C" void GXLoadTlut(const GXTlutObj* object, u32 index)
{
    record("GXLoadTlut", object, index);
}

// The getters answer from the object rather than the trace: HSD branches on
// what they return, and may read them in a loop.  They record nothing.
extern "C" GXTexFmt GXGetTexObjFmt(GXTexObj* object)
{
    return static_cast<GXTexFmt>(load_tex_obj(object).format);
}

extern "C" u16 GXGetTexObjWidth(GXTexObj* object)
{
    return static_cast<u16>(load_tex_obj(object).width);
}

extern "C" u16 GXGetTexObjHeight(GXTexObj* object)
{
    return static_cast<u16>(load_tex_obj(object).height);
}

// ---------------------------------------------------------------------------
// Lighting

extern "C" void GXInitLightAttn(GXLightObj* light, f32 a0, f32 a1, f32 a2,
                                f32 k0, f32 k1, f32 k2)
{
    record("GXInitLightAttn", light, a0, a1, a2, k0, k1, k2);
}

extern "C" void GXInitLightColor(GXLightObj* light, GXColor color)
{
    record("GXInitLightColor", light, color);
}

extern "C" void GXInitLightDir(GXLightObj* light, f32 nx, f32 ny, f32 nz)
{
    record("GXInitLightDir", light, nx, ny, nz);
}

extern "C" void GXInitLightDistAttn(GXLightObj* light, f32 ref_distance,
                                    f32 ref_brightness,
                                    GXDistAttnFn dist_func)
{
    record("GXInitLightDistAttn", light, ref_distance, ref_brightness,
           dist_func);
}

extern "C" void GXInitLightPos(GXLightObj* light, f32 x, f32 y, f32 z)
{
    record("GXInitLightPos", light, x, y, z);
}

extern "C" void GXInitLightSpot(GXLightObj* light, f32 cutoff,
                                GXSpotFn spot_func)
{
    record("GXInitLightSpot", light, cutoff, spot_func);
}

extern "C" void GXLoadLightObjImm(GXLightObj* light, GXLightID id)
{
    record("GXLoadLightObjImm", light, id);
}

extern "C" void GXSetChanAmbColor(GXChannelID channel, GXColor color)
{
    record("GXSetChanAmbColor", channel, color);
}

extern "C" void GXSetChanCtrl(GXChannelID channel, GXBool enable,
                              GXColorSrc ambient_source,
                              GXColorSrc material_source, u32 light_mask,
                              GXDiffuseFn diffuse_fn, GXAttnFn attenuation_fn)
{
    record("GXSetChanCtrl", channel, enable, ambient_source, material_source,
           light_mask, diffuse_fn, attenuation_fn);
}

extern "C" void GXSetChanMatColor(GXChannelID channel, GXColor color)
{
    record("GXSetChanMatColor", channel, color);
}

extern "C" void GXSetNumChans(u8 count) { record("GXSetNumChans", count); }

// ---------------------------------------------------------------------------
// Transform

extern "C" void GXSetProjection(const void* matrix, GXProjectionType type)
{
    g_trace.push_back("GXSetProjection(" + matrix_field(matrix, 16) + ", " +
                      field(type) + ")");
}

extern "C" void GXLoadPosMtxImm(const void* matrix, u32 id)
{
    g_trace.push_back("GXLoadPosMtxImm(" + matrix_field(matrix, 12) + ", " +
                      field(id) + ")");
}

extern "C" void GXLoadNrmMtxImm(const void* matrix, u32 id)
{
    g_trace.push_back("GXLoadNrmMtxImm(" + matrix_field(matrix, 12) + ", " +
                      field(id) + ")");
}

extern "C" void GXLoadTexMtxImm(const void* matrix, u32 id, GXTexMtxType type)
{
    // GX_MTX2x4 is eight elements; GX_MTX3x4 is twelve.
    const std::size_t elements = type == GX_MTX2x4 ? 8U : 12U;
    g_trace.push_back("GXLoadTexMtxImm(" + matrix_field(matrix, elements) +
                      ", " + field(id) + ", " + field(type) + ")");
}

extern "C" void GXSetCurrentMtx(u32 id) { record("GXSetCurrentMtx", id); }

extern "C" void GXSetViewport(f32 left, f32 top, f32 width, f32 height,
                              f32 near_z, f32 far_z)
{
    record("GXSetViewport", left, top, width, height, near_z, far_z);
}

extern "C" void GXSetViewportJitter(f32 left, f32 top, f32 width, f32 height,
                                    f32 near_z, f32 far_z, u32 field_index)
{
    record("GXSetViewportJitter", left, top, width, height, near_z, far_z,
           field_index);
}

// ---------------------------------------------------------------------------
// TEV

extern "C" void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op,
                                  GXCompare comp1, u8 ref1)
{
    record("GXSetAlphaCompare", comp0, ref0, op, comp1, ref1);
}

extern "C" void GXSetNumTevStages(u8 count)
{
    record("GXSetNumTevStages", count);
}

extern "C" void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a,
                                GXTevAlphaArg b, GXTevAlphaArg c,
                                GXTevAlphaArg d)
{
    record("GXSetTevAlphaIn", stage, a, b, c, d);
}

extern "C" void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                                GXTevScale scale, GXBool clamp,
                                GXTevRegID out_reg)
{
    record("GXSetTevAlphaOp", stage, op, bias, scale, clamp, out_reg);
}

extern "C" void GXSetTevColor(GXTevRegID id, GXColor color)
{
    record("GXSetTevColor", id, color);
}

extern "C" void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a,
                                GXTevColorArg b, GXTevColorArg c,
                                GXTevColorArg d)
{
    record("GXSetTevColorIn", stage, a, b, c, d);
}

extern "C" void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                                GXTevScale scale, GXBool clamp,
                                GXTevRegID out_reg)
{
    record("GXSetTevColorOp", stage, op, bias, scale, clamp, out_reg);
}

extern "C" void GXSetTevColorS10(GXTevRegID id, GXColorS10 color)
{
    record("GXSetTevColorS10", id, color);
}

extern "C" void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel)
{
    record("GXSetTevKAlphaSel", stage, sel);
}

extern "C" void GXSetTevKColor(GXTevKColorID id, GXColor color)
{
    record("GXSetTevKColor", id, color);
}

extern "C" void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel)
{
    record("GXSetTevKColorSel", stage, sel);
}

extern "C" void GXSetTevOp(GXTevStageID id, GXTevMode mode)
{
    record("GXSetTevOp", id, mode);
}

extern "C" void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord,
                              GXTexMapID map, GXChannelID color)
{
    record("GXSetTevOrder", stage, coord, map, color);
}

extern "C" void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel,
                                 GXTevSwapSel tex_sel)
{
    record("GXSetTevSwapMode", stage, ras_sel, tex_sel);
}

extern "C" void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red,
                                      GXTevColorChan green,
                                      GXTevColorChan blue,
                                      GXTevColorChan alpha)
{
    record("GXSetTevSwapModeTable", table, red, green, blue, alpha);
}

extern "C" void GXSetZTexture(GXZTexOp op, GXTexFmt format, u32 bias)
{
    record("GXSetZTexture", op, format, bias);
}

// ---------------------------------------------------------------------------
// Pixel

extern "C" void GXSetAlphaUpdate(GXBool enable)
{
    record("GXSetAlphaUpdate", enable);
}

extern "C" void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor,
                               GXBlendFactor dst_factor, GXLogicOp op)
{
    record("GXSetBlendMode", type, src_factor, dst_factor, op);
}

extern "C" void GXSetColorUpdate(GXBool enable)
{
    record("GXSetColorUpdate", enable);
}

extern "C" void GXSetDither(GXBool dither) { record("GXSetDither", dither); }

extern "C" void GXSetDstAlpha(GXBool enable, u8 alpha)
{
    record("GXSetDstAlpha", enable, alpha);
}

extern "C" void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio)
{
    record("GXSetFieldMode", field_mode, half_aspect_ratio);
}

extern "C" void GXSetZCompLoc(GXBool before_tex)
{
    record("GXSetZCompLoc", before_tex);
}

extern "C" void GXSetZMode(GXBool compare_enable, GXCompare func,
                           GXBool update_enable)
{
    record("GXSetZMode", compare_enable, func, update_enable);
}

// ---------------------------------------------------------------------------
// Cull and manage

extern "C" void GXSetCullMode(GXCullMode mode)
{
    record("GXSetCullMode", mode);
}

extern "C" void GXSetScissor(u32 left, u32 top, u32 width, u32 height)
{
    record("GXSetScissor", left, top, width, height);
}

extern "C" void GXPixModeSync(void) { record("GXPixModeSync"); }
