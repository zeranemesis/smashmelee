#include "harness.hpp"

// Upstream's vector and matrix pools, plus the SDK math they sit on.
//
// These were the units the plan expected to be blocked: the 23 math symbols
// listed as missing from Aurora turned out to be macro-aliased onto its
// C_MTX*/C_VEC* implementations, which compile with no dependency beyond their
// own headers.  Nothing had to be written.

#include <cstdint>
#include <cstring>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/mtx.h>
#include <sysdolphin/baselib/objalloc.h>
}

MELEE_TEST(UpstreamMathPools, AllocatesAndReleasesVectorsAndMatrices)
{
    HSD_VecInitAllocData();
    HSD_MtxInitAllocData();

    void* vector = HSD_VecAlloc();
    void* matrix = HSD_MtxAlloc();
    REQUIRE(vector != nullptr);
    REQUIRE(matrix != nullptr);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_VecGetAllocData()), 1U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()), 1U);

    HSD_VecFree(vector);
    HSD_MtxFree(matrix);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_VecGetAllocData()), 0U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()), 0U);
}

MELEE_TEST(UpstreamMathPools, KeepsTheTwoPoolsSeparate)
{
    HSD_VecInitAllocData();
    HSD_MtxInitAllocData();

    void* first = HSD_VecAlloc();
    void* second = HSD_VecAlloc();
    void* matrix = HSD_MtxAlloc();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(matrix != nullptr);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_VecGetAllocData()), 2U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()), 1U);

    HSD_VecFree(first);
    HSD_VecFree(second);
    HSD_MtxFree(matrix);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_VecGetAllocData()), 0U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()), 0U);
}

MELEE_TEST(UpstreamMathPools, DecomposesAnSRTMatrix)
{
    // HSD_MtxSRT takes scale, rotation in radians, translation, and an
    // optional pivot it divides by -- a zero pivot is a division by zero, so
    // absent means NULL rather than a zero vector.  Reading the scale back
    // runs through VECMag, VECNormalize, VECDotProduct, VECSubtract and
    // VECCrossProduct, so this covers Aurora's vector math behaviorally and
    // not just at the link.
    Mtx matrix;
    Vec3 scale = { 2.0F, 3.0F, 4.0F };
    Vec3 rotation = { 0.0F, 0.0F, 0.0F };
    Vec3 translation = { 5.0F, 6.0F, 7.0F };
    HSD_MtxSRT(matrix, &scale, &rotation, &translation, nullptr);

    Vec3 read_scale{};
    Vec3 read_translation{};
    HSD_MtxGetScale(matrix, &read_scale);
    HSD_MtxGetTranslate(matrix, &read_translation);

    CHECK_EQ(read_translation.x, 5.0F);
    CHECK_EQ(read_translation.y, 6.0F);
    CHECK_EQ(read_translation.z, 7.0F);
    CHECK_EQ(read_scale.x, 2.0F);
    CHECK_EQ(read_scale.y, 3.0F);
    CHECK_EQ(read_scale.z, 4.0F);
}
