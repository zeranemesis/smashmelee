#include <melee/sysdolphin/baselib/mtx.h>

namespace {

HSD_ObjAllocData gVecAllocator{};
HSD_ObjAllocData gMtxAllocator{};

} // namespace

extern "C" HSD_ObjAllocData* HSD_VecGetAllocData(void)
{
    return &gVecAllocator;
}

extern "C" void HSD_VecInitAllocData(void)
{
    HSD_ObjAllocInit(&gVecAllocator, sizeof(Vec), alignof(Vec));
}

extern "C" void* HSD_VecAlloc(void)
{
    return HSD_ObjAlloc(&gVecAllocator);
}

extern "C" void HSD_VecFree(void* value)
{
    if (value != nullptr) {
        HSD_ObjFree(&gVecAllocator, value);
    }
}

extern "C" HSD_ObjAllocData* HSD_MtxGetAllocData(void)
{
    return &gMtxAllocator;
}

extern "C" void HSD_MtxInitAllocData(void)
{
    HSD_ObjAllocInit(&gMtxAllocator, sizeof(Mtx), alignof(Mtx));
}

extern "C" void* HSD_MtxAlloc(void)
{
    return HSD_ObjAlloc(&gMtxAllocator);
}

extern "C" void HSD_MtxFree(void* value)
{
    if (value != nullptr) {
        HSD_ObjFree(&gMtxAllocator, value);
    }
}
