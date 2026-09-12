#pragma once

#include <cstdint>

namespace meleeboard::hsd {

class HostScene;

// First host-side HSD draw bridge.  It deliberately uses Aurora's GX
// compatibility API rather than exposing GameCube addresses to the GPU.
class MeleeSceneRenderer {
public:
    explicit MeleeSceneRenderer(const HostScene& scene);

    void render();
    uint32_t drawable_object_count() const;
    uint32_t skipped_object_count() const;
    uint32_t submitted_triangle_count() const;

private:
    const HostScene& scene_;
    uint32_t drawable_object_count_ = 0;
    uint32_t skipped_object_count_ = 0;
    uint32_t submitted_triangle_count_ = 0;
};

} // namespace meleeboard::hsd
