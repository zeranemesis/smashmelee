#include "host_runtime.hpp"
#include "video.hpp"
#include "animation.hpp"
#include "animation_player.hpp"
#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>
#include <melee/sysdolphin/baselib/class.h>
#include <melee/sysdolphin/baselib/gobj.h>
#include <melee/sysdolphin/baselib/aobj.h>
#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/id.h>
#include <melee/sysdolphin/baselib/list.h>
#include <melee/sysdolphin/baselib/mtx.h>
#include <melee/sysdolphin/baselib/object.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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

struct FObjProbe {
    uint32_t type = 0;
    float value = 0.0F;
    uint32_t updates = 0;
};

void record_fobj(void* object, uint32_t type, HSD_ObjData* value)
{
    auto* probe = static_cast<FObjProbe*>(object);
    probe->type = type;
    probe->value = value->fv;
    ++probe->updates;
}

void write_be_word(std::vector<unsigned char>& bytes, size_t offset, uint32_t value)
{
    bytes[offset] = static_cast<unsigned char>(value >> 24U);
    bytes[offset + 1] = static_cast<unsigned char>(value >> 16U);
    bytes[offset + 2] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 3] = static_cast<unsigned char>(value);
}

void write_be_float(std::vector<unsigned char>& bytes, size_t offset, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    write_be_word(bytes, offset, bits);
}

bool verify_animation_materialization()
{
    constexpr size_t kHeaderSize = 0x20;
    constexpr size_t kDataSize = 0x80;
    constexpr size_t kRelocationCount = 3;
    constexpr size_t kPublicCount = 1;
    constexpr size_t kSymbolBytes = sizeof("testAnim");
    const size_t relocation_offset = kHeaderSize + kDataSize;
    const size_t public_offset = relocation_offset + kRelocationCount * 4;
    const size_t symbols_offset = public_offset + kPublicCount * 8;
    std::vector<unsigned char> bytes(symbols_offset + kSymbolBytes);
    write_be_word(bytes, 0x00, static_cast<uint32_t>(bytes.size()));
    write_be_word(bytes, 0x04, kDataSize);
    write_be_word(bytes, 0x08, kRelocationCount);
    write_be_word(bytes, 0x0C, kPublicCount);
    // AnimJoint at data+0; its AObjDesc is data+0x20.
    write_be_word(bytes, kHeaderSize + 0x08, 0x20);
    // AObjDesc at data+0x20; its FObjDesc is data+0x40.
    write_be_word(bytes, kHeaderSize + 0x20, AOBJ_LOOP);
    write_be_float(bytes, kHeaderSize + 0x24, 20.0F);
    write_be_word(bytes, kHeaderSize + 0x28, 0x40);
    // FObjDesc at data+0x40; bytecode is data+0x60.
    write_be_word(bytes, kHeaderSize + 0x44, 6);
    write_be_float(bytes, kHeaderSize + 0x48, 0.0F);
    bytes[kHeaderSize + 0x4C] = 5; // HSD_A_J_TRAX
    bytes[kHeaderSize + 0x4D] = HSD_A_FRAC_U8;
    bytes[kHeaderSize + 0x4E] = HSD_A_FRAC_U8;
    write_be_word(bytes, kHeaderSize + 0x50, 0x60);
    bytes[kHeaderSize + 0x60] = HSD_A_OP_CON;
    bytes[kHeaderSize + 0x61] = 0x10;
    bytes[kHeaderSize + 0x62] = 7;
    bytes[kHeaderSize + 0x63] = 1;
    bytes[kHeaderSize + 0x64] = 9;
    bytes[kHeaderSize + 0x65] = 1;
    write_be_word(bytes, relocation_offset, 0x08);
    write_be_word(bytes, relocation_offset + 4, 0x28);
    write_be_word(bytes, relocation_offset + 8, 0x50);
    write_be_word(bytes, public_offset, 0);
    write_be_word(bytes, public_offset + 4, 0);
    std::memcpy(bytes.data() + symbols_offset, "testAnim", kSymbolBytes);

    Archive archive;
    HostAnimation animation;
    if (!archive.parse(std::move(bytes)) || !animation.load(archive, "testAnim") ||
        animation.joints().size() != 1) {
        return false;
    }
    const HostAnimationJoint& joint = animation.joints().front();
    if (!(joint.has_object && joint.object.flags == AOBJ_LOOP &&
        joint.object.end_frame == 20.0F && joint.object.channels.size() == 1 &&
        joint.object.channels.front().start_frame == 0 &&
        joint.object.channels.front().object_type == 5 &&
        joint.object.channels.front().bytecode ==
            std::vector<uint8_t>{ HSD_A_OP_CON, 0x10, 7, 1, 9, 1 })) {
        return false;
    }
    std::vector<HostJoint> joints(1);
    HostAnimationPlayer player;
    if (!player.attach(animation, joints, { 0 })) {
        return false;
    }
    player.tick();
    return joints.front().translation[0] == 7.0F;
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

bool verify_core_collections()
{
    HSD_ListInitAllocData();
    int first = 1;
    int second = 2;
    HSD_SList* list = HSD_SListAllocAndAppend(nullptr, &first);
    list = HSD_SListAllocAndPrepend(list, &second);
    const bool list_ready = list != nullptr && list->data == &second &&
        list->next != nullptr && list->next->data == &first;
    list = HSD_SListRemove(list);
    list = HSD_SListRemove(list);

    HSD_IDInitAllocData();
    HSD_IDSetup();
    HSD_IDInsertToTable(nullptr, 42, &first);
    HSD_IDInsertToTable(nullptr, 143, &second); // Same hash bucket as 42.
    int32_t found = 0;
    const bool ids_ready = HSD_IDGetData(42, &found) == &first && found == 1 &&
        HSD_IDGetData(143, &found) == &second && found == 1;
    HSD_IDRemoveByIDFromTable(nullptr, 42);
    const bool removal_works = HSD_IDGetData(42, &found) == nullptr &&
        found == 0;
    HSD_IDForgetMemory();
    return list_ready && list == nullptr && ids_ready && removal_works;
}

bool verify_math_allocators()
{
    HSD_VecInitAllocData();
    HSD_MtxInitAllocData();
    void* vector = HSD_VecAlloc();
    void* matrix = HSD_MtxAlloc();
    const bool allocated = vector != nullptr && matrix != nullptr &&
        HSD_ObjAllocGetUsing(HSD_VecGetAllocData()) == 1 &&
        HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()) == 1;
    HSD_VecFree(vector);
    HSD_MtxFree(matrix);
    return allocated && HSD_ObjAllocGetUsing(HSD_VecGetAllocData()) == 0 &&
        HSD_ObjAllocGetUsing(HSD_MtxGetAllocData()) == 0;
}

bool verify_fobj_runtime()
{
    HSD_FObjInitAllocData();
    HSD_FObj* first = HSD_FObjAlloc();
    HSD_FObj* second = HSD_FObjAlloc();
    if (first == nullptr || second == nullptr) {
        HSD_FObjRemove(first);
        HSD_FObjRemove(second);
        return false;
    }
    first->next = second;
    first->startframe = 12;
    first->ad_head = reinterpret_cast<uint8_t*>(first);
    HSD_FObjReqAnimAll(first, 3.0F);
    const bool requested = first->time == 15.0F &&
        HSD_FObjGetState(first) == 1 && HSD_FObjGetState(second) == 1;
    HSD_FObjRemoveAll(first);

    // Two U8 constant keys, each followed by a one-frame wait. This verifies
    // pack decoding, state transitions, and the native callback bridge.
    uint8_t stream[] = { HSD_A_OP_CON, 0x10, 7, 1, 9, 1 };
    HSD_FObj* decoded = HSD_FObjAlloc();
    if (decoded == nullptr) {
        return false;
    }
    decoded->ad_head = stream;
    decoded->length = sizeof(stream);
    decoded->frac_value = HSD_A_FRAC_U8;
    decoded->obj_type = 17;
    FObjProbe probe{};
    HSD_FObjReqAnimAll(decoded, 0.0F);
    HSD_FObjInterpretAnim(decoded, &probe, record_fobj, 0.0F);
    const bool first_key = probe.updates == 1 && probe.type == 17 &&
        probe.value == 7.0F;
    HSD_FObjInterpretAnim(decoded, &probe, record_fobj, 1.0F);
    const bool second_key = probe.updates == 2 && probe.value == 9.0F;
    HSD_FObjRemove(decoded);
    return requested && first_key && second_key &&
        HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()) == 0;
}

bool verify_aobj_runtime()
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    HSD_AObj* animation = HSD_AObjAlloc();
    HSD_FObj* channel = HSD_FObjAlloc();
    if (animation == nullptr || channel == nullptr) {
        HSD_AObjFree(animation);
        HSD_FObjFree(channel);
        return false;
    }
    channel->startframe = 4;
    HSD_AObjSetFObj(animation, channel);
    HSD_AObjSetRate(animation, 0.5F);
    HSD_AObjSetEndFrame(animation, 30.0F);
    HSD_AObjSetFlags(animation, AOBJ_LOOP);
    HSD_AObjReqAnim(animation, 2.0F);
    const bool requested = animation->curr_frame == 2.0F &&
        animation->framerate == 0.5F && animation->end_frame == 30.0F &&
        (HSD_AObjGetFlags(animation) & AOBJ_LOOP) != 0 &&
        (HSD_AObjGetFlags(animation) & AOBJ_NO_ANIM) == 0 &&
        HSD_FObjGetState(channel) == 1;
    HSD_AObjRemove(animation);
    return requested && HSD_ObjAllocGetUsing(HSD_AObjGetAllocData()) == 0 &&
        HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()) == 0;
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
          verify_gobj_scheduler() && verify_core_collections() &&
          verify_math_allocators() && verify_fobj_runtime() &&
          verify_aobj_runtime() && verify_animation_materialization())) {
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
