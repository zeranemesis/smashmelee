#include "harness.hpp"

#include "animation.hpp"
#include "dat_builder.hpp"
#include "hsd_fixtures.hpp"

#include <melee/sysdolphin/baselib/aobj.h>
#include <melee/sysdolphin/baselib/archive.hpp>
#include <melee/sysdolphin/baselib/fobj.h>

#include <vector>

using meleeboard::hsd::Archive;
using meleeboard::hsd::HostAnimation;
using meleeboard::test::DatBuilder;
namespace fixtures = meleeboard::test::hsd;

namespace {

// HSD_A_J_TRAX: the joint translation channel on the X axis.
constexpr uint8_t kTranslationX = 5;
constexpr uint8_t kTwoKeyPack = 0x10;

const std::vector<uint8_t> kConstantSeven{ HSD_A_OP_CON, kTwoKeyPack,
                                           7, 1, 9, 1 };

} // namespace

MELEE_TEST(Animation, MaterializesAJointWithItsBytecode)
{
    DatBuilder builder;
    const uint32_t channel = fixtures::add_anim_channel(
        builder, kTranslationX, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
        kConstantSeven);
    const uint32_t object =
        fixtures::add_anim_object(builder, AOBJ_LOOP, 20.0F, channel);
    const uint32_t joint = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(builder, joint, object);
    builder.symbol("testAnim", joint);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostAnimation animation;
    REQUIRE(animation.load(archive, "testAnim"));
    REQUIRE(animation.joints().size() == 1);

    const auto& host_joint = animation.joints().front();
    CHECK(host_joint.has_object);
    CHECK_EQ(host_joint.object.flags, static_cast<uint32_t>(AOBJ_LOOP));
    CHECK_EQ(host_joint.object.end_frame, 20.0F);
    REQUIRE(host_joint.object.channels.size() == 1);

    const auto& host_channel = host_joint.object.channels.front();
    CHECK_EQ(host_channel.start_frame, 0);
    CHECK_EQ(host_channel.object_type, kTranslationX);
    CHECK_EQ(host_channel.value_fraction, HSD_A_FRAC_U8);
    CHECK(host_channel.bytecode == kConstantSeven);
}

MELEE_TEST(Animation, LinksAHierarchyByIndex)
{
    DatBuilder builder;
    const uint32_t root = fixtures::add_anim_joint(builder);
    const uint32_t child = fixtures::add_anim_joint(builder);
    const uint32_t sibling = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_child(builder, root, child);
    fixtures::set_anim_joint_sibling(builder, child, sibling);
    builder.symbol("tree", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostAnimation animation;
    REQUIRE(animation.load(archive, "tree"));
    REQUIRE(animation.joints().size() == 3);

    // Relationships are stored as indices into joints(), never as archive
    // offsets, so the hierarchy stays valid on a 64-bit host.
    const auto& joints = animation.joints();
    CHECK_EQ(joints[0].source_offset, root);
    REQUIRE(joints[0].child >= 0);
    const auto child_index = static_cast<size_t>(joints[0].child);
    CHECK_EQ(joints[child_index].source_offset, child);
    REQUIRE(joints[child_index].sibling >= 0);
    CHECK_EQ(joints[static_cast<size_t>(joints[child_index].sibling)]
                 .source_offset,
             sibling);
    CHECK_EQ(joints[0].sibling, -1);
}

MELEE_TEST(Animation, LoadsFromARelocatedOffsetWithoutASymbol)
{
    // Melee's data tables (MnSelectChrDataTable among them) point at anonymous
    // animation trees, so the offset entry point must behave like the symbol
    // one and must reject a root outside the data section.
    DatBuilder builder;
    const uint32_t channel = fixtures::add_anim_channel(
        builder, kTranslationX, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
        kConstantSeven);
    const uint32_t object =
        fixtures::add_anim_object(builder, AOBJ_LOOP, 20.0F, channel);
    const uint32_t joint = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(builder, joint, object);
    builder.symbol("testAnim", joint);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostAnimation by_symbol;
    HostAnimation by_offset;
    HostAnimation out_of_range;
    REQUIRE(by_symbol.load(archive, "testAnim"));
    REQUIRE(by_offset.load_at(archive, joint));
    CHECK_EQ(by_offset.joints().size(), by_symbol.joints().size());
    CHECK(!out_of_range.load_at(archive, archive.data_size()));
    CHECK(!out_of_range.load(archive, "notAnAnimation"));
}

MELEE_TEST(Animation, RejectsBytecodeOutsideTheArchive)
{
    DatBuilder builder;
    const uint32_t stream = builder.allocate(4);
    const uint32_t channel = builder.allocate(fixtures::kAnimChannelSize);
    // A length that runs past the end of the data section must be refused
    // rather than copied out of bounds.
    builder.u32(channel + 0x04, builder.data_size() + 64);
    builder.f32(channel + 0x08, 0.0F);
    builder.u8(channel + 0x0C, kTranslationX);
    builder.pointer(channel + 0x10, stream);
    const uint32_t object =
        fixtures::add_anim_object(builder, 0, 1.0F, channel);
    const uint32_t joint = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(builder, joint, object);
    builder.symbol("broken", joint);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostAnimation animation;
    CHECK(!animation.load(archive, "broken"));
    CHECK(animation.joints().empty());
    CHECK(!animation.last_error().empty());
}

MELEE_TEST(Animation, RejectsACyclicChannelChain)
{
    DatBuilder builder;
    const uint32_t first = fixtures::add_anim_channel(
        builder, kTranslationX, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
        kConstantSeven);
    const uint32_t second = fixtures::add_anim_channel(
        builder, kTranslationX, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
        kConstantSeven);
    fixtures::set_anim_channel_next(builder, first, second);
    fixtures::set_anim_channel_next(builder, second, first);
    const uint32_t object =
        fixtures::add_anim_object(builder, 0, 1.0F, first);
    const uint32_t joint = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(builder, joint, object);
    builder.symbol("cyclic", joint);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostAnimation animation;
    CHECK(!animation.load(archive, "cyclic"));
}

MELEE_TEST(Animation, RejectsAnUnsafeObjectPointer)
{
    DatBuilder builder;
    const uint32_t joint = fixtures::add_anim_joint(builder);
    // An unrelocated GameCube address is not a host-safe offset.
    builder.u32(joint + 0x08, 0x80402010);
    builder.symbol("unsafe", joint);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostAnimation animation;
    CHECK(!animation.load(archive, "unsafe"));
}
