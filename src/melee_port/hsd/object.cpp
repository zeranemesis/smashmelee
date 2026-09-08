#include <melee/sysdolphin/baselib/object.h>

extern "C" {

HSD_ClassInfo hsdObj = { { ObjInfoInit } };

void ObjInfoInit(void)
{
    hsdInitClassInfo(&hsdObj, &hsdClass, "sysdolphin_base_library",
                     "hsd_obj", sizeof(HSD_ObjInfo), sizeof(HSD_Obj));
}

bool hsdObjIsDescendantOf(const HSD_Obj* object, HSD_ClassInfo* parent)
{
    return object != nullptr &&
        hsdIsDescendantOf(object->parent.class_info, parent);
}

} // extern "C"
