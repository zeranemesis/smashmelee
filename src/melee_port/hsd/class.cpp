#include <melee/sysdolphin/baselib/class.h>

#include <cstdlib>
#include <cstring>

namespace {

void root_info_init();

HSD_Class* root_alloc(HSD_ClassInfo* info)
{
    auto* object = static_cast<HSD_Class*>(std::malloc(info->head.obj_size));
    if (object != nullptr) {
        ++info->head.nb_exist;
        if (info->head.nb_exist > info->head.nb_peak) {
            info->head.nb_peak = info->head.nb_exist;
        }
    }
    return object;
}

int root_init(HSD_Class*)
{
    return 0;
}

void root_release(HSD_Class*)
{
}

void root_destroy(HSD_Class* object)
{
    if (object == nullptr) {
        return;
    }
    HSD_ClassInfo* info = object->class_info;
    if (info != nullptr && info->head.nb_exist > 0) {
        --info->head.nb_exist;
    }
    std::free(object);
}

void root_amnesia(HSD_ClassInfo* info)
{
    if (info != nullptr) {
        info->head.nb_exist = 0;
        info->head.nb_peak = 0;
    }
}

HSD_ClassInfo* search_tree(HSD_ClassInfo* info, const char* class_name)
{
    if (info == nullptr) {
        return nullptr;
    }
    ClassInfoInit(info);
    if (info->head.class_name != nullptr &&
        std::strcmp(info->head.class_name, class_name) == 0) {
        return info;
    }
    for (HSD_ClassInfo* child = info->head.child; child != nullptr;
         child = child->head.next) {
        if (HSD_ClassInfo* found = search_tree(child, class_name)) {
            return found;
        }
    }
    return nullptr;
}

void forget_children(HSD_ClassInfo* parent, const char* library_name)
{
    HSD_ClassInfo** link = &parent->head.child;
    while (*link != nullptr) {
        HSD_ClassInfo* current = *link;
        forget_children(current, library_name);
        if (current->head.library_name != nullptr &&
            std::strcmp(current->head.library_name, library_name) == 0) {
            if (current->amnesia != nullptr) {
                current->amnesia(current);
            }
            *link = current->head.next;
            current->head.flags = 0;
            current->head.parent = nullptr;
            current->head.next = nullptr;
            current->head.child = nullptr;
        } else {
            link = &current->head.next;
        }
    }
}

HSD_ClassInfo* allocation_root(HSD_ClassInfo* info)
{
    while (info != nullptr && info->head.parent != nullptr &&
           info->head.parent->head.obj_size == info->head.obj_size) {
        info = info->head.parent;
    }
    return info;
}

void root_info_init()
{
    hsdInitClassInfo(&hsdClass, nullptr, "sysdolphin_base_library",
                     "hsd_class", sizeof(HSD_ClassInfo), sizeof(HSD_Class));
    hsdClass.alloc = root_alloc;
    hsdClass.init = root_init;
    hsdClass.release = root_release;
    hsdClass.destroy = root_destroy;
    hsdClass.amnesia = root_amnesia;
}

} // namespace

extern "C" {

HSD_ClassInfo hsdClass = { { root_info_init } };

void ClassInfoInit(HSD_ClassInfo* info)
{
    if (info != nullptr && (info->head.flags & 1U) == 0 &&
        info->head.info_init != nullptr) {
        info->head.info_init();
    }
}

void hsdInitClassInfo(HSD_ClassInfo* class_info,
                      HSD_ClassInfo* parent_info,
                      const char* base_class_library,
                      const char* type,
                      size_t info_size,
                      size_t class_size)
{
    if (class_info == nullptr || info_size < sizeof(HSD_ClassInfo) ||
        class_size < sizeof(HSD_Class)) {
        return;
    }
    class_info->head.flags = 1;
    class_info->head.library_name = base_class_library;
    class_info->head.class_name = type;
    class_info->head.obj_size = class_size;
    class_info->head.info_size = info_size;
    class_info->head.parent = parent_info;
    class_info->head.next = nullptr;
    class_info->head.child = nullptr;
    class_info->head.nb_exist = 0;
    class_info->head.nb_peak = 0;

    if (parent_info != nullptr) {
        ClassInfoInit(parent_info);
        if (class_size < parent_info->head.obj_size ||
            info_size < parent_info->head.info_size) {
            class_info->head.flags = 0;
            return;
        }
        class_info->alloc = parent_info->alloc;
        class_info->init = parent_info->init;
        class_info->release = parent_info->release;
        class_info->destroy = parent_info->destroy;
        class_info->amnesia = parent_info->amnesia;
        class_info->head.next = parent_info->head.child;
        parent_info->head.child = class_info;
    }
}

void* hsdNew(HSD_ClassInfo* info)
{
    if (info == nullptr) {
        return nullptr;
    }
    ClassInfoInit(info);
    if (info->alloc == nullptr || info->init == nullptr ||
        info->destroy == nullptr || (info->head.flags & 1U) == 0) {
        return nullptr;
    }
    HSD_Class* object = info->alloc(info);
    if (object == nullptr) {
        return nullptr;
    }
    std::memset(object, 0, info->head.obj_size);
    object->class_info = info;
    if (info->init(object) < 0) {
        info->destroy(object);
        return nullptr;
    }
    return object;
}

void hsdDelete(void* object)
{
    auto* instance = static_cast<HSD_Class*>(object);
    if (instance == nullptr || instance->class_info == nullptr) {
        return;
    }
    HSD_ClassInfo* info = instance->class_info;
    if (info->release != nullptr) {
        info->release(instance);
    }
    if (info->destroy != nullptr) {
        info->destroy(instance);
    }
}

bool hsdChangeClass(void* object, HSD_ClassInfo* class_info)
{
    auto* instance = static_cast<HSD_Class*>(object);
    if (instance == nullptr || instance->class_info == nullptr ||
        class_info == nullptr) {
        return false;
    }
    ClassInfoInit(class_info);
    HSD_ClassInfo* previous = instance->class_info;
    if (previous->head.obj_size != class_info->head.obj_size ||
        allocation_root(previous) != allocation_root(class_info)) {
        return false;
    }
    if (previous->head.nb_exist > 0) {
        --previous->head.nb_exist;
    }
    ++class_info->head.nb_exist;
    if (class_info->head.nb_exist > class_info->head.nb_peak) {
        class_info->head.nb_peak = class_info->head.nb_exist;
    }
    instance->class_info = class_info;
    return true;
}

bool hsdIsDescendantOf(HSD_ClassInfo* info, HSD_ClassInfo* parent)
{
    if (info == nullptr || parent == nullptr) {
        return false;
    }
    ClassInfoInit(info);
    ClassInfoInit(parent);
    for (HSD_ClassInfo* current = info; current != nullptr;
         current = current->head.parent) {
        if (current == parent) {
            return true;
        }
    }
    return false;
}

HSD_ClassInfo* hsdSearchClassInfo(const char* class_name)
{
    if (class_name == nullptr) {
        return nullptr;
    }
    return search_tree(&hsdClass, class_name);
}

void hsdForgetClassLibrary(const char* library_name)
{
    if (library_name == nullptr) {
        return;
    }
    ClassInfoInit(&hsdClass);
    forget_children(&hsdClass, library_name);
}

void class_set_flags(HSD_ClassInfo* class_info, uint32_t set, uint32_t reset)
{
    if (class_info != nullptr) {
        class_info->head.flags = (class_info->head.flags | set) & ~reset;
    }
}

} // extern "C"
