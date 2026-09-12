#include <melee/sysdolphin/baselib/fobj.h>

#include <cstring>

namespace {

HSD_ObjAllocData gFObjAllocator{};

void request_animation(HSD_FObj* fobj, float startframe)
{
    fobj->ad = fobj->ad_head;
    fobj->time = static_cast<float>(fobj->startframe) + startframe;
    fobj->op = HSD_A_OP_NONE;
    fobj->op_intrp = HSD_A_OP_NONE;
    fobj->flags &= ~0x40U;
    fobj->nb_pack = 0;
    fobj->fterm = 0;
    fobj->p0 = 0.0F;
    fobj->p1 = 0.0F;
    fobj->d0 = 0.0F;
    fobj->d1 = 0.0F;
    HSD_FObjSetState(fobj, 1);
}

} // namespace

extern "C" HSD_ObjAllocData* HSD_FObjGetAllocData(void)
{
    return &gFObjAllocator;
}

extern "C" void HSD_FObjInitAllocData(void)
{
    HSD_ObjAllocInit(&gFObjAllocator, sizeof(HSD_FObj), alignof(HSD_FObj));
}

extern "C" HSD_FObj* HSD_FObjAlloc(void)
{
    auto* result = static_cast<HSD_FObj*>(HSD_ObjAlloc(&gFObjAllocator));
    if (result != nullptr) {
        std::memset(result, 0, sizeof(*result));
    }
    return result;
}

extern "C" void HSD_FObjFree(HSD_FObj* fobj)
{
    if (fobj != nullptr) {
        HSD_ObjFree(&gFObjAllocator, fobj);
    }
}

extern "C" void HSD_FObjRemove(HSD_FObj* fobj)
{
    HSD_FObjFree(fobj);
}

extern "C" void HSD_FObjRemoveAll(HSD_FObj* fobj)
{
    while (fobj != nullptr) {
        HSD_FObj* next = fobj->next;
        HSD_FObjFree(fobj);
        fobj = next;
    }
}

extern "C" uint32_t HSD_FObjSetState(HSD_FObj* fobj, uint32_t state)
{
    if (fobj != nullptr) {
        fobj->flags = static_cast<uint8_t>((state & 0x0FU) | (fobj->flags & 0xF0U));
    }
    return state;
}

extern "C" uint32_t HSD_FObjGetState(HSD_FObj* fobj)
{
    return fobj != nullptr ? fobj->flags & 0x0FU : 0;
}

extern "C" void HSD_FObjReqAnimAll(HSD_FObj* fobj, float startframe)
{
    for (; fobj != nullptr; fobj = fobj->next) {
        request_animation(fobj, startframe);
    }
}

extern "C" HSD_FObj* HSD_FObjLoadDesc(HSD_FObjDesc* description)
{
    if (description == nullptr) {
        return nullptr;
    }
    HSD_FObj* result = HSD_FObjAlloc();
    if (result == nullptr) {
        return nullptr;
    }
    result->next = HSD_FObjLoadDesc(description->next);
    result->startframe = static_cast<int16_t>(description->startframe);
    result->obj_type = description->type;
    result->frac_value = description->frac_value;
    result->frac_slope = description->frac_slope;
    result->ad_head = description->ad;
    result->length = description->length;
    return result;
}
