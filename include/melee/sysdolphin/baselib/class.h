#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HSD_Class HSD_Class;
typedef struct HSD_ClassInfo HSD_ClassInfo;

struct HSD_Class {
    HSD_ClassInfo* class_info;
};

typedef struct HSD_ClassInfoHead {
    void (*info_init)(void);
    uint32_t flags;
    const char* library_name;
    const char* class_name;
    size_t obj_size;
    size_t info_size;
    HSD_ClassInfo* parent;
    HSD_ClassInfo* next;
    HSD_ClassInfo* child;
    uint32_t nb_exist;
    uint32_t nb_peak;
} HSD_ClassInfoHead;

struct HSD_ClassInfo {
    HSD_ClassInfoHead head;
    HSD_Class* (*alloc)(HSD_ClassInfo* info);
    int (*init)(HSD_Class* object);
    void (*release)(HSD_Class* object);
    void (*destroy)(HSD_Class* object);
    void (*amnesia)(HSD_ClassInfo* info);
};

extern HSD_ClassInfo hsdClass;

void ClassInfoInit(HSD_ClassInfo* info);
void hsdInitClassInfo(HSD_ClassInfo* class_info,
                      HSD_ClassInfo* parent_info,
                      const char* base_class_library,
                      const char* type,
                      size_t info_size,
                      size_t class_size);
void* hsdNew(HSD_ClassInfo* info);
void hsdDelete(void* object);
bool hsdChangeClass(void* object, HSD_ClassInfo* class_info);
bool hsdIsDescendantOf(HSD_ClassInfo* info, HSD_ClassInfo* parent);
HSD_ClassInfo* hsdSearchClassInfo(const char* class_name);
void hsdForgetClassLibrary(const char* library_name);
void class_set_flags(HSD_ClassInfo* class_info, uint32_t set, uint32_t reset);

#ifdef __cplusplus
}
#endif
