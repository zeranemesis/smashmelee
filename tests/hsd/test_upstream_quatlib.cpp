#include "harness.hpp"

// Upstream's quaternion library.
//
// This unit is why the Windows job went red twice: mtx.c reaches for M_PI and
// quatlib.c for M_PI_2, neither of which is in standard C, and MSVC's
// <math.h> withholds both until _USE_MATH_DEFINES is defined ahead of it.
// Compiling is now covered by the build itself, so these cases cover the
// arithmetic behind the constants -- the interpolator's three branches, and
// the two independent routes into a quaternion that ought to agree.

#include <cmath>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/quatlib.h>
}

namespace {

// The host's libm and the GameCube's agree to about this much.
constexpr double kTolerance = 1e-5;

constexpr float kHalfSqrt2 = 0.70710678F;

// A +90 degree rotation about Z, as the SDK lays a matrix out: three rows of
// four, rotation in the left 3x3 and translation in the last column.
void rotation_about_z(Mtx matrix, float radians)
{
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix[0][0] = c;    matrix[0][1] = -s;   matrix[0][2] = 0.0F;
    matrix[1][0] = s;    matrix[1][1] = c;    matrix[1][2] = 0.0F;
    matrix[2][0] = 0.0F; matrix[2][1] = 0.0F; matrix[2][2] = 1.0F;
    matrix[0][3] = matrix[1][3] = matrix[2][3] = 0.0F;
}

} // namespace

MELEE_TEST(UpstreamQuatLib, InterpolatesAlongTheShortArc)
{
    // Two rotations about Z, 0 and 90 degrees.  Halfway between them is 45,
    // and this is the ordinary branch: acosf, sinf and a division by sin.
    Quaternion p = { 0.0F, 0.0F, 0.0F, 1.0F };
    Quaternion q = { 0.0F, 0.0F, kHalfSqrt2, kHalfSqrt2 };
    Quaternion out{};

    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &q, &out, 0.5F), 0);
    CHECK_NEAR(out.x, 0.0, kTolerance);
    CHECK_NEAR(out.y, 0.0, kTolerance);
    CHECK_NEAR(out.z, std::sin(M_PI / 8.0), kTolerance);
    CHECK_NEAR(out.w, std::cos(M_PI / 8.0), kTolerance);

    // The endpoints come back unchanged, which is what keeps an animation
    // that interpolates every frame from drifting.
    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &q, &out, 0.0F), 0);
    CHECK_NEAR(out.z, 0.0, kTolerance);
    CHECK_NEAR(out.w, 1.0, kTolerance);

    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &q, &out, 1.0F), 0);
    CHECK_NEAR(out.z, kHalfSqrt2, kTolerance);
    CHECK_NEAR(out.w, kHalfSqrt2, kTolerance);
}

MELEE_TEST(UpstreamQuatLib, BlendsLinearlyWhenTheArcVanishes)
{
    // Within 1e-10 of each other the division by sin(theta) would lose all
    // precision, so the interpolation falls back to a linear blend.  Equal
    // quaternions are the degenerate case of that, and must come back whole.
    Quaternion p = { 0.0F, 0.0F, kHalfSqrt2, kHalfSqrt2 };
    Quaternion q = p;
    Quaternion out{};

    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &q, &out, 0.25F), 0);
    CHECK_NEAR(out.x, 0.0, kTolerance);
    CHECK_NEAR(out.y, 0.0, kTolerance);
    CHECK_NEAR(out.z, kHalfSqrt2, kTolerance);
    CHECK_NEAR(out.w, kHalfSqrt2, kTolerance);
}

MELEE_TEST(UpstreamQuatLib, CollapsesBetweenOpposedQuaternions)
{
    // The third branch, and the one M_PI_2 is in: the two quaternions are
    // opposed, every arc between them is the same length, and there is no
    // shortest one to take.  Upstream writes a perpendicular quaternion into
    // `out` -- and then interpolates between p and the `q` it was handed,
    // not the perpendicular, so that write only survives when the caller
    // passes the same quaternion as both q and out.
    //
    // The game does not: lb_00B0.c calls this with three distinct
    // quaternions.  So on the path the game takes, the weights fall on p and
    // -p and cancel, and the result is the zero quaternion.  That is upstream's
    // behavior, not a rounding artifact, and a reimplementation that "fixed"
    // it would diverge from the disc.
    Quaternion p = { 0.0F, 0.0F, 0.0F, 1.0F };
    Quaternion q = { 0.0F, 0.0F, 0.0F, -1.0F };
    Quaternion out{};

    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &q, &out, 0.25F), 0);
    CHECK_NEAR(out.x, 0.0, kTolerance);
    CHECK_NEAR(out.y, 0.0, kTolerance);
    CHECK_NEAR(out.z, 0.0, kTolerance);
    // sin(pi/4) - sin(pi/4).
    CHECK_NEAR(out.w, 0.0, kTolerance);

    // Aliased, the perpendicular survives and the same code is a quarter turn
    // towards it.  Both halves of t are covered because the branch splits at
    // 0.5 and computes the second from t - 0.5.
    Quaternion aliased = q;
    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &aliased, &aliased, 0.25F), 0);
    CHECK_NEAR(aliased.z, -std::sin(M_PI_2 * 0.5), kTolerance);
    CHECK_NEAR(aliased.w, std::sin(M_PI_2 * 0.5), kTolerance);

    aliased = q;
    CHECK_EQ(HSD_QuatLib_8037EF28(&p, &aliased, &aliased, 0.75F), 0);
    CHECK_NEAR(aliased.z, -std::sin(M_PI_2 * 0.5), kTolerance);
    CHECK_NEAR(aliased.w, std::sin(M_PI_2 * 0.5), kTolerance);
}

MELEE_TEST(UpstreamQuatLib, AgreesBetweenTheMatrixAndEulerRoutes)
{
    // MatToQuat and EulerToQuat are written independently -- one divides the
    // matrix columns by their lengths and picks a branch by trace, the other
    // multiplies out three half-angle sines and cosines.  On the same
    // rotation they have to land on the same quaternion, which pins the sign
    // and handedness conventions of both at once.
    Mtx matrix;
    rotation_about_z(matrix, (float) (M_PI / 2.0));

    Quaternion from_matrix{};
    CHECK_EQ(MatToQuat(matrix, &from_matrix), 0);

    Vec3 euler = { 0.0F, 0.0F, (float) (M_PI / 2.0) };
    Quaternion from_euler{};
    CHECK_EQ(EulerToQuat(&euler, &from_euler), 0);

    CHECK_NEAR(from_matrix.x, from_euler.x, kTolerance);
    CHECK_NEAR(from_matrix.y, from_euler.y, kTolerance);
    CHECK_NEAR(from_matrix.z, from_euler.z, kTolerance);
    CHECK_NEAR(from_matrix.w, from_euler.w, kTolerance);

    CHECK_NEAR(from_matrix.z, kHalfSqrt2, kTolerance);
    CHECK_NEAR(from_matrix.w, kHalfSqrt2, kTolerance);

    // And the matrix reads back as the euler angles it was built from.
    Vec3 read_back{};
    CHECK_EQ(HSD_QuatLib_8037EB28(matrix, &read_back), 0);
    CHECK_NEAR(read_back.x, 0.0, kTolerance);
    CHECK_NEAR(read_back.y, 0.0, kTolerance);
    CHECK_NEAR(read_back.z, M_PI / 2.0, kTolerance);
}

MELEE_TEST(UpstreamQuatLib, ComposesRotationsAboutOneAxis)
{
    // An axis-angle quaternion times another about the same axis is the sum
    // of the angles.  The axis is deliberately not a unit vector: the
    // constructor normalizes it, and a caller that has scaled a bone axis
    // relies on that.
    Vec3 axis = { 0.0F, 0.0F, 5.0F };
    Quaternion thirty{};
    Quaternion sixty{};
    REQUIRE_EQ(HSD_QuatLib_8037ECE0(&axis, &thirty, (float) (M_PI / 6.0)), 0);
    REQUIRE_EQ(HSD_QuatLib_8037ECE0(&axis, &sixty, (float) (M_PI / 3.0)), 0);

    Quaternion product{};
    CHECK_EQ(HSD_QuatLib_8037EC4C(&thirty, &thirty, &product), 0);
    CHECK_NEAR(product.x, 0.0, kTolerance);
    CHECK_NEAR(product.y, 0.0, kTolerance);
    CHECK_NEAR(product.z, sixty.z, kTolerance);
    CHECK_NEAR(product.w, sixty.w, kTolerance);

    // A degenerate axis has no rotation to describe, and is refused rather
    // than dividing by its length.
    Vec3 nothing = { 0.0F, 0.0F, 0.0F };
    Quaternion untouched = { 1.0F, 2.0F, 3.0F, 4.0F };
    CHECK_EQ(HSD_QuatLib_8037ECE0(&nothing, &untouched, 1.0F), -1);
    CHECK_EQ(untouched.x, 1.0F);
    CHECK_EQ(untouched.w, 4.0F);
}
