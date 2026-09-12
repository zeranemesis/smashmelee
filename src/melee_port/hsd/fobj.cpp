#include <melee/sysdolphin/baselib/fobj.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

HSD_ObjAllocData gFObjAllocator{};

constexpr uint8_t kKeyPending = 0x40U;
constexpr uint8_t kKeyReady = 0x80U;
constexpr uint8_t kNewSegment = 0x20U;

bool has_bytes(const HSD_FObj* fobj, size_t count)
{
    if (fobj == nullptr || fobj->ad_head == nullptr || fobj->ad == nullptr ||
        fobj->ad < fobj->ad_head) {
        return false;
    }
    const size_t offset = static_cast<size_t>(fobj->ad - fobj->ad_head);
    return offset <= fobj->length && count <= fobj->length - offset;
}

bool read_byte(HSD_FObj* fobj, uint8_t* value)
{
    if (!has_bytes(fobj, 1)) {
        return false;
    }
    *value = *fobj->ad++;
    return true;
}

bool read_value(HSD_FObj* fobj, uint8_t fraction, float* value)
{
    const uint8_t encoding = fraction & 0xE0U;
    const uint8_t fraction_bits = fraction & 0x1FU;
    if (fraction_bits >= 31) {
        return false;
    }
    if (encoding == 0) {
        if (!has_bytes(fobj, 4)) {
            return false;
        }
        const uint32_t bits = static_cast<uint32_t>(fobj->ad[0]) |
            static_cast<uint32_t>(fobj->ad[1]) << 8U |
            static_cast<uint32_t>(fobj->ad[2]) << 16U |
            static_cast<uint32_t>(fobj->ad[3]) << 24U;
        std::memcpy(value, &bits, sizeof(bits));
        fobj->ad += 4;
        return std::isfinite(*value);
    }

    const float scale = static_cast<float>(uint32_t{ 1 } << fraction_bits);
    switch (encoding) {
    case 0x20U: // signed 16-bit
    case 0x40U: { // unsigned 16-bit
        if (!has_bytes(fobj, 2)) {
            return false;
        }
        const uint16_t raw = static_cast<uint16_t>(fobj->ad[0]) |
            static_cast<uint16_t>(fobj->ad[1]) << 8U;
        fobj->ad += 2;
        *value = encoding == 0x20U
            ? static_cast<float>(static_cast<int16_t>(raw)) / scale
            : static_cast<float>(raw) / scale;
        return true;
    }
    case 0x60U: // signed 8-bit
    case 0x80U: { // unsigned 8-bit
        uint8_t raw = 0;
        if (!read_byte(fobj, &raw)) {
            return false;
        }
        *value = encoding == 0x60U
            ? static_cast<float>(static_cast<int8_t>(raw)) / scale
            : static_cast<float>(raw) / scale;
        return true;
    }
    default:
        return false;
    }
}

bool read_pack_count(HSD_FObj* fobj, uint32_t* count)
{
    uint8_t byte = 0;
    if (!read_byte(fobj, &byte)) {
        return false;
    }
    uint32_t result = ((byte >> 4U) & 7U) + 1U;
    uint32_t shift = 3;
    while ((byte & 0x80U) != 0) {
        if (shift >= 32 || !read_byte(fobj, &byte)) {
            return false;
        }
        result += static_cast<uint32_t>(byte & 0x7FU) << shift;
        shift += 7;
    }
    *count = result;
    return true;
}

bool read_wait(HSD_FObj* fobj, uint16_t* wait)
{
    uint32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte = 0;
    do {
        if (shift >= 16 || !read_byte(fobj, &byte)) {
            return false;
        }
        result |= static_cast<uint32_t>(byte & 0x7FU) << shift;
        shift += 7;
    } while ((byte & 0x80U) != 0);
    *wait = static_cast<uint16_t>(result);
    return true;
}

float hermite(float frame_term, float time, float p0, float p1, float d0,
              float d1)
{
    const float time_squared = time * time;
    const float term_squared = frame_term * frame_term;
    const float time_squared_term = time_squared * frame_term;
    const float time_cubed_term_squared = term_squared * time_squared * time;
    const float twice_time_cubed = 2.0F * time_cubed_term_squared * frame_term;
    const float three_time_squared = 3.0F * time_squared * term_squared;
    return d1 * (time_cubed_term_squared - time_squared_term) +
        d0 * (time + time_cubed_term_squared - 2.0F * time_squared_term) +
        p0 * (1.0F + twice_time_cubed - three_time_squared) +
        p1 * (three_time_squared - twice_time_cubed);
}

void update_value(HSD_FObj* fobj, void* object,
                  HSD_ObjUpdateFunc update_function)
{
    if (update_function == nullptr) {
        return;
    }
    HSD_ObjData value{};
    switch (fobj->op_intrp) {
    case HSD_A_OP_KEY:
        if ((fobj->flags & kKeyReady) == 0) {
            return;
        }
        value.fv = fobj->p0;
        fobj->flags &= ~kKeyReady;
        break;
    case HSD_A_OP_CON:
        value.fv = fobj->time >= fobj->fterm ? fobj->p1 : fobj->p0;
        break;
    case HSD_A_OP_LIN:
        if ((fobj->flags & kNewSegment) != 0) {
            fobj->flags &= ~kNewSegment;
            if (fobj->fterm != 0) {
                fobj->d0 = (fobj->p1 - fobj->p0) / fobj->fterm;
            } else {
                fobj->d0 = 0.0F;
                fobj->p0 = fobj->p1;
            }
        }
        value.fv = fobj->d0 * fobj->time + fobj->p0;
        break;
    case HSD_A_OP_SPL0:
    case HSD_A_OP_SPL:
    case HSD_A_OP_SLP:
        value.fv = fobj->fterm != 0
            ? hermite(1.0F / fobj->fterm, fobj->time, fobj->p0, fobj->p1,
                      fobj->d0, fobj->d1)
            : fobj->p1;
        break;
    default:
        return;
    }
    update_function(object, fobj->obj_type, &value);
}

bool load_data(HSD_FObj* fobj, uint32_t* state)
{
    if (!has_bytes(fobj, 1)) {
        *state = 6;
        return true;
    }
    fobj->op_intrp = fobj->op;
    if (fobj->nb_pack == 0) {
        uint8_t op = 0;
        uint32_t pack_count = 0;
        if (!read_byte(fobj, &op) || !read_pack_count(fobj, &pack_count) ||
            pack_count > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        fobj->nb_pack = static_cast<uint16_t>(pack_count);
        fobj->op = op & 0x0FU;
    }
    --fobj->nb_pack;
    const uint32_t previous_state = *state;
    switch (fobj->op) {
    case HSD_A_OP_CON:
    case HSD_A_OP_LIN:
        fobj->p0 = fobj->p1;
        if (!read_value(fobj, fobj->frac_value, &fobj->p1)) {
            return false;
        }
        if (fobj->op_intrp != HSD_A_OP_SLP) {
            fobj->d0 = fobj->d1;
            fobj->d1 = 0.0F;
        }
        break;
    case HSD_A_OP_SPL0:
        fobj->p0 = fobj->p1;
        fobj->d0 = fobj->d1;
        if (!read_value(fobj, fobj->frac_value, &fobj->p1)) {
            return false;
        }
        fobj->d1 = 0.0F;
        break;
    case HSD_A_OP_SPL:
        fobj->p0 = fobj->p1;
        if (!read_value(fobj, fobj->frac_value, &fobj->p1)) {
            return false;
        }
        fobj->d0 = fobj->d1;
        if (!read_value(fobj, fobj->frac_slope, &fobj->d1)) {
            return false;
        }
        break;
    case HSD_A_OP_SLP:
        if (!read_value(fobj, fobj->frac_slope, &fobj->d1)) {
            return false;
        }
        return true;
    case HSD_A_OP_KEY:
        if ((fobj->flags & kKeyPending) != 0) {
            fobj->op_intrp = fobj->op;
            fobj->flags = static_cast<uint8_t>((fobj->flags & ~kKeyPending) |
                                                kKeyReady);
            fobj->p0 = fobj->p1;
        }
        if (!read_value(fobj, fobj->frac_value, &fobj->p1)) {
            return false;
        }
        fobj->flags |= kKeyPending;
        break;
    default:
        *state = 0;
        return true;
    }
    *state = previous_state == 1 ? 3 : 4;
    return true;
}

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

extern "C" void HSD_FObjInterpretAnim(HSD_FObj* fobj, void* object,
                                         HSD_ObjUpdateFunc update_function,
                                         float rate)
{
    if (fobj == nullptr || HSD_FObjGetState(fobj) == 0 || !std::isfinite(rate)) {
        return;
    }
    fobj->time += rate;
    if (fobj->time < 0.0F) {
        return;
    }
    uint32_t state = HSD_FObjGetState(fobj);
    for (uint32_t steps = 0; steps < 1024; ++steps) {
        switch (state) {
        case 6:
            fobj->time += fobj->fterm;
            if ((fobj->flags & kKeyPending) != 0) {
                fobj->op_intrp = fobj->op;
                fobj->flags = static_cast<uint8_t>((fobj->flags & ~kKeyPending) |
                                                    kKeyReady);
                fobj->p0 = fobj->p1;
            }
            update_value(fobj, object, update_function);
            return;
        case 1:
        case 2:
            if (!load_data(fobj, &state)) {
                HSD_FObjSetState(fobj, 0);
                return;
            }
            HSD_FObjSetState(fobj, state);
            break;
        case 3:
            if ((fobj->flags & kKeyReady) != 0) {
                update_value(fobj, object, update_function);
            }
            if (!read_wait(fobj, &fobj->fterm)) {
                HSD_FObjSetState(fobj, 0);
                return;
            }
            fobj->flags |= kNewSegment;
            state = 2;
            HSD_FObjSetState(fobj, state);
            break;
        case 4:
            if (fobj->fterm <= fobj->time) {
                fobj->time -= fobj->fterm;
                state = 3;
                HSD_FObjSetState(fobj, state);
                break;
            }
            update_value(fobj, object, update_function);
            HSD_FObjSetState(fobj, 5);
            return;
        case 5:
            state = 4;
            HSD_FObjSetState(fobj, state);
            break;
        default:
            return;
        }
    }
    HSD_FObjSetState(fobj, 0); // Malformed bytecode must not spin a host frame.
}

extern "C" void HSD_FObjInterpretAnimAll(HSD_FObj* fobj, void* object,
                                            HSD_ObjUpdateFunc update_function,
                                            float rate)
{
    for (; fobj != nullptr; fobj = fobj->next) {
        HSD_FObjInterpretAnim(fobj, object, update_function, rate);
    }
}

extern "C" void HSD_FObjStopAnim(HSD_FObj* fobj, void* object,
                                    HSD_ObjUpdateFunc update_function,
                                    float rate)
{
    if (fobj != nullptr && fobj->op_intrp == HSD_A_OP_KEY) {
        HSD_FObjInterpretAnim(fobj, object, update_function, rate);
    }
    HSD_FObjSetState(fobj, 0);
}

extern "C" void HSD_FObjStopAnimAll(HSD_FObj* fobj, void* object,
                                       HSD_ObjUpdateFunc update_function,
                                       float rate)
{
    for (; fobj != nullptr; fobj = fobj->next) {
        HSD_FObjStopAnim(fobj, object, update_function, rate);
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
