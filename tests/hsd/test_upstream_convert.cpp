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

#include <algorithm>
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
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/lobj.h>
#include <sysdolphin/baselib/spline.h>
#include <sysdolphin/baselib/video.h>
#include <sysdolphin/baselib/wobj.h>
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

MELEE_TEST(UpstreamConvert, ConvertsAJointsParticleList)
{
    // A joint whose union is a particle list: two HSD_SList nodes, whose data
    // word is a packed bank-and-offset integer rather than a pointer.  Bank
    // is the bottom six bits; the offset sits above it, shifted by six.
    DatBuilder builder;
    const uint32_t second = builder.allocate(0x08);
    const uint32_t first = builder.allocate(0x08);
    builder.pointer(first + 0x00, second);
    builder.u32(first + 0x04, (7u << JOBJ_PTCL_OFFSET_SHIFT) | 5u);
    builder.u32(second + 0x04, (9u << JOBJ_PTCL_OFFSET_SHIFT) | 3u);

    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.u32(joint + 0x04, JOBJ_PTCL);
    builder.pointer(joint + 0x10, first);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->u.ptcl != nullptr);
    CHECK_EQ(converter.particle_node_count(), std::size_t(2));
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));

    // The chain, and the packed word read back the way HSD_JObjDisp reads it.
    HSD_SList* node = host->u.ptcl;
    CHECK_EQ((uint32_t) (uintptr_t) node->data & JOBJ_PTCL_BANK_MASK, 5u);
    CHECK_EQ(((uint32_t) (uintptr_t) node->data >> JOBJ_PTCL_OFFSET_SHIFT) &
                 JOBJ_PTCL_OFFSET_MASK,
             7u);
    REQUIRE(node->next != nullptr);
    node = node->next;
    CHECK_EQ((uint32_t) (uintptr_t) node->data & JOBJ_PTCL_BANK_MASK, 3u);
    CHECK_EQ(((uint32_t) (uintptr_t) node->data >> JOBJ_PTCL_OFFSET_SHIFT) &
                 JOBJ_PTCL_OFFSET_MASK,
             9u);
    CHECK(node->next == nullptr);

    // Upstream's own loader walks the list and sets the top bit of every
    // node's data word in place -- `*(u32*) &slist->data |= 0x80000000`.  The
    // packed value has to survive that, which is what pins the layout choice
    // of storing it in the low half of a host pointer.
    HSD_JObj* jobj = HSD_JObjLoadJoint(host);
    REQUIRE(jobj != nullptr);
    CHECK(jobj->u.ptcl == host->u.ptcl);
    for (HSD_SList* walk = jobj->u.ptcl; walk != nullptr; walk = walk->next) {
        CHECK(((uint32_t) (uintptr_t) walk->data & 0x80000000u) != 0);
    }
    CHECK_EQ((uint32_t) (uintptr_t) jobj->u.ptcl->data,
             0x80000000u | (7u << JOBJ_PTCL_OFFSET_SHIFT) | 5u);

    HSD_JObjRemoveAll(jobj);
}

MELEE_TEST(UpstreamConvert, ConvertsTheFourReferenceObjectTypesByFlag)
{
    // The top nibble of an robj's flags says what its union holds, and
    // upstream's own switch in HSD_RObjLoadDesc is the specification -- it
    // panics on a value it does not know, so the converter reports rather
    // than guesses.  Four types, chained off one joint.
    DatBuilder builder;

    const uint32_t target_joint = add_joint(builder, 9.0F, 0.0F, 0.0F);

    const uint32_t ik_hint = builder.allocate(0x08);
    builder.f32(ik_hint + 0x00, 12.5F);
    builder.f32(ik_hint + 0x04, 0.25F);

    const uint32_t bytecode = builder.allocate(8);
    builder.u8(bytecode + 0, 0x42);

    const uint32_t rvalues = builder.allocate(2 * 8);
    builder.u32(rvalues + 0x00, 0xABCU);
    builder.pointer(rvalues + 0x04, target_joint);
    // rvalues + 0x08 stays zero and unrelocated: the terminator.

    const uint32_t bcexp = builder.allocate(0x08);
    builder.pointer(bcexp + 0x00, bytecode);
    builder.pointer(bcexp + 0x04, rvalues);

    const uint32_t exp = builder.allocate(0x08);
    // exp + 0x00, the function pointer, stays null on purpose: see below.
    builder.pointer(exp + 0x04, rvalues);

    const uint32_t robj_exp = builder.allocate(0x0C);
    builder.u32(robj_exp + 0x04, 0x00000000U); // REFTYPE_EXP
    builder.pointer(robj_exp + 0x08, exp);

    const uint32_t robj_bytecode = builder.allocate(0x0C);
    builder.u32(robj_bytecode + 0x04, 0x30000000U); // REFTYPE_BYTECODE
    builder.pointer(robj_bytecode + 0x08, bcexp);
    builder.pointer(robj_bytecode + 0x00, robj_exp);

    const uint32_t robj_ik = builder.allocate(0x0C);
    builder.u32(robj_ik + 0x04, 0x40000000U); // REFTYPE_IKHINT
    builder.pointer(robj_ik + 0x08, ik_hint);
    builder.pointer(robj_ik + 0x00, robj_bytecode);

    const uint32_t robj_limit = builder.allocate(0x0C);
    builder.u32(robj_limit + 0x04, 0x20000000U | 7U); // REFTYPE_LIMIT, type 7
    builder.f32(robj_limit + 0x08, 1.5F);
    builder.pointer(robj_limit + 0x00, robj_ik);

    const uint32_t robj_jobj = builder.allocate(0x0C);
    builder.u32(robj_jobj + 0x04, 0x10000000U); // REFTYPE_JOBJ
    builder.pointer(robj_jobj + 0x08, target_joint);
    builder.pointer(robj_jobj + 0x00, robj_limit);

    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x3C, robj_jobj);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    CHECK_EQ(converter.error(), std::string());
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));

    HSD_RObjDesc* jobj_ref = host->robjdesc;
    REQUIRE(jobj_ref != nullptr);
    CHECK_EQ(jobj_ref->flags, 0x10000000U);
    REQUIRE(jobj_ref->u.joint != nullptr);
    CHECK_NEAR(jobj_ref->u.joint->position.x, 9.0, kTolerance);

    HSD_RObjDesc* limit = jobj_ref->next;
    REQUIRE(limit != nullptr);
    // A float by value, not a pointer -- which is why the union has to be
    // read by type rather than uniformly relocated.
    CHECK_NEAR(limit->u.limit, 1.5, kTolerance);

    HSD_RObjDesc* ik = limit->next;
    REQUIRE(ik != nullptr);
    REQUIRE(ik->u.ik_hint != nullptr);
    CHECK_NEAR(ik->u.ik_hint->bone_length, 12.5, kTolerance);
    CHECK_NEAR(ik->u.ik_hint->rotate_x, 0.25, kTolerance);

    HSD_RObjDesc* bc = ik->next;
    REQUIRE(bc != nullptr);
    REQUIRE(bc->u.bcexp != nullptr);
    REQUIRE(bc->u.bcexp->bytecode != nullptr);
    CHECK_EQ(bc->u.bcexp->bytecode[0], static_cast<u8>(0x42));
    REQUIRE(bc->u.bcexp->rvalue != nullptr);
    CHECK_EQ(bc->u.bcexp->rvalue[0].flags, 0xABCU);
    REQUIRE(bc->u.bcexp->rvalue[0].joint != nullptr);
    CHECK(bc->u.bcexp->rvalue[0].joint == jobj_ref->u.joint);
    CHECK(bc->u.bcexp->rvalue[1].joint == nullptr);

    HSD_RObjDesc* expression = bc->next;
    REQUIRE(expression != nullptr);
    REQUIRE(expression->u.exp != nullptr);
    // The function pointer stays null, deliberately.  The field holds the
    // address of a function in the console's executable; there is no host
    // value that means the same thing, and a truncated one would be a jump
    // into nothing.  Upstream already handles null here -- expLoadDesc
    // substitutes dummy_func -- so the safe answer is also upstream's own.
    CHECK(expression->u.exp->func == nullptr);
    REQUIRE(expression->u.exp->rvalue != nullptr);
    CHECK(expression->next == nullptr);
}

MELEE_TEST(UpstreamConvert, ReportsAReferenceObjectTypeUpstreamWouldPanicOn)
{
    // 0x50000000 is not one of the five HSD_RObjLoadDesc handles, and its
    // default arm calls HSD_Panic.  Converting the union by guessing would
    // turn a panic into silent wrong behavior, so the reference is reported.
    DatBuilder builder;
    const uint32_t unknown = builder.allocate(0x08);
    const uint32_t robj = builder.allocate(0x0C);
    builder.u32(robj + 0x04, 0x50000000U);
    builder.pointer(robj + 0x08, unknown);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x3C, robj);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->robjdesc != nullptr);

    REQUIRE_EQ(converter.unconverted().size(), std::size_t(1));
    CHECK_EQ(converter.unconverted()[0].target, unknown);
    CHECK_EQ(std::string(converter.unconverted()[0].kind),
             std::string("HSD_RObjDesc::u (unknown type)"));
}

MELEE_TEST(UpstreamConvert, BuildsASkinnedPrimitivesEnvelopeTable)
{
    // The skinning path.  A primitive flagged POBJ_ENVELOPE points at a
    // NULL-terminated array of runs, and each run is {joint, weight} pairs
    // ending at an entry whose joint is NULL.  Two levels of terminator, both
    // of which depend on the relocation table rather than on the bytes.
    DatBuilder builder;

    const uint32_t bone_a = add_joint(builder, 1.0F, 0.0F, 0.0F);
    const uint32_t bone_b = add_joint(builder, 0.0F, 1.0F, 0.0F);

    // Run one: both bones, then the terminator.
    const uint32_t run_one = builder.allocate(3 * 8);
    builder.pointer(run_one + 0x00, bone_a);
    builder.f32(run_one + 0x04, 0.75F);
    builder.pointer(run_one + 0x08, bone_b);
    builder.f32(run_one + 0x0C, 0.25F);
    // run_one + 0x10 stays zero and unrelocated: the terminator.

    // Run two: one bone.
    const uint32_t run_two = builder.allocate(2 * 8);
    builder.pointer(run_two + 0x00, bone_b);
    builder.f32(run_two + 0x04, 1.0F);

    const uint32_t table = builder.allocate(3 * 4);
    builder.pointer(table + 0x00, run_one);
    builder.pointer(table + 0x04, run_two);
    // table + 0x08 stays zero and unrelocated: the terminator.

    const uint32_t primitive = builder.allocate(0x18);
    builder.u16(primitive + 0x0C, 2 << 12); // POBJ_ENVELOPE
    builder.pointer(primitive + 0x14, table);

    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x0C, primitive);
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
    HSD_PObjDesc* pobj = host->u.dobjdesc->pobjdesc;
    REQUIRE(pobj != nullptr);
    REQUIRE(pobj->u.envelope_p != nullptr);

    HSD_EnvelopeDesc** table_host = pobj->u.envelope_p;
    REQUIRE(table_host[0] != nullptr);
    REQUIRE(table_host[1] != nullptr);
    CHECK(table_host[2] == nullptr);

    REQUIRE(table_host[0][0].joint != nullptr);
    CHECK_NEAR(table_host[0][0].joint->position.x, 1.0, kTolerance);
    CHECK_NEAR(table_host[0][0].weight, 0.75, kTolerance);
    REQUIRE(table_host[0][1].joint != nullptr);
    CHECK_NEAR(table_host[0][1].joint->position.y, 1.0, kTolerance);
    CHECK_NEAR(table_host[0][1].weight, 0.25, kTolerance);
    CHECK(table_host[0][2].joint == nullptr);

    CHECK(table_host[1][1].joint == nullptr);

    // The same bone reached twice is one host joint, which is what lets the
    // skinning weight the matrix the rest of the scene graph animates.
    CHECK(table_host[0][1].joint == table_host[1][0].joint);
}

MELEE_TEST(UpstreamConvert, BuildsARigidPrimitivesJoint)
{
    // POBJ_SKIN, the default: the union is a single joint whose matrix the
    // primitive is drawn in.  The joint converts through the same graph as
    // the tree it hangs off, so reaching it twice gives one object.
    DatBuilder builder;
    const uint32_t bone = add_joint(builder, 4.0F, 0.0F, 0.0F);
    const uint32_t primitive = builder.allocate(0x18);
    builder.pointer(primitive + 0x14, bone);
    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x0C, primitive);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.pointer(joint + 0x08, bone);
    builder.pointer(joint + 0x10, display_object);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));

    REQUIRE(host->u.dobjdesc != nullptr);
    REQUIRE(host->u.dobjdesc->pobjdesc != nullptr);
    REQUIRE(host->u.dobjdesc->pobjdesc->u.joint != nullptr);
    CHECK_NEAR(host->u.dobjdesc->pobjdesc->u.joint->position.x, 4.0,
               kTolerance);
    // Reached as the tree's child and as the primitive's joint -- one object.
    CHECK(host->u.dobjdesc->pobjdesc->u.joint == host->child);
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

// Lays down a spline of `type` with `numcv` control parameters, its control
// points on a straight line one unit apart, a normalized cumulative length
// table, and -- for the curved types -- a polynomial row per segment.
uint32_t add_spline(DatBuilder& builder, uint8_t type, int16_t numcv,
                    bool with_polynomials)
{
    const uint32_t points = numcv < 2 ? (uint32_t) numcv
                            : type == 1 ? 3u * (uint32_t) numcv - 2u
                            : type >= 2 ? (uint32_t) numcv + 2u
                                        : (uint32_t) numcv;
    const uint32_t cv = builder.allocate(points * 12u);
    for (uint32_t index = 0; index < points; ++index) {
        builder.f32(cv + index * 12u + 0, (float) index);
        builder.f32(cv + index * 12u + 4, 0.0F);
        builder.f32(cv + index * 12u + 8, 0.0F);
    }

    const uint32_t lengths = builder.allocate((uint32_t) numcv * 4u);
    for (int16_t index = 0; index < numcv; ++index) {
        builder.f32(lengths + (uint32_t) index * 4u,
                    (float) index / (float) (numcv - 1));
    }

    const uint32_t rows = numcv > 1 ? (uint32_t) numcv - 1u : 0u;
    uint32_t polynomials = 0;
    if (with_polynomials) {
        polynomials = builder.allocate(rows * 5u * 4u);
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < 5; ++column) {
                builder.f32(polynomials + (row * 5u + column) * 4u,
                            (float) (row * 10u + column));
            }
        }
    }

    const uint32_t spline = builder.allocate(0x18);
    builder.u8(spline + 0x00, type);
    builder.s16(spline + 0x02, numcv);
    builder.f32(spline + 0x04, 0.5F);
    builder.pointer(spline + 0x08, cv);
    builder.f32(spline + 0x0C, 12.5F);
    builder.pointer(spline + 0x10, lengths);
    if (with_polynomials) {
        builder.pointer(spline + 0x14, polynomials);
    }
    return spline;
}

MELEE_TEST(UpstreamConvert, ConvertsAJointsSpline)
{
    DatBuilder builder;
    const uint32_t spline = add_spline(builder, 3, 4, true);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.u32(joint + 0x04, JOBJ_SPLINE);
    builder.pointer(joint + 0x10, spline);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->u.spline != nullptr);
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
    CHECK_EQ(converter.spline_count(), std::size_t(1));

    HSD_Spline* curve = host->u.spline;
    CHECK_EQ((int) curve->type, 3);
    CHECK_EQ((int) curve->numcv, 4);
    CHECK_NEAR(curve->tension, 0.5, kTolerance);
    CHECK_NEAR(curve->totalLength, 12.5, kTolerance);

    // A cardinal spline reads cv[idx .. idx+3] with idx up to numcv-2, and
    // cv[numcv] at the far end: numcv + 2 points, not numcv.  All six are
    // here, in order.
    REQUIRE(curve->cv != nullptr);
    for (int index = 0; index < 6; ++index) {
        CHECK_NEAR(curve->cv[index].x, (double) index, kTolerance);
    }

    // The cumulative length table runs 0 to 1 over numcv entries.
    REQUIRE(curve->segLength != nullptr);
    CHECK_NEAR(curve->segLength[0], 0.0, kTolerance);
    CHECK_NEAR(curve->segLength[3], 1.0, kTolerance);

    // Five coefficients per segment, and numcv - 1 segments.
    REQUIRE(curve->segPoly != nullptr);
    CHECK_NEAR(curve->segPoly[0][0], 0.0, kTolerance);
    CHECK_NEAR(curve->segPoly[0][4], 4.0, kTolerance);
    CHECK_NEAR(curve->segPoly[2][3], 23.0, kTolerance);

    // And the joint loads: upstream copies the descriptor's spline pointer
    // straight across, so the host curve is what the scene graph animates on.
    HSD_JObj* jobj = HSD_JObjLoadJoint(host);
    REQUIRE(jobj != nullptr);
    CHECK(jobj->u.spline == curve);
    HSD_JObjRemoveAll(jobj);
}

MELEE_TEST(UpstreamConvert, EvaluatesAConvertedSplineWithUpstreamsOwnMath)
{
    // The end-to-end check: on-disc bytes in, upstream's own spline evaluator
    // out.  A linear spline whose four control points sit at x = 0, 1, 2, 3.
    DatBuilder builder;
    const uint32_t curve = add_spline(builder, 0, 4, false);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    builder.u32(joint + 0x04, JOBJ_SPLINE);
    builder.pointer(joint + 0x10, curve);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(joint);
    REQUIRE(host != nullptr);
    REQUIRE(host->u.spline != nullptr);

    // u is normalized over the whole curve: 0.5 lands halfway along the
    // middle segment, at x = 1.5.
    Vec3 point{};
    splGetSplinePoint(&point, host->u.spline, 0.5F);
    CHECK_NEAR(point.x, 1.5, kTolerance);

    // The ends are the first and last control points exactly, which is the
    // u == 1 branch of splGetSplinePoint reading cv[numcv - 1].
    splGetSplinePoint(&point, host->u.spline, 0.0F);
    CHECK_NEAR(point.x, 0.0, kTolerance);
    splGetSplinePoint(&point, host->u.spline, 1.0F);
    CHECK_NEAR(point.x, 3.0, kTolerance);

    // And by arc length, which goes through the cumulative table instead: the
    // table is uniform here, so half the length is half the parameter.
    splArcLengthPoint(&point, host->u.spline, 0.5F);
    CHECK_NEAR(point.x, 1.5, kTolerance);
}

MELEE_TEST(UpstreamConvert, SizesControlPointsByTheCurveType)
{
    // Four control parameters, four curve types, four different on-disc
    // control-point counts.  A converter that assumed numcv would leave
    // upstream reading past the end of the array for three of the four.
    struct Case {
        uint8_t type;
        int last_index;   // the highest index splGetSplinePoint reads
    };
    const Case cases[] = {
        { 0, 3 }, // linear:   numcv         = 4 points
        { 1, 9 }, // Bezier:   3*numcv - 2   = 10
        { 2, 5 }, // B-spline: numcv + 2     = 6
        { 3, 5 }, // cardinal: numcv + 2     = 6
    };

    for (const Case& one : cases) {
        DatBuilder builder;
        const uint32_t curve = add_spline(builder, one.type, 4, one.type != 0);
        const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
        builder.u32(joint + 0x04, JOBJ_SPLINE);
        builder.pointer(joint + 0x10, curve);

        const Archive archive = parse(builder);
        REQUIRE(archive.is_valid());

        ArchiveConverter converter(archive);
        HSD_Joint* host = converter.joint(joint);
        REQUIRE(host != nullptr);
        REQUIRE(host->u.spline != nullptr);
        REQUIRE(host->u.spline->cv != nullptr);
        // add_spline writes each point's x as its index, so the last point
        // reading back as its own index is the count being right.
        CHECK_NEAR(host->u.spline->cv[one.last_index].x,
                   (double) one.last_index, kTolerance);
    }
}

MELEE_TEST(UpstreamConvert, RefusesASplineWhosePointsRunOffTheSection)
{
    // numcv says four cardinal control parameters, which needs six points.
    // Put three at the very end of the data section, so the other three would
    // have to come from past it.
    //
    // The console would read them: the whole file is in memory, so the fourth
    // point lands in the relocation table and the curve draws with garbage.
    // A host cannot read past its own buffer, and converting only what fits
    // would hand upstream an array it indexes past -- so this refuses, and
    // says which field was short.
    DatBuilder builder;
    const uint32_t spline = builder.allocate(0x18);
    const uint32_t joint = add_joint(builder, 0.0F, 0.0F, 0.0F);
    const uint32_t cv = builder.allocate(3 * 12);

    builder.u8(spline + 0x00, 3);
    builder.s16(spline + 0x02, 4);
    builder.pointer(spline + 0x08, cv);
    builder.u32(joint + 0x04, JOBJ_SPLINE);
    builder.pointer(joint + 0x10, spline);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());
    REQUIRE_EQ(cv + 3 * 12, archive.data_size());

    ArchiveConverter converter(archive);
    CHECK(converter.joint(joint) == nullptr);
    CHECK(converter.error().find("control points running off") !=
          std::string::npos);
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
                         "GXSetTevKAlphaSel GXSetColorUpdate "
                         "GXSetAlphaUpdate GXSetDstAlpha GXSetBlendMode "
                         "GXSetZMode GXSetZCompLoc GXSetAlphaCompare "
                         "GXSetDither GXSetNumTevStages GXSetNumTexGens "
                         "GXSetNumChans GXSetChanMatColor GXSetChanCtrl "
                         "GXSetCullMode GXSetCurrentMtx GXLoadPosMtxImm "
                         "GXSetArray GXClearVtxDesc GXSetVtxDesc "
                         "GXSetVtxAttrFmt GXCallDisplayList"));

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
                         "GXSetColorUpdate GXSetAlphaUpdate GXSetDstAlpha "
                         "GXSetBlendMode GXSetZMode GXSetZCompLoc "
                         "GXSetAlphaCompare GXSetDither GXSetNumTevStages "
                         "GXSetNumTexGens GXSetNumChans GXSetChanMatColor "
                         "GXSetChanCtrl GXSetCullMode GXSetCurrentMtx "
                         "GXLoadPosMtxImm GXSetArray GXClearVtxDesc "
                         "GXSetVtxDesc GXSetVtxAttrFmt GXCallDisplayList"));

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

MELEE_TEST(UpstreamConvert, BuildsAMorphTargetsShapeSet)
{
    // POBJ_SHAPEANIM: the union is a shape set, whose two index tables hold
    // one pointer per shape.  The index runs themselves are bytes and stay in
    // the archive; only the table of addresses is rebuilt at host width.
    DatBuilder builder;

    const uint32_t indices_one = builder.allocate(4);
    builder.u8(indices_one + 0, 0x10);
    const uint32_t indices_two = builder.allocate(4);
    builder.u8(indices_two + 0, 0x20);

    const uint32_t index_table = builder.allocate(2 * 4);
    builder.pointer(index_table + 0x00, indices_one);
    builder.pointer(index_table + 0x04, indices_two);

    const uint32_t vertices = builder.allocate(3 * 4);
    const uint32_t descriptors = builder.allocate(0x18 * 2);
    builder.u32(descriptors + 0x00, 9); // GX_VA_POS
    builder.u32(descriptors + 0x04, 2); // GX_INDEX8
    builder.u16(descriptors + 0x12, 12);
    builder.pointer(descriptors + 0x14, vertices);
    builder.u32(descriptors + 0x18, 0xFF);

    const uint32_t shape_set = builder.allocate(0x1C);
    builder.u16(shape_set + 0x00, 1); // flags
    builder.u16(shape_set + 0x02, 2); // nb_shape
    builder.u32(shape_set + 0x04, 3); // nb_vertex_index
    builder.pointer(shape_set + 0x08, descriptors);
    builder.pointer(shape_set + 0x0C, index_table);

    const uint32_t primitive = builder.allocate(0x18);
    builder.u16(primitive + 0x0C, 1 << 12); // POBJ_SHAPEANIM
    builder.pointer(primitive + 0x14, shape_set);

    const uint32_t display_object = builder.allocate(0x10);
    builder.pointer(display_object + 0x0C, primitive);
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
    REQUIRE(host->u.dobjdesc->pobjdesc != nullptr);
    HSD_ShapeSetDesc* shapes = host->u.dobjdesc->pobjdesc->u.shape_set;
    REQUIRE(shapes != nullptr);

    CHECK_EQ(shapes->flags, static_cast<u16>(1));
    CHECK_EQ(shapes->nb_shape, static_cast<u16>(2));
    CHECK_EQ(shapes->nb_vertex_index, 3);
    REQUIRE(shapes->vertex_desc != nullptr);
    CHECK_EQ(shapes->vertex_desc[0].attr, GX_VA_POS);

    REQUIRE(shapes->vertex_idx_list != nullptr);
    REQUIRE(shapes->vertex_idx_list[0] != nullptr);
    REQUIRE(shapes->vertex_idx_list[1] != nullptr);
    CHECK_EQ(shapes->vertex_idx_list[0][0], static_cast<u8>(0x10));
    CHECK_EQ(shapes->vertex_idx_list[1][0], static_cast<u8>(0x20));

    // The normal side is absent in this fixture and stays null rather than
    // pointing at a table of nothing.
    CHECK(shapes->normal_desc == nullptr);
    CHECK(shapes->normal_idx_list == nullptr);
}

MELEE_TEST(UpstreamConvert, GivesEveryJointADistinctLowWord)
{
    // The fix for the collision tests/hsd/test_upstream_core.cpp demonstrates.
    // Upstream keys its ID table on `(u32) joint`, so two joint descriptors
    // whose host addresses differ only above bit 31 would be one key.  The
    // converter allocates joints from a single block whose size is a power of
    // two and whose base is aligned to that size: such a block cannot straddle
    // a boundary its own size, so the low word rises monotonically across it
    // and no two addresses in it share one.
    DatBuilder builder;
    std::vector<uint32_t> offsets;
    uint32_t previous = 0;
    for (int index = 0; index < 256; ++index) {
        const uint32_t joint =
            add_joint(builder, static_cast<float>(index), 0.0F, 0.0F);
        if (index != 0) {
            builder.pointer(previous + 0x08, joint);
        }
        previous = joint;
        offsets.push_back(joint);
    }

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_Joint* host = converter.joint(offsets.front());
    REQUIRE(host != nullptr);
    CHECK_EQ(converter.joint_count(), std::size_t(256));

    // The arena's own invariant, checked rather than assumed.
    CHECK(converter.joint_arena().low_words_are_distinct());

    // And the invariant it exists for, checked on the joints themselves.
    std::vector<u32> keys;
    for (HSD_Joint* joint = host; joint != nullptr; joint = joint->child) {
        keys.push_back(
            static_cast<u32>(reinterpret_cast<std::uintptr_t>(joint)));
    }
    REQUIRE_EQ(keys.size(), std::size_t(256));
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

MELEE_TEST(UpstreamConvert, RefusesMoreJointsThanTheArenaHolds)
{
    // The arena is one allocation on purpose -- two separately aligned blocks
    // could collide with each other in their low words, which is the very
    // thing this prevents.  So running out is a refusal with a reason, not a
    // second block.
    DatBuilder builder;
    const uint32_t first = add_joint(builder, 0.0F, 0.0F, 0.0F);
    const uint32_t second = add_joint(builder, 1.0F, 0.0F, 0.0F);
    builder.pointer(first + 0x08, second);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    // One joint's worth of arena, so the second has nowhere to go.
    ArchiveConverter converter(archive, sizeof(HSD_Joint));
    CHECK(converter.joint(first) == nullptr);
    CHECK(converter.error().find("descriptor arena") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The scene's other roots.
//
// A joint graph is geometry.  Nothing reaches the screen until a camera is
// current, and a stage carries its lighting and its fog in the same archive.
// These cases convert all three and hand them to upstream's own loaders.

namespace {

// Restores the render mode the boot installed.  The camera scales its
// viewport by the mode's dimensions, and one earlier case fabricates a mode
// of its own, so this is stated rather than inherited.
void use_console_render_mode()
{
    *HSD_VIGetRenderMode() = GXNtsc480IntDf;
}

// An eye at z = 10 looking at the origin, 640x480, near 1, far 100 -- the
// same camera tests/hsd/test_upstream_scene.cpp builds by hand.
uint32_t add_camera(DatBuilder& builder, uint16_t projection_type)
{
    const uint32_t eye = builder.allocate(0x14);
    builder.f32(eye + 0x04, 0.0F);
    builder.f32(eye + 0x08, 0.0F);
    builder.f32(eye + 0x0C, 10.0F);

    const uint32_t interest = builder.allocate(0x14);

    const uint32_t camera = builder.allocate(0x40);
    builder.u16(camera + 0x04, 0); // flags: roll rather than an up vector
    builder.u16(camera + 0x06, projection_type);
    builder.s16(camera + 0x08, 0);   // viewport xmin
    builder.s16(camera + 0x0A, 640); // xmax
    builder.s16(camera + 0x0C, 0);   // ymin
    builder.s16(camera + 0x0E, 480); // ymax
    builder.u16(camera + 0x10, 0);   // scissor left
    builder.u16(camera + 0x12, 640); // right
    builder.u16(camera + 0x14, 0);   // top
    builder.u16(camera + 0x16, 480); // bottom
    builder.pointer(camera + 0x18, eye);
    builder.pointer(camera + 0x1C, interest);
    builder.f32(camera + 0x20, 0.0F);   // roll
    builder.f32(camera + 0x28, 1.0F);   // near
    builder.f32(camera + 0x2C, 100.0F); // far

    if (projection_type == PROJ_PERSPECTIVE) {
        builder.f32(camera + 0x30, 60.0F);
        builder.f32(camera + 0x34, 640.0F / 480.0F);
    } else {
        builder.f32(camera + 0x30, 3.0F);  // top
        builder.f32(camera + 0x34, -3.0F); // bottom
        builder.f32(camera + 0x38, -4.0F); // left
        builder.f32(camera + 0x3C, 4.0F);  // right
    }
    return camera;
}

} // namespace

MELEE_TEST(UpstreamScene, ConvertsACameraIndistinguishablyFromAHandBuiltOne)
{
    init_object_pools();
    use_console_render_mode();

    // The hand-built descriptor first, recorded.  This is the same camera
    // test_upstream_scene.cpp asserts term by term, so the numbers are
    // already pinned; what this case adds is that the converted one produces
    // exactly the same frame.
    HSD_WObjDesc eye{};
    eye.pos = Vec3{ 0.0F, 0.0F, 10.0F };
    HSD_WObjDesc interest{};
    HSD_CObjDesc built{};
    built.perspective.projection_type = PROJ_PERSPECTIVE;
    built.perspective.viewport = HSD_RectS16{ 0, 640, 0, 480 };
    built.perspective.scissor = Scissor{ 0, 640, 0, 480 };
    built.perspective.eyepos = &eye;
    built.perspective.interest = &interest;
    built.perspective.nnear = 1.0F;
    built.perspective.ffar = 100.0F;
    built.perspective.fov = 60.0F;
    built.perspective.aspect = 640.0F / 480.0F;

    meleeboard::test::gx::reset();
    HSD_CObj* by_hand = HSD_CObjLoadDesc(&built);
    REQUIRE(by_hand != nullptr);
    REQUIRE(HSD_CObjSetCurrent(by_hand));
    const std::string hand_built = meleeboard::test::gx::joined();
    HSD_CObjEndCurrent();

    // Now the same camera out of an archive.
    DatBuilder builder;
    const uint32_t offset = add_camera(builder, PROJ_PERSPECTIVE);
    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_CObjDesc* converted = converter.camera(offset);
    REQUIRE(converted != nullptr);
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
    // The eye and the interest are two separate world objects, both built.
    CHECK_EQ(converter.world_object_count(), std::size_t(2));
    REQUIRE(converted->common.eyepos != nullptr);
    CHECK_NEAR(converted->common.eyepos->pos.z, 10.0, kTolerance);

    meleeboard::test::gx::reset();
    HSD_CObj* from_disc = HSD_CObjLoadDesc(converted);
    REQUIRE(from_disc != nullptr);
    REQUIRE(HSD_CObjSetCurrent(from_disc));

    // Byte for byte the same frame: viewport, scissor and projection matrix.
    // Any field read from the wrong offset would move a number here.
    CHECK_EQ(meleeboard::test::gx::joined(), hand_built);

    HSD_CObjEndCurrent();
}

MELEE_TEST(UpstreamScene, ConvertsEachCameraProjectionIntoItsOwnUnionArm)
{
    // Perspective puts two floats at +0x30; frustum and ortho put four.  A
    // converter that read the wrong arm would give a camera that draws with
    // a field-of-view where a clipping plane belongs.
    {
        DatBuilder builder;
        const uint32_t offset = add_camera(builder, PROJ_PERSPECTIVE);
        const Archive archive = parse(builder);
        ArchiveConverter converter(archive);
        HSD_CObjDesc* camera = converter.camera(offset);
        REQUIRE(camera != nullptr);
        CHECK_NEAR(camera->perspective.fov, 60.0, kTolerance);
        CHECK_NEAR(camera->perspective.aspect, 640.0 / 480.0, kTolerance);
    }
    for (uint16_t type : { (uint16_t) PROJ_FRUSTUM, (uint16_t) PROJ_ORTHO }) {
        DatBuilder builder;
        const uint32_t offset = add_camera(builder, type);
        const Archive archive = parse(builder);
        ArchiveConverter converter(archive);
        HSD_CObjDesc* camera = converter.camera(offset);
        REQUIRE(camera != nullptr);
        CHECK_NEAR(camera->frustum.top, 3.0, kTolerance);
        CHECK_NEAR(camera->frustum.bottom, -3.0, kTolerance);
        CHECK_NEAR(camera->frustum.left, -4.0, kTolerance);
        CHECK_NEAR(camera->frustum.right, 4.0, kTolerance);
    }
}

MELEE_TEST(UpstreamScene, ReportsACameraProjectionUpstreamWouldAssertOn)
{
    // CObjLoad's switch has no default but an assertion, so a projection type
    // it does not know is reported rather than silently read as one of the
    // three.
    DatBuilder builder;
    const uint32_t offset = add_camera(builder, 9);
    const Archive archive = parse(builder);
    ArchiveConverter converter(archive);

    HSD_CObjDesc* camera = converter.camera(offset);
    REQUIRE(camera != nullptr);
    CHECK_EQ((int) camera->common.projection_type, 9);
    REQUIRE_EQ(converter.unconverted().size(), std::size_t(1));
    CHECK_EQ(std::string(converter.unconverted()[0].kind),
             std::string("HSD_CObjDesc projection (unknown type)"));
}

MELEE_TEST(UpstreamScene, ConvertsALightChainAndEachUnionShape)
{
    init_object_pools();

    // Three lights in one chain: an infinite one that reads nothing from the
    // union, a point light with raw attenuation coefficients, and a spot
    // light with distance attenuation and a cutoff.
    DatBuilder builder;

    const uint32_t attn = builder.allocate(0x18);
    for (int index = 0; index < 6; ++index) {
        builder.f32(attn + (uint32_t) index * 4, (float) (index + 1));
    }
    const uint32_t spot = builder.allocate(0x14);
    builder.f32(spot + 0x00, 45.0F); // cutoff
    builder.u32(spot + 0x04, 2);     // spot_func
    builder.f32(spot + 0x08, 0.5F);  // ref_br
    builder.f32(spot + 0x0C, 20.0F); // ref_dist
    builder.u32(spot + 0x10, 1);     // dist_func

    const uint32_t position = builder.allocate(0x14);
    builder.f32(position + 0x04, 1.0F);
    builder.f32(position + 0x08, 2.0F);
    builder.f32(position + 0x0C, 3.0F);
    const uint32_t interest = builder.allocate(0x14);

    const uint32_t third = builder.allocate(0x1C);
    builder.u16(third + 0x08, LOBJ_SPOT | LOBJ_DIFFUSE);
    builder.u16(third + 0x0A, 0); // distance attenuation, not raw
    builder.pointer(third + 0x10, position);
    builder.pointer(third + 0x14, interest);
    builder.pointer(third + 0x18, spot);

    const uint32_t second = builder.allocate(0x1C);
    builder.u16(second + 0x08, LOBJ_POINT);
    builder.u16(second + 0x0A, LOBJ_LIGHT_ATTN);
    builder.pointer(second + 0x10, position);
    builder.pointer(second + 0x18, attn);
    builder.pointer(second + 0x04, third);

    const uint32_t first = builder.allocate(0x1C);
    builder.u16(first + 0x08, LOBJ_INFINITE | LOBJ_DIFFUSE);
    builder.u8(first + 0x0C, 0x10);
    builder.u8(first + 0x0D, 0x20);
    builder.u8(first + 0x0E, 0x30);
    builder.u8(first + 0x0F, 0xFF);
    builder.pointer(first + 0x10, position);
    builder.pointer(first + 0x04, second);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_LightDesc* head = converter.light(first);
    REQUIRE(head != nullptr);
    CHECK_EQ(converter.light_count(), std::size_t(3));
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
    // Two world objects for three lights: all three name the same position,
    // and converting one offset twice gives the same pointer -- here as
    // everywhere else in this converter.
    CHECK_EQ(converter.world_object_count(), std::size_t(2));

    CHECK_EQ((int) head->color.r, 0x10);
    CHECK_EQ((int) head->color.g, 0x20);
    CHECK_EQ((int) head->color.b, 0x30);
    CHECK_EQ((int) head->color.a, 0xFF);
    REQUIRE(head->position != nullptr);
    CHECK_NEAR(head->position->pos.y, 2.0, kTolerance);
    // An infinite light's union is never read by LObjLoad, so it stays null
    // rather than being guessed at from a size nothing states.
    CHECK(head->u.p == nullptr);

    REQUIRE(head->next != nullptr);
    HSD_LightDesc* point = head->next;
    CHECK_EQ((int) (point->flags & LOBJ_TYPE_MASK), LOBJ_POINT);
    REQUIRE(point->u.attn != nullptr);
    CHECK_NEAR(point->u.attn->a0, 1.0, kTolerance);
    CHECK_NEAR(point->u.attn->k2, 6.0, kTolerance);

    REQUIRE(point->next != nullptr);
    HSD_LightDesc* spotlight = point->next;
    CHECK_EQ((int) (spotlight->flags & LOBJ_TYPE_MASK), LOBJ_SPOT);
    REQUIRE(spotlight->u.spot != nullptr);
    CHECK_NEAR(spotlight->u.spot->cutoff, 45.0, kTolerance);
    CHECK_EQ(spotlight->u.spot->spot_func, 2u);
    CHECK_NEAR(spotlight->u.spot->ref_dist, 20.0, kTolerance);
    CHECK_EQ(spotlight->u.spot->dist_func, 1u);
    CHECK(spotlight->next == nullptr);

    // And upstream's own loader builds the chain: three HSD_LObj, in order,
    // each carrying the descriptor's colour.
    HSD_LObj* lobj = HSD_LObjLoadDesc(head);
    REQUIRE(lobj != nullptr);
    CHECK_EQ((int) lobj->color.g, 0x20);
    REQUIRE(lobj->next != nullptr);
    REQUIRE(lobj->next->next != nullptr);
    CHECK(lobj->next->next->next == nullptr);

    HSD_LObjRemoveAll(lobj);
}

MELEE_TEST(UpstreamScene, ConvertsTheFogAndItsAdjustmentTable)
{
    DatBuilder builder;

    const uint32_t adjust = builder.allocate(0x44);
    builder.u16(adjust + 0x00, 320); // center
    builder.u16(adjust + 0x02, 640); // width
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            builder.f32(adjust + 0x04 + (row * 4 + column) * 4,
                        (float) (row * 4 + column));
        }
    }

    const uint32_t fog = builder.allocate(0x14);
    builder.u32(fog + 0x00, GX_FOG_LIN);
    builder.pointer(fog + 0x04, adjust);
    builder.f32(fog + 0x08, 10.0F);
    builder.f32(fog + 0x0C, 200.0F);
    builder.u8(fog + 0x10, 0x80);
    builder.u8(fog + 0x11, 0x90);
    builder.u8(fog + 0x12, 0xA0);
    builder.u8(fog + 0x13, 0xFF);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    ArchiveConverter converter(archive);
    HSD_FogDesc* host = converter.fog(fog);
    REQUIRE(host != nullptr);
    CHECK_EQ(host->type, (u32) GX_FOG_LIN);
    CHECK_NEAR(host->start, 10.0, kTolerance);
    CHECK_NEAR(host->end, 200.0, kTolerance);
    CHECK_EQ((int) host->color.r, 0x80);
    CHECK_EQ((int) host->color.a, 0xFF);

    // The adjustment table carries a Mtx44 -- four rows of four, not the 3x4
    // the rest of HSD uses.  Reading it as a Mtx would drop the last row and
    // shift every term after the twelfth.
    REQUIRE(host->fogadjdesc != nullptr);
    CHECK_EQ((int) host->fogadjdesc->center, 320);
    CHECK_EQ((int) host->fogadjdesc->width, 640);
    CHECK_NEAR(host->fogadjdesc->mtx[0][0], 0.0, kTolerance);
    CHECK_NEAR(host->fogadjdesc->mtx[2][3], 11.0, kTolerance);
    CHECK_NEAR(host->fogadjdesc->mtx[3][3], 15.0, kTolerance);
}

MELEE_TEST(UpstreamScene, BuildsEveryModelInASceneFromItsPublicSymbol)
{
    // How a real archive is entered: a public symbol names a scene root, the
    // root's models list is a NULL-terminated array of model descriptors, and
    // each descriptor's first field is a joint.  That much of the layout is
    // validated against GALE01's own MnMaAll.dat; the camera, light and fog
    // lists beside it are not, which is why they are reached by offset.
    DatBuilder builder;
    const uint32_t first_root = add_joint(builder, 1.0F, 0.0F, 0.0F);
    const uint32_t first_child = add_joint(builder, 0.0F, 2.0F, 0.0F);
    builder.pointer(first_root + 0x08, first_child);
    const uint32_t second_root = add_joint(builder, 5.0F, 0.0F, 0.0F);

    const uint32_t first_model = builder.allocate(0x14);
    builder.pointer(first_model + 0x00, first_root);
    const uint32_t second_model = builder.allocate(0x14);
    builder.pointer(second_model + 0x00, second_root);

    const uint32_t models = builder.allocate(3 * 4);
    builder.pointer(models + 0, first_model);
    builder.pointer(models + 4, second_model);
    // The third entry stays the zero the allocator wrote and is never
    // relocated, which is what terminates the array.

    const uint32_t scene = builder.allocate(0x10);
    builder.pointer(scene + 0x00, models);
    builder.symbol("sceneRoot", scene);

    const Archive archive = parse(builder);
    REQUIRE(archive.is_valid());

    const auto joint_offsets = archive.scene_model_joints("sceneRoot");
    REQUIRE(joint_offsets.has_value());
    REQUIRE_EQ(joint_offsets->size(), std::size_t(2));
    CHECK_EQ((*joint_offsets)[0], first_root);
    CHECK_EQ((*joint_offsets)[1], second_root);

    ArchiveConverter converter(archive);
    std::vector<HSD_Joint*> hosts;
    for (uint32_t offset : *joint_offsets) {
        HSD_Joint* host = converter.joint(offset);
        REQUIRE(host != nullptr);
        hosts.push_back(host);
    }

    // Three joints across two models, and each model's tree intact.
    CHECK_EQ(converter.joint_count(), std::size_t(3));
    CHECK_EQ(converter.unconverted().size(), std::size_t(0));
    REQUIRE(hosts[0]->child != nullptr);
    CHECK_NEAR(hosts[0]->child->position.y, 2.0, kTolerance);
    CHECK(hosts[1]->child == nullptr);
    CHECK_NEAR(hosts[1]->position.x, 5.0, kTolerance);

    CHECK(!archive.scene_model_joints("missing").has_value());
}
