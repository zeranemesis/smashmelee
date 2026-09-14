#include "upstream_boot.hpp"

#include "harness.hpp"

#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/synth.h>
}

#include "gx_record.hpp"
#include "os_arena.hpp"
#include "os_scheduler.hpp"

namespace meleeboard::test {

namespace {

bool g_booted = false;
std::vector<std::string> g_boot_trace;
void** g_frame_buffers = nullptr;
int g_audio_heap_free = -1;
int g_main_heap_free = -1;

void run_boot()
{
    if (g_booted) {
        return;
    }
    g_booted = true;

    // The arena the boot ROM would have left behind, and a timebase at zero.
    os::reset_arena();
    os::reset();
    gx::reset();

    // main() sets four of the five init parameters; the audio heap keeps its
    // default.  The render mode is the one Melee ships with.
    HSD_SetInitParameter(HSD_INIT_XFB_MAX_NUM, 2);
    HSD_SetInitParameter(HSD_INIT_RENDER_MODE_OBJ, &GXNtsc480IntDf);
    HSD_SetInitParameter(HSD_INIT_FIFO_SIZE, 0x40000);
    HSD_SetInitParameter(HSD_INIT_HEAP_MAX_NUM, 4);

    // Two framebuffers and a 256 KiB graphics fifo, carved off the bottom of
    // the arena before the heaps take what is left.
    g_frame_buffers = HSD_AllocateXFB(2, &GXNtsc480IntDf);
    HSD_GXSetFifoObj(GXInit(HSD_AllocateFifo(0x40000), 0x40000));

    HSD_InitComponent();

    g_boot_trace = gx::trace();
    g_audio_heap_free = OSCheckHeap(HSD_Synth_804D6018);
    g_main_heap_free = OSCheckHeap(HSD_GetHeap());
}

} // namespace

void boot_hsd() { run_boot(); }

const std::vector<std::string>& boot_gx_trace() { return g_boot_trace; }

void* const* boot_frame_buffers() { return g_frame_buffers; }

int boot_audio_heap_free() { return g_audio_heap_free; }

int boot_main_heap_free() { return g_main_heap_free; }

namespace {

// Registered before main(); run_all() calls it before the first case.
[[maybe_unused]] const int g_registered = [] {
    set_environment_setup(&boot_hsd);
    return 0;
}();

} // namespace

} // namespace meleeboard::test
