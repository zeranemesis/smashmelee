#include "host_runtime.hpp"
#include "video.hpp"

#include <melee/sysdolphin/baselib/aobj.h>
#include <melee/sysdolphin/baselib/class.h>
#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/gobj.h>
#include <melee/sysdolphin/baselib/id.h>
#include <melee/sysdolphin/baselib/list.h>
#include <melee/sysdolphin/baselib/mtx.h>
#include <melee/sysdolphin/baselib/object.h>

#include <cstdint>

namespace meleeboard::hsd {

namespace {

uint64_t gHeartbeatCount = 0;

void heartbeat(HSD_GObj*)
{
    ++gHeartbeatCount;
}

// Mirrors HSD_ObjInit from sysdolphin/baselib/initialize.c, in the same
// order.  The RObj, render, shadow, and z-list pools join this list as those
// subsystems are ported.
void hsd_obj_init()
{
    HSD_ListInitAllocData();
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    HSD_IDInitAllocData();
    HSD_VecInitAllocData();
    HSD_MtxInitAllocData();
}

} // namespace

bool initialize_host_runtime()
{
    // HSD_InitComponent sets the ID table up and then brings the object pools
    // online through HSD_ObjInit.  Its OS/VI/GX half is Aurora's host
    // swapchain here, established by initialize_video below.
    //
    // The behavior of everything initialized here is pinned down by the
    // offline suite under tests/hsd, which runs in CI on every push.
    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);
    HSD_IDSetup();
    hsd_obj_init();

    if (!initialize_video()) {
        return false;
    }

    HSD_GObjInit();
    HSD_GObj* heartbeat_object = GObj_Create(0, 0, 0);
    if (heartbeat_object == nullptr ||
        HSD_GObj_SetupProc(heartbeat_object, heartbeat, 0) == nullptr) {
        HSD_GObjShutdown();
        shutdown_video();
        return false;
    }
    gHeartbeatCount = 0;
    return true;
}

void tick_host_runtime()
{
    begin_video_frame();
    HSD_GObjThink();
}

void shutdown_host_runtime()
{
    HSD_GObjShutdown();
    shutdown_video();
}

uint64_t rendered_frame_count()
{
    return video_frame_count();
}

} // namespace meleeboard::hsd
