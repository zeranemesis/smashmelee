#include <melee/sysdolphin/baselib/aobj.h>

#include <cstring>

namespace {

HSD_ObjAllocData gAObjAllocator{};

} // namespace

extern "C" HSD_ObjAllocData* HSD_AObjGetAllocData(void)
{
    return &gAObjAllocator;
}

extern "C" void HSD_AObjInitAllocData(void)
{
    HSD_ObjAllocInit(&gAObjAllocator, sizeof(HSD_AObj), alignof(HSD_AObj));
}

extern "C" HSD_AObj* HSD_AObjAlloc(void)
{
    auto* result = static_cast<HSD_AObj*>(HSD_ObjAlloc(&gAObjAllocator));
    if (result != nullptr) {
        std::memset(result, 0, sizeof(*result));
        result->flags = AOBJ_NO_ANIM;
        result->framerate = 1.0F;
    }
    return result;
}

extern "C" void HSD_AObjFree(HSD_AObj* aobj)
{
    if (aobj != nullptr) {
        HSD_ObjFree(&gAObjAllocator, aobj);
    }
}

extern "C" void HSD_AObjRemove(HSD_AObj* aobj)
{
    if (aobj == nullptr) {
        return;
    }
    HSD_FObjRemoveAll(aobj->fobj);
    aobj->fobj = nullptr;
    // Native object references are borrowed only after the future JObj bridge
    // has resolved them; this partial AObj core never owns a GameCube pointer.
    aobj->hsd_obj = nullptr;
    HSD_AObjFree(aobj);
}

extern "C" uint32_t HSD_AObjGetFlags(HSD_AObj* aobj)
{
    return aobj != nullptr ? aobj->flags : 0;
}

extern "C" void HSD_AObjSetFlags(HSD_AObj* aobj, uint32_t flags)
{
    if (aobj != nullptr) {
        aobj->flags |= flags & (AOBJ_LOOP | AOBJ_NO_UPDATE);
    }
}

extern "C" void HSD_AObjClearFlags(HSD_AObj* aobj, uint32_t flags)
{
    if (aobj != nullptr) {
        aobj->flags &= ~(flags & (AOBJ_LOOP | AOBJ_NO_UPDATE));
    }
}

extern "C" void HSD_AObjSetFObj(HSD_AObj* aobj, HSD_FObj* fobj)
{
    if (aobj == nullptr) {
        return;
    }
    HSD_FObjRemoveAll(aobj->fobj);
    aobj->fobj = fobj;
}

extern "C" void HSD_AObjReqAnim(HSD_AObj* aobj, float frame)
{
    if (aobj == nullptr) {
        return;
    }
    aobj->curr_frame = frame;
    aobj->flags = (aobj->flags & ~AOBJ_NO_ANIM) | AOBJ_FIRST_PLAY;
    HSD_FObjReqAnimAll(aobj->fobj, frame);
}

extern "C" void HSD_AObjSetRate(HSD_AObj* aobj, float rate)
{
    if (aobj != nullptr) {
        aobj->framerate = rate;
    }
}

extern "C" void HSD_AObjSetRewindFrame(HSD_AObj* aobj, float frame)
{
    if (aobj != nullptr) {
        aobj->rewind_frame = frame;
    }
}

extern "C" void HSD_AObjSetEndFrame(HSD_AObj* aobj, float frame)
{
    if (aobj != nullptr) {
        aobj->end_frame = frame;
    }
}

extern "C" void HSD_AObjSetCurrentFrame(HSD_AObj* aobj, float frame)
{
    if (aobj != nullptr) {
        aobj->curr_frame = frame;
    }
}

extern "C" HSD_AObj* HSD_AObjLoadDesc(HSD_AObjDesc* description)
{
    if (description == nullptr || description->obj_id != 0) {
        // obj_id is a GameCube descriptor address until the JObj archive
        // bridge owns it. Rejecting it preserves host pointer safety.
        return nullptr;
    }
    HSD_AObj* result = HSD_AObjAlloc();
    if (result == nullptr) {
        return nullptr;
    }
    HSD_AObjSetFlags(result, description->flags);
    HSD_AObjSetRewindFrame(result, 0.0F);
    HSD_AObjSetEndFrame(result, description->end_frame);
    HSD_AObjSetFObj(result, HSD_FObjLoadDesc(description->fobjdesc));
    return result;
}

extern "C" void HSD_AObjForgetMemory(void)
{
    // The allocator owns only host blocks.  Resetting it is intentionally
    // deferred to the runtime shutdown path, matching the other host pools.
}
