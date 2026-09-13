#include "harness.hpp"
#include "dat_builder.hpp"

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

extern "C" {
#include <sysdolphin/baselib/aobj.h>
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
    // A joint with geometry.  The display object is not converted -- that is
    // the next converter -- and the field is left null, but the reference is
    // reported so nobody mistakes an unbuilt scene for an empty one.
    DatBuilder builder;
    const uint32_t display_object = builder.allocate(0x10);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    CHECK(host->u.dobjdesc == nullptr);

    REQUIRE_EQ(converter.unconverted().size(), std::size_t(1));
    CHECK_EQ(converter.unconverted()[0].holder, joint);
    CHECK_EQ(converter.unconverted()[0].target, display_object);
    CHECK_EQ(std::string(converter.unconverted()[0].kind),
             std::string("HSD_DObjDesc"));
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
