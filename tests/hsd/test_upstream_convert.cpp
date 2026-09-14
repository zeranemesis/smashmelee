#include "harness.hpp"
#include "dat_builder.hpp"
#include "gx_record.hpp"

// The first *32b converter, and the first time in this project that the
// game's own loader is handed a real archive.
//
// A DAT is built here in the console's layout -- big-endian, 32-bit offsets,
// an explicit relocation table -- converted into host-sized structures, and
// given to upstream's HSD_JObjLoadJoint.  What comes back is upstream's own
// joint tree, built from bytes in the shape the disc stores them.

#include <cstring>
#include <string>
#include <vector>

#include <melee/port/dolphin_compat.h>
#include <melee/sysdolphin/baselib/archive.hpp>

#include "archive_convert.hpp"
#include "gx_array_registry.hpp"

extern "C" {
#include <sysdolphin/baselib/aobj.h>
#include <dolphin/mtx.h>
#include <sysdolphin/baselib/displayfunc.h>
#include <sysdolphin/baselib/fobj.h>
#include <sysdolphin/baselib/id.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/list.h>
#include <sysdolphin/baselib/mtx.h>
#include <sysdolphin/baselib/objalloc.h>
#include <sysdolphin/baselib/robj.h>
#include <sysdolphin/baselib/shadow.h>
#include <sysdolphin/baselib/tev.h>
}

using meleeboard::hsd::Archive;
using meleeboard::hsd::ArchiveConverter;
using meleeboard::test::DatBuilder;

namespace {

constexpr double kTolerance = 1e-5;
constexpr uint32_t kJointSize = 0x40;

void init_object_pools()
{
    HSD_ListInitAllocData();
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    HSD_IDInitAllocData();
    HSD_VecInitAllocData();
    HSD_MtxInitAllocData();
    HSD_RObjInitAllocData();
    HSD_RenderInitAllocData();
    HSD_ShadowInitAllocData();
    HSD_ZListInitAllocData();
}

uint32_t add_joint(DatBuilder& builder, float x, float y, float z)
{
    const uint32_t joint = builder.allocate(kJointSize);
    builder.f32(joint + 0x20, 1.0F); // scale
    builder.f32(joint + 0x24, 1.0F);
    builder.f32(joint + 0x28, 1.0F);
    builder.f32(joint + 0x2C, x); // position
    builder.f32(joint + 0x30, y);
    builder.f32(joint + 0x34, z);
    return joint;
}

Archive parse(const DatBuilder& builder)
{
    Archive archive;
    archive.parse(builder.build());
    return archive;
}

} // namespace

MELEE_TEST(UpstreamConvert, BuildsAHostJointTreeFromOnDiscBytes)
{
    DatBuilder builder;
    const uint32_t root = add_joint(builder, 1.0F, 2.0F, 3.0F);
    const uint32_t child = add_joint(builder, 0.0F, 3.0F, 0.0F);
    const uint32_t sibling = add_joint(builder, 7.0F, 0.0F, 0.0F);
    builder.pointer(root + 0x08, child);
    builder.pointer(child + 0x0C, sibling);
    builder.u32(root + 0x04, JOBJ_SKELETON_ROOT);
    builder.symbol("scene_root_joint", root);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(root);
    REQUIRE(host != nullptr);
    CHECK_EQ(converter.error(), std::string());
    CHECK_EQ(converter.joint_count(), std::size_t(3));

    CHECK_EQ(host->flags, static_cast<u32>(JOBJ_SKELETON_ROOT));
    CHECK_NEAR(host->position.x, 1.0, kTolerance);
    CHECK_NEAR(host->position.y, 2.0, kTolerance);
    CHECK_NEAR(host->position.z, 3.0, kTolerance);
    CHECK_NEAR(host->scale.x, 1.0, kTolerance);

    REQUIRE(host->child != nullptr);
    CHECK(host->next == nullptr);
    CHECK_NEAR(host->child->position.y, 3.0, kTolerance);
    REQUIRE(host->child->next != nullptr);
    CHECK_NEAR(host->child->next->position.x, 7.0, kTolerance);
    CHECK(host->child->next->next == nullptr);
    CHECK(host->child->child == nullptr);

    // Nothing in this tree points at a structure the converter cannot build.
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
}

MELEE_TEST(UpstreamConvert, GivesOneOnDiscJointOneHostAddress)
{
    // HSD keys its ID table on a descriptor's address and reference-counts by
    // identity, so a joint reached twice has to come back as one object.  Two
    // parents sharing a child is the case that would otherwise split it.
    DatBuilder builder;
    const uint32_t shared = add_joint(builder, 5.0F, 0.0F, 0.0F);
    const uint32_t second = add_joint(builder, 0.0F, 0.0F, 0.0F);
    const uint32_t first = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(first + 0x08, shared);
    builder.pointer(first + 0x0C, second);
    builder.pointer(second + 0x08, shared);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(first);
    REQUIRE(host != nullptr);
    REQUIRE(host->child != nullptr);
    REQUIRE(host->next != nullptr);
    REQUIRE(host->next->child != nullptr);

    CHECK(host->child == host->next->child);
    CHECK_EQ(converter.joint_count(), std::size_t(3));

    // And asking again returns the same pointer rather than converting twice.
    CHECK(converter.joint(shared) == host->child);
    CHECK_EQ(converter.joint_count(), std::size_t(3));
}

MELEE_TEST(UpstreamConvert, SeparatesARelocatedZeroFromANullPointer)
{
    // The reason there is no wire struct here.  Both of these joints hold a
    // zero word in their child field; one is named in the relocation table
    // and one is not, and nothing inside the structure says which.  The first
    // points at data-section offset zero, the second is a null pointer.
    DatBuilder builder;
    // Offset zero has to be a joint for the first case to mean anything, so
    // it is allocated first.
    const uint32_t at_zero = add_joint(builder, 9.0F, 0.0F, 0.0F);
    REQUIRE_EQ(at_zero, 0U);
    const uint32_t relocated = add_joint(builder, 0.0F, 0.0F, 0.0F);
    const uint32_t null_child = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(relocated + 0x08, at_zero);
    // null_child's child field is left as the zero the allocator wrote, and
    // never recorded as a relocation.

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* with_child = converter.joint(relocated);
    REQUIRE(with_child != nullptr);
    REQUIRE(with_child->child != nullptr);
    CHECK_NEAR(with_child->child->position.x, 9.0, kTolerance);

    HSD_Joint* without = converter.joint(null_child);
    REQUIRE(without != nullptr);
    CHECK(without->child == nullptr);
}

MELEE_TEST(UpstreamConvert, RecordsWhatItCannotBuildYet)
{
    // A primitive whose union names a shape set or an envelope table.  Those
    // are still to come, so the field is left null -- but the reference is
    // reported, because a model that silently loses its skinning looks
    // exactly like a broken renderer.
    DatBuilder builder;
    const uint32_t envelopes = builder.allocate(0x10);
    const uint32_t primitive = builder.allocate(0x18);
    builder.pointer(primitive + 0x14, envelopes);
    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x0C, primitive);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->u.dobjdesc != nullptr);
    REQUIRE(host->u.dobjdesc->pobjdesc != nullptr);
    CHECK(host->u.dobjdesc->pobjdesc->u.joint == nullptr);

    REQUIRE_EQ(converter.unconverted().size(), std::size_t(1));
    CHECK_EQ(converter.unconverted()[0].holder, primitive);
    CHECK_EQ(converter.unconverted()[0].target, envelopes);
    CHECK_EQ(std::string(converter.unconverted()[0].kind),
             std::string("HSD_PObjDesc::u"));
}

MELEE_TEST(UpstreamConvert, ConvertsThePixelEngineDescriptorByValue)
{
    // Twelve bytes, every one of them a u8, so the structure is the same size
    // on the console and here -- which matters, because MObjLoad copies it
    // with memcpy(sizeof(HSD_PEDesc)) rather than field by field.
    DatBuilder builder;
    const uint32_t pedesc = builder.allocate(12);
    for (uint32_t index = 0; index < 12; ++index) {
        builder.u8(pedesc + index, static_cast<uint8_t>(index + 1));
    }
    const uint32_t material_object = builder.allocate(0x18);
    builder.pointer(material_object + 0x14, pedesc);
    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x08, material_object);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->u.dobjdesc != nullptr);
    REQUIRE(host->u.dobjdesc->mobjdesc != nullptr);
    HSD_PEDesc* pe = host->u.dobjdesc->mobjdesc->pedesc;
    REQUIRE(pe != nullptr);

    CHECK_EQ(pe->flags, static_cast<u8>(1));
    CHECK_EQ(pe->ref0, static_cast<u8>(2));
    CHECK_EQ(pe->dst_alpha, static_cast<u8>(4));
    CHECK_EQ(pe->src_factor, static_cast<u8>(6));
    CHECK_EQ(pe->alpha_comp1, static_cast<u8>(12));

    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
}

MELEE_TEST(UpstreamConvert, LeavesRenderDescAloneWithoutReportingIt)
{
    // HSD_MObjDesc::renderdesc appears exactly once in upstream's tree -- its
    // own declaration in mobj.h.  Nothing reads it, so leaving it null is
    // correct, and listing it as unconverted would put something in that list
    // that can never be cleared.  A caller is told the list must be empty
    // before it renders; that only holds if everything in it is real work.
    DatBuilder builder;
    const uint32_t renderdesc = builder.allocate(0x10);
    const uint32_t material_object = builder.allocate(0x18);
    builder.pointer(material_object + 0x10, renderdesc);
    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x08, material_object);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->u.dobjdesc != nullptr);
    REQUIRE(host->u.dobjdesc->mobjdesc != nullptr);
    CHECK(host->u.dobjdesc->mobjdesc->renderdesc == nullptr);
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
}

MELEE_TEST(UpstreamConvert, StillRecordsASplineJointAsUnconverted)
{
    // A joint whose union is a spline rather than a display object.  Which of
    // the three the union holds is decided by the joint's flags, and only the
    // display object is built.
    DatBuilder builder;
    const uint32_t spline = builder.allocate(0x18);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.u32(joint + 0x04, JOBJ_SPLINE);
    builder.pointer(joint + 0x10, spline);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    CHECK(host->u.spline == nullptr);

    REQUIRE_EQ(converter.unconverted().size(), std::size_t(1));
    CHECK_EQ(converter.unconverted()[0].target, spline);
    CHECK_EQ(std::string(converter.unconverted()[0].kind),
             std::string("HSD_Spline"));
}

MELEE_TEST(UpstreamConvert, RefusesAJointOutsideTheDataSection)
{
    DatBuilder builder;
    add_joint(builder, 0.0F, 0.0F, 0.0F);
    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    CHECK(converter.joint(archive.data_size()) == nullptr);
    CHECK(converter.error().find("outside the archive") != std::string::npos);
}

MELEE_TEST(UpstreamConvert, UpstreamsOwnLoaderAcceptsTheConvertedTree)
{
    // The whole point.  Bytes in the shape the disc stores them go in, and
    // upstream's HSD_JObjLoadJoint -- the game's own code, unmodified --
    // builds its joint tree out of them.
    init_object_pools();

    DatBuilder builder;
    const uint32_t root = add_joint(builder, 1.0F, 2.0F, 3.0F);
    const uint32_t child = add_joint(builder, 0.0F, 3.0F, 0.0F);
    const uint32_t sibling = add_joint(builder, 7.0F, 0.0F, 0.0F);
    builder.pointer(root + 0x08, child);
    builder.pointer(child + 0x0C, sibling);
    builder.symbol("scene_root_joint", root);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());
    const auto root_offset = archive.public_symbol_offset("scene_root_joint");
    REQUIRE(root_offset.has_value());

    ArchiveConverter converter(archive);
    HSD_Joint* descriptor = converter.joint(*root_offset);
    REQUIRE(descriptor != nullptr);

    HSD_JObj* jobj = HSD_JObjLoadJoint(descriptor);
    REQUIRE(jobj != nullptr);

    HSD_JObj* loaded_child = HSD_JObjGetChild(jobj);
    REQUIRE(loaded_child != nullptr);
    HSD_JObj* loaded_sibling = HSD_JObjGetNext(loaded_child);
    REQUIRE(loaded_sibling != nullptr);

    CHECK_NEAR(jobj->translate.x, 1.0, kTolerance);
    CHECK_NEAR(jobj->translate.y, 2.0, kTolerance);
    CHECK_NEAR(jobj->translate.z, 3.0, kTolerance);
    CHECK_NEAR(loaded_child->translate.y, 3.0, kTolerance);
    CHECK_NEAR(loaded_sibling->translate.x, 7.0, kTolerance);

    // And the tree composes: the child sits three units above a root at
    // (1, 2, 3), so its world position is the sum -- computed by upstream's
    // matrix code, from a descriptor built out of on-disc bytes.
    HSD_JObjSetupMatrix(loaded_child);
    Vec3 world{};
    HSD_MtxGetTranslate(loaded_child->mtx, &world);
    CHECK_NEAR(world.x, 1.0, kTolerance);
    CHECK_NEAR(world.y, 5.0, kTolerance);
    CHECK_NEAR(world.z, 3.0, kTolerance);

    HSD_JObjRemoveAll(jobj);
}

MELEE_TEST(UpstreamConvert, ConvertsAJointsDisplayObject)
{
    // The chain a joint with geometry hangs off: a display object, the
    // material object beside it, and the primitive that carries the vertex
    // descriptors and the display list.
    DatBuilder builder;

    const uint32_t vertices = builder.allocate(3 * 3 * 4);
    const float positions[9] = { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                 0.0F, 0.0F, 1.0F, 0.0F };
    for (uint32_t index = 0; index < 9; ++index) {
        builder.f32(vertices + index * 4, positions[index]);
    }

    const uint32_t descriptors = builder.allocate(0x18 * 2);
    builder.u32(descriptors + 0x00, 9); // GX_VA_POS
    builder.u32(descriptors + 0x04, 2); // GX_INDEX8
    builder.u32(descriptors + 0x08, 1); // GX_POS_XYZ
    builder.u32(descriptors + 0x0C, 4); // GX_F32
    builder.u16(descriptors + 0x12, 12);
    builder.pointer(descriptors + 0x14, vertices);
    builder.u32(descriptors + 0x18, 0xFF); // GX_VA_NULL terminator

    const uint32_t display_list = builder.allocate(32, 32);
    builder.u8(display_list + 0, 0x98); // GX_TRIANGLESTRIP | GX_VTXFMT0
    builder.u8(display_list + 1, 0x00);
    builder.u8(display_list + 2, 0x03); // three vertices
    builder.u8(display_list + 3, 0x00);
    builder.u8(display_list + 4, 0x01);
    builder.u8(display_list + 5, 0x02);

    const uint32_t material = builder.allocate(0x14);
    builder.u8(material + 0x04, 0x11); // diffuse r
    builder.u8(material + 0x05, 0x22);
    builder.u8(material + 0x06, 0x33);
    builder.u8(material + 0x07, 0x44);
    builder.f32(material + 0x0C, 0.5F);  // alpha
    builder.f32(material + 0x10, 50.0F); // shininess

    const uint32_t material_object = builder.allocate(0x18);
    builder.u32(material_object + 0x04, 0x1234); // rendermode
    builder.pointer(material_object + 0x0C, material);

    const uint32_t primitive = builder.allocate(0x18);
    builder.pointer(primitive + 0x08, descriptors);
    builder.u16(primitive + 0x0E, 1); // one 32-byte display-list unit
    builder.pointer(primitive + 0x10, display_list);

    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x08, material_object);
    builder.pointer(display_object + 0x0C, primitive);

    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    CHECK_EQ(converter.error(), std::string());

    // Nothing in this model is now unconverted.
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));

    REQUIRE(host->u.dobjdesc != nullptr);
    HSD_DObjDesc* dobj = host->u.dobjdesc;
    CHECK(dobj->next == nullptr);

    REQUIRE(dobj->mobjdesc != nullptr);
    CHECK_EQ(dobj->mobjdesc->rendermode, 0x1234U);
    REQUIRE(dobj->mobjdesc->mat != nullptr);
    CHECK_EQ(dobj->mobjdesc->mat->diffuse.r, static_cast<u8>(0x11));
    CHECK_EQ(dobj->mobjdesc->mat->diffuse.a, static_cast<u8>(0x44));
    CHECK_NEAR(dobj->mobjdesc->mat->alpha, 0.5, kTolerance);
    CHECK_NEAR(dobj->mobjdesc->mat->shininess, 50.0, kTolerance);

    REQUIRE(dobj->pobjdesc != nullptr);
    HSD_PObjDesc* pobj = dobj->pobjdesc;
    CHECK_EQ(pobj->n_display, static_cast<u16>(1));
    CHECK(pobj->display != nullptr);
    CHECK_EQ(pobj->display[0], static_cast<u8>(0x98));

    // The vertex descriptor list crossed as a block, terminator included.
    REQUIRE(pobj->verts != nullptr);
    CHECK_EQ(pobj->verts[0].attr, GX_VA_POS);
    CHECK_EQ(pobj->verts[0].attr_type, GX_INDEX8);
    CHECK_EQ(pobj->verts[0].comp_type, GX_F32);
    CHECK_EQ(pobj->verts[0].stride, static_cast<u16>(12));
    CHECK_EQ(pobj->verts[1].attr, GX_VA_NULL);

    // Vertex data is not a structure, so it stays in the archive and is
    // addressed where it lies -- still big-endian, which is why the extent
    // and the byte order are reported rather than assumed.
    REQUIRE_EQ(converter.vertex_arrays().size(), std::size_t(1));
    CHECK(converter.vertex_arrays()[0].base == pobj->verts[0].vertex);
    CHECK(converter.vertex_arrays()[0].extent >= 36U);
}

namespace {

// The model the draw test uses: one joint, one display object, one material,
// one indexed triangle strip of three vertices.  Returned as the joint's
// offset in the archive `builder` is building.
uint32_t add_triangle_model(DatBuilder& builder)
{
    const uint32_t vertices = builder.allocate(3 * 3 * 4);
    const float positions[9] = { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                 0.0F, 0.0F, 1.0F, 0.0F };
    for (uint32_t index = 0; index < 9; ++index) {
        builder.f32(vertices + index * 4, positions[index]);
    }

    const uint32_t descriptors = builder.allocate(0x18 * 2);
    builder.u32(descriptors + 0x00, 9); // GX_VA_POS
    builder.u32(descriptors + 0x04, 2); // GX_INDEX8
    builder.u32(descriptors + 0x08, 1); // GX_POS_XYZ
    builder.u32(descriptors + 0x0C, 4); // GX_F32
    builder.u16(descriptors + 0x12, 12);
    builder.pointer(descriptors + 0x14, vertices);
    builder.u32(descriptors + 0x18, 0xFF);

    const uint32_t display_list = builder.allocate(32, 32);
    builder.u8(display_list + 0, 0x98);
    builder.u8(display_list + 2, 0x03);
    builder.u8(display_list + 4, 0x01);
    builder.u8(display_list + 5, 0x02);

    const uint32_t material = builder.allocate(0x14);
    builder.u8(material + 0x04, 0xFF);
    builder.u8(material + 0x05, 0xFF);
    builder.u8(material + 0x06, 0xFF);
    builder.u8(material + 0x07, 0xFF);
    builder.f32(material + 0x0C, 1.0F);
    builder.f32(material + 0x10, 50.0F);

    const uint32_t material_object = builder.allocate(0x18);
    builder.pointer(material_object + 0x0C, material);

    const uint32_t primitive = builder.allocate(0x18);
    builder.pointer(primitive + 0x08, descriptors);
    builder.u16(primitive + 0x0E, 1);
    builder.pointer(primitive + 0x10, display_list);

    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x08, material_object);
    builder.pointer(display_object + 0x0C, primitive);

    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    // JOBJ_OPA is the transparency bit HSD_JObjDispDObj tests before it will
    // draw anything at all.
    builder.u32(joint + 0x04, JOBJ_OPA);
    builder.pointer(joint + 0x10, display_object);
    return joint;
}

} // namespace

MELEE_TEST(UpstreamConvert, DrawsAConvertedModelThroughTheGamesOwnDisplayPath)
{
    // The first frame in this project drawn by Melee's own code.
    //
    // An archive in the console's layout goes in; upstream's jobj, dobj, mobj
    // and pobj walk it and talk to GX; and the recorder writes down what they
    // said.  Nothing here is a reimplementation of anything.
    init_object_pools();

    DatBuilder builder;
    const uint32_t joint = add_triangle_model(builder);
    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* descriptor = converter.joint(joint);
    REQUIRE(descriptor != nullptr);
    REQUIRE_EQ(converter.unconverted().size(), std::size_t(0));

    // The vertex arrays the converter found are what answer GXSetArray's two
    // extra arguments.  Registering them is the caller's job, explicitly.
    meleeboard::hsd::forget_vertex_arrays();
    meleeboard::hsd::register_vertex_arrays(converter);

    HSD_JObj* jobj = HSD_JObjLoadJoint(descriptor);
    REQUIRE(jobj != nullptr);
    REQUIRE(HSD_JObjGetDObj(jobj) != nullptr);

    Mtx view;
    PSMTXIdentity(view);

    meleeboard::test::gx::reset();
    HSD_JObjDisp(jobj, view, HSD_TRSP_OPA, 0);

    // Something was drawn.
    CHECK(!meleeboard::test::gx::trace().empty());
    CHECK_EQ(meleeboard::test::gx::count("GXCallDisplayList"), 1U);

    // The vertex array reached GX as the bytes in the archive, with the
    // stride the descriptor named, the length the registry answered, and a
    // byte order of big-endian -- because nothing converted the array, and
    // Aurora's backend has to be told that.
    REQUIRE_EQ(meleeboard::test::gx::count("GXSetArray"), 1U);
    const std::string& set_array =
        *meleeboard::test::gx::call("GXSetArray");
    CHECK(set_array.find("GXSetArray(9, p") == 0);
    const uint32_t extent = converter.vertex_arrays()[0].extent;
    CHECK(set_array.find(", " + std::to_string(extent) + ", 12, 0)") !=
          std::string::npos);
    CHECK(extent >= 36U);

    // The position matrix is the joint's, composed with the view -- the
    // identity here, so the joint's own matrix reaches GX unchanged.
    REQUIRE_EQ(meleeboard::test::gx::count("GXLoadPosMtxImm"), 1U);
    CHECK_EQ(*meleeboard::test::gx::call("GXLoadPosMtxImm"),
             std::string("GXLoadPosMtxImm([1 0 0 0 0 1 0 0 0 0 1 0], 0)"));

    // And the frame in full, in order.  This is the golden trace phase 3 of
    // docs/PLAN.md is built on: a TEV stage, the pixel-engine state, one
    // lighting channel, the position matrix, the vertex binding, and the
    // draw.  Every line of it came out of upstream's code.  If a change to
    // the converter, the loader or the SDK boundary moves any of it, this is
    // what says so.
    std::string shape;
    for (const std::string& name : meleeboard::test::gx::names()) {
        if (!shape.empty()) {
            shape += ' ';
        }
        shape += name;
    }
    CHECK_EQ(shape,
             std::string("GXPixModeSync GXSetTevKColor GXSetTevColor "
                         "GXPixModeSync GXSetTevOrder GXSetTevColorOp "
                         "GXSetTevColorIn GXSetTevAlphaOp GXSetTevAlphaIn "
                         "GXSetTevSwapMode GXSetTevKColorSel "
                         "GXSetTevKAlphaSel GXSetColorUpdate GXSetBlendMode "
                         "GXSetZMode GXSetZCompLoc GXSetAlphaCompare "
                         "GXSetNumTevStages GXSetNumTexGens GXSetNumChans "
                         "GXSetChanMatColor GXSetChanCtrl GXSetCurrentMtx "
                         "GXLoadPosMtxImm GXSetArray GXClearVtxDesc "
                         "GXSetVtxDesc GXSetVtxAttrFmt GXCallDisplayList"));

    HSD_JObjRemoveAll(jobj);
}

MELEE_TEST(UpstreamConvert, ConvertsATextureAndEverythingUnderIt)
{
    DatBuilder builder;

    // 8x8 RGB5A3: four 4x4 tiles of 32 bytes.
    const uint32_t pixels = builder.allocate(4 * 32, 32);
    builder.u8(pixels + 0, 0xAB);

    const uint32_t image = builder.allocate(0x18);
    builder.pointer(image + 0x00, pixels);
    builder.u16(image + 0x04, 8);
    builder.u16(image + 0x06, 8);
    builder.u32(image + 0x08, 5); // GX_TF_RGB5A3
    builder.u32(image + 0x0C, 0);
    builder.f32(image + 0x10, 0.0F);
    builder.f32(image + 0x14, 3.0F);

    const uint32_t entries = builder.allocate(16 * 2, 32);
    const uint32_t palette = builder.allocate(0x10);
    builder.pointer(palette + 0x00, entries);
    builder.u32(palette + 0x04, 1); // GX_TL_RGB565
    builder.u32(palette + 0x08, 0x5A5A);
    builder.u16(palette + 0x0C, 16);

    const uint32_t lod = builder.allocate(0x10);
    builder.u32(lod + 0x00, 1); // GX_LINEAR
    builder.f32(lod + 0x04, -0.5F);
    builder.u8(lod + 0x08, 1);
    builder.u8(lod + 0x09, 1);
    builder.u32(lod + 0x0C, 2); // GX_ANISO_4

    const uint32_t tev = builder.allocate(0x20);
    for (uint32_t index = 0; index < 16; ++index) {
        builder.u8(tev + index, static_cast<uint8_t>(index + 1));
    }
    builder.u8(tev + 0x10, 0x10); // konst.r
    builder.u8(tev + 0x14, 0x20); // tev0.r
    builder.u8(tev + 0x18, 0x30); // tev1.r
    builder.u32(tev + 0x1C, 0xFFFFFFFFU);

    const uint32_t texture = builder.allocate(0x5C);
    builder.u32(texture + 0x08, 0); // GX_TEXMAP0
    builder.u32(texture + 0x0C, 4); // GX_TG_TEX0
    builder.f32(texture + 0x1C, 1.0F); // scale
    builder.f32(texture + 0x20, 2.0F);
    builder.f32(texture + 0x24, 3.0F);
    builder.u32(texture + 0x34, 0); // GX_CLAMP
    builder.u32(texture + 0x38, 1); // GX_REPEAT
    builder.u8(texture + 0x3C, 2);  // repeat_s
    builder.u8(texture + 0x3D, 3);  // repeat_t
    builder.u32(texture + 0x40, 0x12345678U);
    builder.f32(texture + 0x44, 0.25F);
    builder.u32(texture + 0x48, 1); // GX_LINEAR
    builder.pointer(texture + 0x4C, image);
    builder.pointer(texture + 0x50, palette);
    builder.pointer(texture + 0x54, lod);
    builder.pointer(texture + 0x58, tev);

    const uint32_t material_object = builder.allocate(0x18);
    builder.pointer(material_object + 0x08, texture);
    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x08, material_object);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    CHECK_EQ(converter.error(), std::string());
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));

    REQUIRE(host->u.dobjdesc != nullptr);
    REQUIRE(host->u.dobjdesc->mobjdesc != nullptr);
    HSD_TObjDesc* tobj = host->u.dobjdesc->mobjdesc->texdesc;
    REQUIRE(tobj != nullptr);

    CHECK_EQ(tobj->id, GX_TEXMAP0);
    CHECK_EQ(tobj->src, GX_TG_TEX0);
    CHECK_EQ(tobj->wrap_s, GX_CLAMP);
    CHECK_EQ(tobj->wrap_t, GX_REPEAT);
    CHECK_EQ(tobj->repeat_s, static_cast<u8>(2));
    CHECK_EQ(tobj->repeat_t, static_cast<u8>(3));
    CHECK_EQ(tobj->blend_flags, 0x12345678U);
    CHECK_NEAR(tobj->blending, 0.25, kTolerance);
    CHECK_EQ(tobj->magFilt, GX_LINEAR);
    CHECK_NEAR(tobj->scale.x, 1.0, kTolerance);
    CHECK_NEAR(tobj->scale.z, 3.0, kTolerance);
    CHECK(tobj->next == nullptr);

    REQUIRE(tobj->imagedesc != nullptr);
    CHECK_EQ(tobj->imagedesc->width, static_cast<u16>(8));
    CHECK_EQ(tobj->imagedesc->height, static_cast<u16>(8));
    CHECK_EQ(tobj->imagedesc->format, GX_TF_RGB5A3);
    CHECK_NEAR(tobj->imagedesc->maxLOD, 3.0, kTolerance);
    REQUIRE(tobj->imagedesc->image_ptr != nullptr);
    // The pixels are the archive's own bytes, in the console's tiled layout.
    CHECK_EQ(static_cast<const u8*>(tobj->imagedesc->image_ptr)[0],
             static_cast<u8>(0xAB));

    REQUIRE(tobj->tlutdesc != nullptr);
    CHECK_EQ(tobj->tlutdesc->fmt, GX_TL_RGB565);
    CHECK_EQ(tobj->tlutdesc->tlut_name, 0x5A5AU);
    CHECK_EQ(tobj->tlutdesc->n_entries, static_cast<u16>(16));
    CHECK(tobj->tlutdesc->lut != nullptr);

    REQUIRE(tobj->lod != nullptr);
    CHECK_EQ(tobj->lod->minFilt, GX_LINEAR);
    CHECK_NEAR(tobj->lod->LODBias, -0.5, kTolerance);
    CHECK_EQ(tobj->lod->bias_clamp, static_cast<GXBool>(1));
    CHECK_EQ(tobj->lod->edgeLODEnable, static_cast<GXBool>(1));
    CHECK_EQ(tobj->lod->max_anisotropy, GX_ANISO_4);

    REQUIRE(tobj->tev != nullptr);
    // Sixteen selector bytes in declaration order, starting at color_op.
    CHECK_EQ(tobj->tev->color_op, static_cast<u8>(1));
    CHECK_EQ(tobj->tev->alpha_op, static_cast<u8>(2));
    CHECK_EQ(tobj->tev->alpha_d, static_cast<u8>(16));
    CHECK_EQ(tobj->tev->konst.r, static_cast<u8>(0x10));
    CHECK_EQ(tobj->tev->tev0.r, static_cast<u8>(0x20));
    CHECK_EQ(tobj->tev->tev1.r, static_cast<u8>(0x30));
    CHECK_EQ(tobj->tev->active, 0xFFFFFFFFU);
}

MELEE_TEST(UpstreamConvert, DrawsATexturedModelThroughTheGamesOwnDisplayPath)
{
    // The same model as the untextured draw, with a texture on its material.
    // Upstream's tobj and texp decide what that means for GX; this asserts
    // what they decided.
    init_object_pools();

    DatBuilder builder;
    const uint32_t joint = add_triangle_model(builder);

    const uint32_t pixels = builder.allocate(4 * 32, 32);
    const uint32_t image = builder.allocate(0x18);
    builder.pointer(image + 0x00, pixels);
    builder.u16(image + 0x04, 8);
    builder.u16(image + 0x06, 8);
    builder.u32(image + 0x08, 5); // GX_TF_RGB5A3

    const uint32_t texture = builder.allocate(0x5C);
    builder.u32(texture + 0x08, 0); // GX_TEXMAP0
    builder.u32(texture + 0x0C, 4); // GX_TG_TEX0
    builder.f32(texture + 0x1C, 1.0F);
    builder.f32(texture + 0x20, 1.0F);
    builder.f32(texture + 0x24, 1.0F);
    builder.u32(texture + 0x34, 0); // GX_CLAMP
    builder.u32(texture + 0x38, 0);
    // Upstream asserts on these: MakeTextureMtx divides the repeat count by
    // the scale, so a texture with a zero repeat is not a texture with no
    // repeats, it is a malformed one.  That is the format telling us, in
    // tobj.c's own words, rather than a rule taken from a document.
    builder.u8(texture + 0x3C, 1);
    builder.u8(texture + 0x3D, 1);
    builder.u32(texture + 0x48, 1); // GX_LINEAR magnification
    builder.pointer(texture + 0x4C, image);

    // add_triangle_model put the material object two allocations before the
    // primitive; rather than reach back for it, the texture is attached by
    // walking the converted graph below.
    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* descriptor = converter.joint(joint);
    REQUIRE(descriptor != nullptr);
    REQUIRE(descriptor->u.dobjdesc != nullptr);
    REQUIRE(descriptor->u.dobjdesc->mobjdesc != nullptr);

    // Convert the texture on its own and hang it on the material.  A material
    // that names its texture in the archive gets this for free; doing it by
    // hand here keeps the fixture readable.
    HSD_TObjDesc* tobj = converter.texture(texture);
    REQUIRE(tobj != nullptr);
    descriptor->u.dobjdesc->mobjdesc->texdesc = tobj;

    meleeboard::hsd::forget_vertex_arrays();
    meleeboard::hsd::register_vertex_arrays(converter);

    HSD_JObj* jobj = HSD_JObjLoadJoint(descriptor);
    REQUIRE(jobj != nullptr);

    Mtx view;
    PSMTXIdentity(view);

    meleeboard::test::gx::reset();
    HSD_JObjDisp(jobj, view, HSD_TRSP_OPA, 0);

    // The texture reached GX: an object built from the image's dimensions and
    // format, and loaded onto a texture map.
    REQUIRE_EQ(meleeboard::test::gx::count("GXInitTexObj"), 1U);
    const std::string& init = *meleeboard::test::gx::call("GXInitTexObj");
    CHECK(init.find(", 8, 8, 5, 0, 0, 0)") != std::string::npos);
    CHECK_EQ(meleeboard::test::gx::count("GXLoadTexObj"), 1U);

    // And the draw still happened.
    CHECK_EQ(meleeboard::test::gx::count("GXCallDisplayList"), 1U);

    // A texture stage is now in the TEV chain, and a texture coordinate is
    // generated for it -- neither of which the untextured frame had.
    CHECK(meleeboard::test::gx::count("GXSetTexCoordGen2") >= 1U);
    REQUIRE_EQ(meleeboard::test::gx::count("GXSetNumTexGens"), 1U);
    CHECK_EQ(*meleeboard::test::gx::call("GXSetNumTexGens"),
             std::string("GXSetNumTexGens(1)"));

    // The frame in full, in order.  Two of its argument values are
    // deliberately not asserted, and the reason is a real finding rather than
    // a limitation of this test.
    //
    // HSD_TExpSetReg in texp.c declares `GXColor reg[8]` and never
    // initializes it, then writes only the components a constant names before
    // handing the whole colour to GX.  So GXSetTevKColor's alpha and
    // GXSetTevColor's rgb are read before they are written -- in the shipped
    // game, not in this port.  Compiling upstream with
    // -ftrivial-auto-var-init=pattern turns both into 0xAA, which is how that
    // was established rather than guessed.
    //
    // A golden frame therefore cannot include those components on any host.
    // See docs/PLAN.md on fidelity.
    std::string shape;
    for (const std::string& name : meleeboard::test::gx::names()) {
        if (!shape.empty()) {
            shape += ' ';
        }
        shape += name;
    }
    CHECK_EQ(shape,
             std::string("GXLoadTexMtxImm GXInitTexObj GXInitTexObjLOD "
                         "GXLoadTexObj GXSetTexCoordGen2 GXPixModeSync "
                         "GXSetTevKColor GXSetTevColor GXPixModeSync "
                         "GXSetTevOrder GXSetTevColorOp GXSetTevColorIn "
                         "GXSetTevAlphaOp GXSetTevAlphaIn GXSetTevSwapMode "
                         "GXSetTevKColorSel GXSetTevKAlphaSel "
                         "GXSetNumTevStages GXSetNumTexGens GXSetNumChans "
                         "GXSetCurrentMtx GXLoadPosMtxImm GXSetArray "
                         "GXClearVtxDesc GXSetVtxDesc GXSetVtxAttrFmt "
                         "GXCallDisplayList"));

    // The texture matrix is loaded before anything else, and the texture
    // coordinate is generated through it.
    CHECK_EQ(*meleeboard::test::gx::call("GXLoadTexMtxImm"),
             std::string("GXLoadTexMtxImm([1 0 0 0 0 1 0 0 0 0 1 0], 64, 0)"));
    CHECK_EQ(*meleeboard::test::gx::call("GXSetTexCoordGen2"),
             std::string("GXSetTexCoordGen2(0, 1, 4, 60, 0, 64)"));

    // The vertex array now reaches GX with a real extent, answered by the
    // registry from what the load recorded.
    REQUIRE_EQ(meleeboard::test::gx::count("GXSetArray"), 1U);
    CHECK(meleeboard::test::gx::call("GXSetArray")->find(", 12, 0)") !=
          std::string::npos);

    HSD_JObjRemoveAll(jobj);
}
