#pragma once

#include <cstdint>

namespace meleeboard::hsd {

class HostScene;

// First host-side HSD draw bridge.  It deliberately uses Aurora's GX
// compatibility API rather than exposing GameCube addresses to the GPU.
class MeleeSceneRenderer {
public:
    explicit MeleeSceneRenderer(const HostScene& scene);

    // Render this scene's roots as children of a joint in another animated
    // HostScene. Melee uses this for the five independently animated main-menu
    // cursor instances attached to MenMainConTop option joints.
    void attach_roots_to(const HostScene& parent, uint32_t traversal_index);
    void render();
    uint32_t drawable_object_count() const;
    uint32_t skipped_object_count() const;
    uint32_t submitted_triangle_count() const;

private:
    const HostScene& scene_;
    const HostScene* parent_scene_ = nullptr;
    uint32_t parent_traversal_index_ = 0;
    uint32_t drawable_object_count_ = 0;
    uint32_t skipped_object_count_ = 0;
    uint32_t submitted_triangle_count_ = 0;
};

} // namespace meleeboard::hsd
