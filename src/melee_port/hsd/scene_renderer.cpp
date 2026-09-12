#include "scene_renderer.hpp"

#include "scene.hpp"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace meleeboard::hsd {
namespace {

using Matrix = std::array<float, 12>;

Matrix identity()
{
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
}

Matrix multiply(const Matrix& a, const Matrix& b)
{
    Matrix result{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
            result[row * 4 + column] = a[row * 4] * b[column] +
                a[row * 4 + 1] * b[4 + column] +
                a[row * 4 + 2] * b[8 + column];
        }
        result[row * 4 + 3] = a[row * 4] * b[3] +
            a[row * 4 + 1] * b[7] + a[row * 4 + 2] * b[11] +
            a[row * 4 + 3];
    }
    return result;
}

Matrix local_matrix(const HostJoint& joint)
{
    // HSD_JObjMakeMatrix calls HSD_MtxSRT for ordinary JObjs.  The first
    // bridge mirrors its SRT purpose (animation/IK-specific flags follow in
    // later work), with the standard HSD Euler rotations expressed in radians.
    const float sx = std::sin(joint.rotation[0]);
    const float cx = std::cos(joint.rotation[0]);
    const float sy = std::sin(joint.rotation[1]);
    const float cy = std::cos(joint.rotation[1]);
    const float sz = std::sin(joint.rotation[2]);
    const float cz = std::cos(joint.rotation[2]);
    Matrix result = {
        joint.scale[0] * (cy * cz), joint.scale[1] * (cz * sx * sy - cx * sz), joint.scale[2] * (cx * cz * sy + sx * sz), joint.translation[0],
        joint.scale[0] * (cy * sz), joint.scale[1] * (cx * cz + sx * sy * sz), joint.scale[2] * (cx * sy * sz - cz * sx), joint.translation[1],
        joint.scale[0] * -sy,       joint.scale[1] * (cy * sx),               joint.scale[2] * (cx * cy),               joint.translation[2],
    };
    return result;
}

std::array<float, 3> transform_point(const Matrix& matrix,
                                     const std::array<float, 3>& point)
{
    return {
        matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3],
        matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7],
        matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11],
    };
}

bool drawable(const HostDrawObject& object)
{
    if (!object.position_stream_decoded || object.positions.empty() ||
        object.triangle_indices.size() < 3 || object.triangle_indices.size() % 3 != 0) {
        return false;
    }
    return std::all_of(object.triangle_indices.begin(), object.triangle_indices.end(),
                       [&object](uint32_t index) { return index < object.positions.size(); });
}

} // namespace

MeleeSceneRenderer::MeleeSceneRenderer(const HostScene& scene) : scene_(scene)
{
    for (const HostDrawObject& object : scene_.draw_objects()) {
        if (drawable(object)) {
            ++drawable_object_count_;
            submitted_triangle_count_ +=
                static_cast<uint32_t>(object.triangle_indices.size() / 3);
        } else {
            ++skipped_object_count_;
        }
    }
}

void MeleeSceneRenderer::render()
{
    const auto& joints = scene_.joints();
    const auto& objects = scene_.draw_objects();
    std::unordered_map<uint32_t, uint32_t> joint_by_source;
    joint_by_source.reserve(joints.size());
    for (uint32_t index = 0; index < joints.size(); ++index) {
        joint_by_source.emplace(joints[index].source_offset, index);
    }

    std::vector<Matrix> worlds(joints.size(), identity());
    std::vector<int32_t> object_owner(objects.size(), -1);
    std::vector<bool> visited(joints.size(), false);
    std::vector<bool> visited_objects(objects.size(), false);
    const auto visit = [&](auto&& self, int32_t index, const Matrix& parent) -> void {
        if (index < 0 || static_cast<size_t>(index) >= joints.size() ||
            visited[static_cast<size_t>(index)]) {
            return;
        }
        const size_t joint_index = static_cast<size_t>(index);
        visited[joint_index] = true;
        worlds[joint_index] = multiply(parent, local_matrix(joints[joint_index]));
        for (int32_t object = joints[joint_index].first_draw_object;
             object >= 0 && static_cast<size_t>(object) < objects.size();
             object = objects[static_cast<size_t>(object)].next) {
            if (visited_objects[static_cast<size_t>(object)]) {
                break;
            }
            visited_objects[static_cast<size_t>(object)] = true;
            object_owner[static_cast<size_t>(object)] = index;
        }
        self(self, joints[joint_index].child, worlds[joint_index]);
        self(self, joints[joint_index].sibling, parent);
    };
    for (uint32_t root : scene_.model_roots()) {
        const auto found = joint_by_source.find(root);
        if (found != joint_by_source.end()) {
            visit(visit, static_cast<int32_t>(found->second), identity());
        }
    }

    // A deterministic bounds-based camera is intentionally temporary.  It
    // proves the HostScene path before CObj materialization is added.
    std::array<float, 3> minimum = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    std::array<float, 3> maximum = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
    bool has_geometry = false;
    for (uint32_t object_index = 0; object_index < objects.size(); ++object_index) {
        if (!drawable(objects[object_index]) || object_owner[object_index] < 0) {
            continue;
        }
        const Matrix& world = worlds[static_cast<size_t>(object_owner[object_index])];
        for (const auto& position : objects[object_index].positions) {
            const auto transformed = transform_point(world, position);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], transformed[axis]);
                maximum[axis] = std::max(maximum[axis], transformed[axis]);
            }
            has_geometry = true;
        }
    }
    if (!has_geometry) {
        return;
    }

    const std::array<float, 3> center = {
        (minimum[0] + maximum[0]) * 0.5F,
        (minimum[1] + maximum[1]) * 0.5F,
        (minimum[2] + maximum[2]) * 0.5F,
    };
    const float extent = std::max({ maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2], 1.0F });
    Point3d eye = { center[0], center[1] + extent * 0.8F, center[2] + extent * 2.0F };
    Point3d target = { center[0], center[1], center[2] };
    Vec up = { 0.0F, 1.0F, 0.0F };
    Mtx view{};
    Mtx44 projection{};
    MTXLookAt(view, &eye, &up, &target);
    MTXPerspective(projection, 45.0F, 4.0F / 3.0F, std::max(0.1F, extent * 0.01F), extent * 8.0F + 100.0F);
    Matrix view_matrix{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            view_matrix[row * 4 + column] = view[row][column];
        }
    }

    GXSetProjection(projection, GX_PERSPECTIVE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG,
                  GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, { 72, 220, 180, 255 });
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

    for (uint32_t object_index = 0; object_index < objects.size(); ++object_index) {
        const HostDrawObject& object = objects[object_index];
        if (!drawable(object) || object_owner[object_index] < 0) {
            continue;
        }
        const Matrix model_view = multiply(
            view_matrix, worlds[static_cast<size_t>(object_owner[object_index])]);
        GXLoadPosMtxImm(model_view.data(), GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        for (size_t start = 0; start < object.triangle_indices.size();) {
            const size_t remaining = object.triangle_indices.size() - start;
            const size_t count = std::min<size_t>(remaining, 65535 / 3 * 3);
            GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast<u16>(count));
            for (size_t index = start; index < start + count; ++index) {
                const uint32_t vertex = object.triangle_indices[index];
                const auto& position = object.positions[vertex];
                GXPosition3f32(position[0], position[1], position[2]);
            }
            GXEnd();
            start += count;
        }
    }
}

uint32_t MeleeSceneRenderer::drawable_object_count() const { return drawable_object_count_; }
uint32_t MeleeSceneRenderer::skipped_object_count() const { return skipped_object_count_; }
uint32_t MeleeSceneRenderer::submitted_triangle_count() const { return submitted_triangle_count_; }

} // namespace meleeboard::hsd
