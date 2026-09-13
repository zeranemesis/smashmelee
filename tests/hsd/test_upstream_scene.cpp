#include "harness.hpp"
#include "gx_record.hpp"

// Upstream's scene graph, running against the recording GX surface.
//
// These are the units the plan called the render half, and the reason they
// could not come over before: every one of them talks to GX.  They talk to it
// here too -- gx_record.cpp just writes down what was said.  So the
// assertions are of two kinds, and both are worth having:
//
//   * what HSD computes, read back out of its own objects (a joint tree's
//     shape, the matrices it composes);
//   * what HSD sends, read out of the trace (a camera's viewport and
//     projection).
//
// The second kind is the one phase 3 of docs/PLAN.md is built on.

#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/cobj.h>
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
#include <sysdolphin/baselib/video.h>
#include <sysdolphin/baselib/wobj.h>
}

namespace gx = meleeboard::test::gx;

namespace {

constexpr double kTolerance = 1e-4;

// initialize.c keeps HSD_ObjInit static, and initialize.c is phase 1 -- it
// reaches for the OS arena.  This is the same list in the same order, and it
// is not optional: HSD_JObjSetupMatrix borrows a scratch vector from the pool
// and upstream asserts rather than returning when the pool is empty, which is
// how a host bring-up finds out it skipped this.
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

// A render mode whose framebuffer and display are the same size, so the
// camera's scale factors are exactly one and the viewport it sends is the
// viewport it was given.  cobj divides by viWidth and viHeight, so this is
// not optional decoration -- it is the video state the camera reads.
void use_square_render_mode()
{
    GXRenderModeObj* rmode = HSD_VIGetRenderMode();
    std::memset(rmode, 0, sizeof *rmode);
    rmode->fbWidth = 640;
    rmode->viWidth = 640;
    rmode->efbHeight = 480;
    rmode->viHeight = 480;
    rmode->field_rendering = 0;
}

// Pulls the bracketed numbers out of a recorded matrix line.  A golden frame
// compares as text, but the numbers a camera computes come out of the host's
// tanf, which agrees with the GameCube's to a few bits rather than exactly --
// so the shape of the frame is asserted as text and the values with a
// tolerance.  Anything integral stays exact.
std::vector<double> matrix_in(const std::string& line)
{
    std::vector<double> values;
    const std::size_t open = line.find('[');
    const std::size_t close = line.find(']', open);
    if (open == std::string::npos || close == std::string::npos) {
        return values;
    }
    std::istringstream stream(line.substr(open + 1, close - open - 1));
    double value = 0.0;
    while (stream >> value) {
        values.push_back(value);
    }
    return values;
}

std::string frame_shape()
{
    std::string text;
    for (const std::string& name : gx::names()) {
        if (!text.empty()) {
            text += ' ';
        }
        text += name;
    }
    return text;
}

HSD_Joint make_joint()
{
    HSD_Joint joint{};
    joint.scale = Vec3{ 1.0F, 1.0F, 1.0F };
    return joint;
}

} // namespace

MELEE_TEST(UpstreamJObj, LoadsATreeOfJointsAndKeepsItsShape)
{
    init_object_pools();

    // Three joints: a root with one child, and a sibling beside the child.
    // The descriptors are ordinary host memory -- no archive, so no
    // relocation, which keeps this about the tree and not about the loader.
    HSD_Joint sibling = make_joint();
    sibling.position = Vec3{ 7.0F, 0.0F, 0.0F };

    HSD_Joint child = make_joint();
    child.position = Vec3{ 0.0F, 3.0F, 0.0F };
    child.next = &sibling;

    HSD_Joint root = make_joint();
    root.position = Vec3{ 1.0F, 2.0F, 3.0F };
    root.child = &child;

    HSD_JObj* jobj = HSD_JObjLoadJoint(&root);
    REQUIRE(jobj != nullptr);

    HSD_JObj* loaded_child = HSD_JObjGetChild(jobj);
    REQUIRE(loaded_child != nullptr);
    HSD_JObj* loaded_sibling = HSD_JObjGetNext(loaded_child);
    REQUIRE(loaded_sibling != nullptr);

    // The child list ends after the sibling, and the root has no sibling of
    // its own: HSD_JObjLoadJoint is given one joint, not a list.
    CHECK(HSD_JObjGetNext(loaded_sibling) == nullptr);
    CHECK(HSD_JObjGetNext(jobj) == nullptr);
    CHECK(HSD_JObjGetChild(loaded_child) == nullptr);

    // Every joint's translation came across.
    CHECK_NEAR(jobj->translate.x, 1.0, kTolerance);
    CHECK_NEAR(jobj->translate.y, 2.0, kTolerance);
    CHECK_NEAR(jobj->translate.z, 3.0, kTolerance);
    CHECK_NEAR(loaded_child->translate.y, 3.0, kTolerance);
    CHECK_NEAR(loaded_sibling->translate.x, 7.0, kTolerance);

    HSD_JObjRemoveAll(jobj);
}

MELEE_TEST(UpstreamJObj, ComposesAChildsWorldMatrixThroughItsParent)
{
    init_object_pools();

    // The child sits three units above a root that sits at (1, 2, 3), so its
    // world translation is the sum.  HSD_JObjSetupMatrix walks up to the root
    // and back down, which is the whole point of the dirty flag.
    HSD_Joint child = make_joint();
    child.position = Vec3{ 0.0F, 3.0F, 0.0F };

    HSD_Joint root = make_joint();
    root.position = Vec3{ 1.0F, 2.0F, 3.0F };
    root.child = &child;

    HSD_JObj* jobj = HSD_JObjLoadJoint(&root);
    REQUIRE(jobj != nullptr);
    HSD_JObj* loaded_child = HSD_JObjGetChild(jobj);
    REQUIRE(loaded_child != nullptr);

    HSD_JObjSetupMatrix(loaded_child);

    // The composed matrix is the joint's own; its translation column is the
    // child's position in world space.
    Vec3 position{};
    HSD_MtxGetTranslate(loaded_child->mtx, &position);
    CHECK_NEAR(position.x, 1.0, kTolerance);
    CHECK_NEAR(position.y, 5.0, kTolerance);
    CHECK_NEAR(position.z, 3.0, kTolerance);

    // Moving the root moves the child with it, once the subtree is marked.
    Vec3 moved{ 10.0F, 0.0F, 0.0F };
    HSD_JObjSetTranslate(jobj, &moved);
    HSD_JObjSetupMatrix(loaded_child);
    HSD_MtxGetTranslate(loaded_child->mtx, &position);
    CHECK_NEAR(position.x, 10.0, kTolerance);
    CHECK_NEAR(position.y, 3.0, kTolerance);
    CHECK_NEAR(position.z, 0.0, kTolerance);

    HSD_JObjRemoveAll(jobj);
}

MELEE_TEST(UpstreamCObj, SendsItsViewportAndProjectionToGX)
{
    init_object_pools();
    use_square_render_mode();
    gx::reset();

    HSD_WObjDesc eye{};
    eye.pos = Vec3{ 0.0F, 0.0F, 10.0F };
    HSD_WObjDesc interest{};
    interest.pos = Vec3{ 0.0F, 0.0F, 0.0F };

    HSD_CObjDesc desc{};
    desc.perspective.projection_type = PROJ_PERSPECTIVE;
    desc.perspective.viewport = HSD_RectS16{ 0, 640, 0, 480 };
    desc.perspective.scissor = Scissor{ 0, 640, 0, 480 };
    desc.perspective.eyepos = &eye;
    desc.perspective.interest = &interest;
    desc.perspective.nnear = 1.0F;
    desc.perspective.ffar = 100.0F;
    desc.perspective.fov = 60.0F;
    desc.perspective.aspect = 640.0F / 480.0F;

    HSD_CObj* cobj = HSD_CObjLoadDesc(&desc);
    REQUIRE(cobj != nullptr);

    REQUIRE(HSD_CObjSetCurrent(cobj));

    // The scale factors are one, so the viewport reaches GX as it was
    // written in the descriptor.
    REQUIRE_EQ(gx::count("GXSetViewport"), 1U);
    CHECK_EQ(*gx::call("GXSetViewport"),
             std::string("GXSetViewport(0, 0, 640, 480, 0, 1)"));

    // The scissor likewise, and as integers.
    REQUIRE_EQ(gx::count("GXSetScissor"), 1U);
    CHECK_EQ(*gx::call("GXSetScissor"),
             std::string("GXSetScissor(0, 0, 640, 480)"));

    // A perspective camera sends a perspective projection, and the matrix is
    // in the trace as numbers rather than as the address of a matrix -- which
    // is what makes a recorded frame worth comparing.
    REQUIRE_EQ(gx::count("GXSetProjection"), 1U);
    const std::string& projection = *gx::call("GXSetProjection");
    CHECK(projection.find(", " + std::to_string(GX_PERSPECTIVE) + ")") !=
          std::string::npos);

    // The GameCube's perspective matrix, term by term: the near plane over
    // the half-width and half-height it subtends, and a depth range mapped
    // into [0, -1] with w carried in the last row.
    const std::vector<double> m = matrix_in(projection);
    REQUIRE_EQ(m.size(), 16U);
    const double half_height = std::tan(0.5 * 60.0 * M_PI / 180.0);
    const double half_width = half_height * (640.0 / 480.0);
    CHECK_NEAR(m[0], 1.0 / half_width, kTolerance);
    CHECK_NEAR(m[5], 1.0 / half_height, kTolerance);
    CHECK_NEAR(m[10], -1.0 / 99.0, kTolerance);
    CHECK_NEAR(m[11], -100.0 / 99.0, kTolerance);
    CHECK_NEAR(m[14], -1.0, kTolerance);
    // Everything off those terms is zero, including the last element: the
    // GameCube's projection is not an affine 4x4.
    for (std::size_t i : { 1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U, 12U, 13U, 15U }) {
        CHECK_NEAR(m[i], 0.0, kTolerance);
    }

    // And the whole frame, in order.  Setting a camera current is these three
    // calls and nothing else -- no matrix load, no state.  That is the shape
    // a ported renderer has to reproduce.
    CHECK_EQ(frame_shape(),
             std::string("GXSetViewport GXSetScissor GXSetProjection"));

    HSD_CObjEndCurrent();
}

MELEE_TEST(UpstreamCObj, JittersTheViewportForAnInterlacedMode)
{
    // The same camera in a field-rendering mode takes the other branch, and
    // the trace says so: a jittered viewport carries the field index.
    init_object_pools();
    use_square_render_mode();
    HSD_VIGetRenderMode()->field_rendering = 1;
    gx::reset();

    HSD_WObjDesc eye{};
    eye.pos = Vec3{ 0.0F, 0.0F, 10.0F };
    HSD_WObjDesc interest{};

    HSD_CObjDesc desc{};
    desc.perspective.projection_type = PROJ_PERSPECTIVE;
    desc.perspective.viewport = HSD_RectS16{ 0, 640, 0, 480 };
    desc.perspective.scissor = Scissor{ 0, 640, 0, 480 };
    desc.perspective.eyepos = &eye;
    desc.perspective.interest = &interest;
    desc.perspective.nnear = 1.0F;
    desc.perspective.ffar = 100.0F;
    desc.perspective.fov = 60.0F;
    desc.perspective.aspect = 640.0F / 480.0F;

    HSD_CObj* cobj = HSD_CObjLoadDesc(&desc);
    REQUIRE(cobj != nullptr);
    REQUIRE(HSD_CObjSetCurrent(cobj));

    CHECK_EQ(gx::count("GXSetViewport"), 0U);
    REQUIRE_EQ(gx::count("GXSetViewportJitter"), 1U);
    CHECK_EQ(*gx::call("GXSetViewportJitter"),
             std::string("GXSetViewportJitter(0, 0, 640, 480, 0, 1, 0)"));
    CHECK_EQ(frame_shape(),
             std::string("GXSetViewportJitter GXSetScissor GXSetProjection"));

    HSD_CObjEndCurrent();
}

MELEE_TEST(UpstreamGXRecorder, NumbersPointersByFirstAppearance)
{
    // The recorder is an instrument, so it gets its own test.  Addresses must
    // never reach the trace: a golden frame has to compare equal across runs,
    // hosts and allocators.
    gx::reset();

    int first = 0;
    int second = 0;
    GXCopyTex(&second, GX_FALSE);
    GXCopyTex(&first, GX_TRUE);
    GXCopyTex(&second, GX_FALSE);
    GXCopyTex(nullptr, GX_FALSE);

    CHECK_EQ(gx::joined(), std::string("GXCopyTex(p0, 0)\n"
                                       "GXCopyTex(p1, 1)\n"
                                       "GXCopyTex(p0, 0)\n"
                                       "GXCopyTex(null, 0)\n"));

    // And the numbering restarts, so one case cannot shift another's trace.
    gx::reset();
    GXCopyTex(&second, GX_FALSE);
    CHECK_EQ(gx::joined(), std::string("GXCopyTex(p0, 0)\n"));
}

MELEE_TEST(UpstreamGXRecorder, AnswersTextureDimensionsFromTheObject)
{
    // The four GX calls that return something HSD branches on.  tobj reads
    // these back rather than keeping its own copy, so a stub that recorded
    // and returned zero would send it down the wrong path.
    gx::reset();

    GXTexObj texture;
    std::memset(&texture, 0, sizeof texture);
    GXInitTexObj(&texture, nullptr, 64, 32, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP,
                 GX_FALSE);

    CHECK_EQ(GXGetTexObjWidth(&texture), static_cast<u16>(64));
    CHECK_EQ(GXGetTexObjHeight(&texture), static_cast<u16>(32));
    CHECK_EQ(GXGetTexObjFmt(&texture), GX_TF_RGB5A3);

    // Reading the dimensions is not a GX call in the trace.
    CHECK_EQ(gx::count("GXInitTexObj"), 1U);
    CHECK_EQ(gx::trace().size(), 1U);

    // An object nothing initialized answers zero rather than whatever was in
    // the memory, so a test can see that it was never set up.
    GXTexObj untouched;
    std::memset(&untouched, 0xCD, sizeof untouched);
    CHECK_EQ(GXGetTexObjWidth(&untouched), static_cast<u16>(0));
}
