#include "host_runtime.hpp"
#include "video.hpp"

#include <melee/sysdolphin/baselib/class.h>
#include <melee/sysdolphin/baselib/gobj.h>
#include <melee/sysdolphin/baselib/object.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstddef>
#include <cstdint>

namespace meleeboard::hsd {

namespace {

uint64_t gHeartbeatCount = 0;

void heartbeat(HSD_GObj*)
{
    ++gHeartbeatCount;
}

struct GObjProbe {
    uint8_t value;
    uint8_t* order;
    uint8_t* count;
};

void record_gobj(HSD_GObj* gobj)
{
    auto* probe = static_cast<GObjProbe*>(HSD_GObjGetUserData(gobj));
    probe->order[(*probe->count)++] = probe->value;
}

bool verify_gobj_scheduler()
{
    uint8_t order[2]{};
    uint8_t count = 0;
    GObjProbe late{ 2, order, &count };
    GObjProbe early{ 1, order, &count };

    HSD_GObjInit();
    HSD_GObj* later = GObj_Create(2, 0, 20);
    HSD_GObj* earlier = GObj_Create(1, 0, 10);
    if (later == nullptr || earlier == nullptr ||
        HSD_GObj_SetupProc(later, record_gobj, 0) == nullptr ||
        HSD_GObj_SetupProc(earlier, record_gobj, 0) == nullptr) {
        HSD_GObjShutdown();
        return false;
    }
    later->user_data = &late;
    earlier->user_data = &early;
    HSD_GObjThink();
    const bool ordered = count == 2 && order[0] == 1 && order[1] == 2 &&
        HSD_GObjGetFrameCount() == 1;
    HSD_GObjDestroy(later);
    HSD_GObjDestroy(earlier);
    HSD_GObjShutdown();
    return ordered;
}

} // namespace

bool initialize_host_runtime()
{
    struct alignas(32) ProbeObject {
        std::byte payload[48];
    };

    HSD_ObjAllocData pool{};
    HSD_ObjAllocInit(&pool, sizeof(ProbeObject), alignof(ProbeObject));
    HSD_ObjAllocSetNumLimit(&pool, 2);
    HSD_ObjAllocEnableNumLimit(&pool);

    void* first = HSD_ObjAlloc(&pool);
    void* second = HSD_ObjAlloc(&pool);
    void* limited = HSD_ObjAlloc(&pool);
    const bool aligned = first != nullptr &&
        reinterpret_cast<uintptr_t>(first) % alignof(ProbeObject) == 0;
    const bool initial_stats = second != nullptr && limited == nullptr &&
        HSD_ObjAllocGetUsing(&pool) == 2 && HSD_ObjAllocGetPeak(&pool) == 2;

    HSD_ObjFree(&pool, first);
    void* recycled = HSD_ObjAlloc(&pool);
    const bool free_list_reused = recycled == first &&
        HSD_ObjAllocGetUsing(&pool) == 2 && HSD_ObjAllocGetPeak(&pool) == 2;

    HSD_ObjFree(&pool, recycled);
    HSD_ObjFree(&pool, second);
    const bool final_stats = HSD_ObjAllocGetUsing(&pool) == 0 &&
        HSD_ObjAllocGetFreed(&pool) == 2;
    HSD_ObjAllocShutdown(&pool);

    ClassInfoInit(&hsdClass);
    ClassInfoInit(&hsdObj);
    HSD_Obj* object = static_cast<HSD_Obj*>(hsdNew(&hsdObj));
    const bool class_ready = object != nullptr &&
        hsdObjIsDescendantOf(object, &hsdClass) &&
        hsdSearchClassInfo("hsd_obj") == &hsdObj &&
        hsdObj.head.nb_exist == 1 && hsdObj.head.nb_peak == 1;
    if (object != nullptr) {
        ref_INC(object);
    }
    const bool references_work = object != nullptr && ref_CNT(object) == 1 &&
        ref_DEC(object) && ref_CNT(object) == 0;
    hsdDelete(object);
    const bool class_stats = hsdObj.head.nb_exist == 0 &&
        hsdObj.head.nb_peak == 1;

    if (!(aligned && initial_stats && free_list_reused && final_stats &&
          class_ready && references_work && class_stats &&
          verify_gobj_scheduler())) {
        return false;
    }

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
