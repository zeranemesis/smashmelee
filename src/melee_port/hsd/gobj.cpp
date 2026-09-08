#include <melee/sysdolphin/baselib/gobj.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstring>

namespace {

constexpr uint8_t kPLinkCount = 64;
constexpr uint8_t kProcPriorityCount = 3;

HSD_GObj* gPLinkHeads[kPLinkCount]{};
HSD_ObjAllocData gGObjAllocator{};
HSD_ObjAllocData gProcAllocator{};
bool gInitialized = false;
bool gInThink = false;
uint64_t gFrameCount = 0;

void unlink_gobj(HSD_GObj* gobj)
{
    HSD_GObj** head = &gPLinkHeads[gobj->p_link];
    if (gobj->prev != nullptr) {
        gobj->prev->next = gobj->next;
    } else {
        *head = gobj->next;
    }
    if (gobj->next != nullptr) {
        gobj->next->prev = gobj->prev;
    }
}

void remove_destroyed()
{
    for (HSD_GObj*& head : gPLinkHeads) {
        HSD_GObj* current = head;
        while (current != nullptr) {
            HSD_GObj* next = current->next;
            if (current->pending_destroy != 0) {
                unlink_gobj(current);
                HSD_GObjProc* proc = current->proc;
                while (proc != nullptr) {
                    HSD_GObjProc* child = proc->child;
                    HSD_ObjFree(&gProcAllocator, proc);
                    proc = child;
                }
                if (current->user_data_remove_func != nullptr) {
                    current->user_data_remove_func(current->user_data);
                }
                HSD_ObjFree(&gGObjAllocator, current);
            } else {
                HSD_GObjProc** link = &current->proc;
                while (*link != nullptr) {
                    HSD_GObjProc* proc = *link;
                    if (proc->pending_remove != 0) {
                        *link = proc->child;
                        HSD_ObjFree(&gProcAllocator, proc);
                    } else {
                        link = &proc->child;
                    }
                }
            }
            current = next;
        }
    }
}

} // namespace

extern "C" void HSD_GObjInit(void)
{
    HSD_GObjShutdown();
    HSD_ObjAllocInit(&gGObjAllocator, sizeof(HSD_GObj), alignof(HSD_GObj));
    HSD_ObjAllocInit(&gProcAllocator, sizeof(HSD_GObjProc), alignof(HSD_GObjProc));
    std::memset(gPLinkHeads, 0, sizeof(gPLinkHeads));
    gFrameCount = 0;
    gInitialized = true;
}

extern "C" void HSD_GObjShutdown(void)
{
    if (!gInitialized) {
        return;
    }
    for (HSD_GObj* head : gPLinkHeads) {
        for (HSD_GObj* current = head; current != nullptr; current = current->next) {
            if (current->user_data_remove_func != nullptr) {
                current->user_data_remove_func(current->user_data);
            }
        }
    }
    std::memset(gPLinkHeads, 0, sizeof(gPLinkHeads));
    HSD_ObjAllocShutdown(&gProcAllocator);
    HSD_ObjAllocShutdown(&gGObjAllocator);
    gInThink = false;
    gInitialized = false;
    gFrameCount = 0;
}

extern "C" HSD_GObj* GObj_Create(uint16_t classifier, uint8_t p_link,
                                   uint8_t priority)
{
    if (!gInitialized || p_link >= kPLinkCount) {
        return nullptr;
    }
    auto* gobj = static_cast<HSD_GObj*>(HSD_ObjAlloc(&gGObjAllocator));
    if (gobj == nullptr) {
        return nullptr;
    }
    std::memset(gobj, 0, sizeof(*gobj));
    gobj->classifier = classifier;
    gobj->p_link = p_link;
    gobj->gx_link = UINT8_MAX;
    gobj->p_priority = priority;

    HSD_GObj** link = &gPLinkHeads[p_link];
    HSD_GObj* previous = nullptr;
    while (*link != nullptr && (*link)->p_priority <= priority) {
        previous = *link;
        link = &(*link)->next;
    }
    gobj->next = *link;
    gobj->prev = previous;
    if (*link != nullptr) {
        (*link)->prev = gobj;
    }
    *link = gobj;
    return gobj;
}

extern "C" HSD_GObjProc* HSD_GObj_SetupProc(HSD_GObj* gobj,
                                              HSD_GObjEvent func,
                                              uint8_t priority)
{
    if (!gInitialized || gobj == nullptr || func == nullptr ||
        gobj->pending_destroy != 0 || priority >= kProcPriorityCount) {
        return nullptr;
    }
    auto* proc = static_cast<HSD_GObjProc*>(HSD_ObjAlloc(&gProcAllocator));
    if (proc == nullptr) {
        return nullptr;
    }
    std::memset(proc, 0, sizeof(*proc));
    proc->s_link = priority;
    proc->gobj = gobj;
    proc->on_invoke = func;
    proc->child = gobj->proc;
    gobj->proc = proc;
    return proc;
}

extern "C" void HSD_GObjProcRemove(HSD_GObjProc* proc)
{
    if (proc != nullptr) {
        proc->pending_remove = 1;
    }
}

extern "C" void HSD_GObjDestroy(HSD_GObj* gobj)
{
    if (gobj == nullptr || gobj->pending_destroy != 0) {
        return;
    }
    gobj->pending_destroy = 1;
    if (!gInThink) {
        remove_destroyed();
    }
}

extern "C" void HSD_GObjThink(void)
{
    if (!gInitialized) {
        return;
    }
    gInThink = true;
    for (uint8_t proc_priority = 0; proc_priority < kProcPriorityCount;
         ++proc_priority) {
        for (HSD_GObj* head : gPLinkHeads) {
            for (HSD_GObj* gobj = head; gobj != nullptr; gobj = gobj->next) {
                if (gobj->pending_destroy != 0) {
                    continue;
                }
                for (HSD_GObjProc* proc = gobj->proc; proc != nullptr;
                     proc = proc->child) {
                    if (proc->pending_remove == 0 &&
                        proc->s_link == proc_priority) {
                        proc->on_invoke(gobj);
                    }
                }
            }
        }
    }
    gInThink = false;
    remove_destroyed();
    ++gFrameCount;
}

extern "C" uint64_t HSD_GObjGetFrameCount(void)
{
    return gFrameCount;
}
