#include "archive_convert.hpp"

#include <cstring>
#include <utility>

// Why there is no HSD_Joint32b here.
//
// docs/PLAN.md expected this to follow the Mario Party 4 precedent in
// src/port/byteswap.cpp: a `<Name>32b` struct laid over the archive's bytes
// with big-endian fields, and a byteswap_<name>() that copies it into the
// host structure.  For Melee's containers that shape is not merely awkward,
// it is unable to express the data.
//
// An HSD pointer field is not self-describing.  The console's loader adds the
// data section's base to every field named in the archive's *relocation
// table*, and leaves every other field alone -- so a pointer field holding
// zero is the first byte of the data section when the table names it, and is
// NULL when the table does not.  The two cases are identical in the struct
// and differ only in a table stored elsewhere in the file.  A wire struct
// cannot tell them apart, which is exactly the defect this port already had
// and fixed once (see Archive::kNullOffset).
//
// So the conversion reads through Archive, which carries the relocation table
// and answers kNullOffset for a field the table does not name.  The bounds
// checks come along for free.

namespace meleeboard::hsd {
namespace {

// Field offsets within an on-disc HSD_Joint.  Upstream's own comments in
// sysdolphin/baselib/jobj.h are the source; the structure is 0x40 bytes on
// the console and larger here, which is the whole reason for this file.
constexpr uint32_t kJointClassName = 0x00;
constexpr uint32_t kJointFlags = 0x04;
constexpr uint32_t kJointChild = 0x08;
constexpr uint32_t kJointNext = 0x0C;
constexpr uint32_t kJointUnion = 0x10;
constexpr uint32_t kJointRotation = 0x14;
constexpr uint32_t kJointScale = 0x20;
constexpr uint32_t kJointPosition = 0x2C;
constexpr uint32_t kJointMatrix = 0x38;
constexpr uint32_t kJointRObjDesc = 0x3C;
constexpr uint32_t kJointSize = 0x40;

// What the union at +0x10 holds is decided by two of the joint's flags.
const char* union_kind(uint32_t flags)
{
    if ((flags & JOBJ_PTCL) != 0) {
        return "HSD_SList (particle)";
    }
    if ((flags & JOBJ_SPLINE) != 0) {
        return "HSD_Spline";
    }
    return "HSD_DObjDesc";
}

} // namespace

bool ArchiveConverter::read_vector(uint32_t offset, Vec3& out)
{
    const auto x = archive_.data_float(offset + 0);
    const auto y = archive_.data_float(offset + 4);
    const auto z = archive_.data_float(offset + 8);
    if (!x.has_value() || !y.has_value() || !z.has_value()) {
        return false;
    }
    out.x = *x;
    out.y = *y;
    out.z = *z;
    return true;
}

bool ArchiveConverter::read_matrix(uint32_t offset, MtxPtr& out)
{
    Mtx matrix;
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            const auto value =
                archive_.data_float(offset + (row * 4 + column) * 4);
            if (!value.has_value()) {
                return false;
            }
            matrix[row][column] = *value;
        }
    }
    matrices_.emplace_back();
    std::memcpy(matrices_.back().value, matrix, sizeof matrix);
    out = matrices_.back().value;
    return true;
}

char* ArchiveConverter::read_string(uint32_t offset)
{
    std::string text;
    for (uint32_t index = 0;; ++index) {
        const auto byte = archive_.data_byte(offset + index);
        if (!byte.has_value()) {
            // An unterminated string runs off the data section.  Treat it as
            // absent rather than returning half of it.
            return nullptr;
        }
        if (*byte == 0) {
            break;
        }
        text.push_back(static_cast<char>(*byte));
    }
    strings_.push_back(std::move(text));
    return strings_.back().data();
}

void ArchiveConverter::note_unconverted(uint32_t holder, uint32_t target,
                                        const char* kind)
{
    for (const UnconvertedReference& seen : unconverted_) {
        if (seen.holder == holder && seen.target == target) {
            return;
        }
    }
    unconverted_.push_back(UnconvertedReference{ holder, target, kind });
}

HSD_Joint* ArchiveConverter::joint(uint32_t offset)
{
    error_.clear();
    if (offset == Archive::kNullOffset) {
        return nullptr;
    }

    // Two passes over a worklist rather than recursion: a joint tree is a
    // graph with a sibling chain that can be long, and an archive that is
    // wrong can point a joint at itself.  The offset map ends both.
    std::vector<uint32_t> pending{ offset };
    std::vector<uint32_t> converted;

    while (!pending.empty()) {
        const uint32_t current = pending.back();
        pending.pop_back();
        if (joints_by_offset_.count(current) != 0) {
            continue;
        }
        if (!archive_.contains_data_range(current, kJointSize)) {
            error_ = "joint at offset " + std::to_string(current) +
                     " lies outside the archive's data section";
            return nullptr;
        }

        const auto flags = archive_.data_word(current + kJointFlags);
        const auto child = archive_.data_pointer(current + kJointChild);
        const auto next = archive_.data_pointer(current + kJointNext);
        const auto class_name = archive_.data_pointer(current + kJointClassName);
        if (!flags.has_value() || !child.has_value() || !next.has_value() ||
            !class_name.has_value()) {
            error_ = "joint at offset " + std::to_string(current) +
                     " has a field that is neither a null nor a relocated "
                     "offset";
            return nullptr;
        }

        joints_.emplace_back();
        HSD_Joint& host = joints_.back();
        std::memset(&host, 0, sizeof host);
        host.flags = *flags;

        if (*class_name != Archive::kNullOffset) {
            host.class_name = read_string(*class_name);
            if (host.class_name == nullptr) {
                error_ = "joint at offset " + std::to_string(current) +
                         " names a class that runs off the data section";
                return nullptr;
            }
        }

        if (!read_vector(current + kJointRotation, host.rotation) ||
            !read_vector(current + kJointScale, host.scale) ||
            !read_vector(current + kJointPosition, host.position)) {
            error_ = "joint at offset " + std::to_string(current) +
                     " has a transform outside the data section";
            return nullptr;
        }

        const auto matrix = archive_.data_pointer(current + kJointMatrix);
        if (matrix.has_value() && *matrix != Archive::kNullOffset) {
            if (!read_matrix(*matrix, host.mtx)) {
                error_ = "joint at offset " + std::to_string(current) +
                         " carries a matrix outside the data section";
                return nullptr;
            }
        }

        // The two structure kinds this converter does not build yet.  They
        // are recorded rather than refused: the rest of the graph is correct,
        // and a caller that needs them can see exactly what is missing.
        const auto union_field = archive_.data_pointer(current + kJointUnion);
        if (union_field.has_value() &&
            *union_field != Archive::kNullOffset) {
            note_unconverted(current, *union_field, union_kind(*flags));
        }
        const auto robjdesc = archive_.data_pointer(current + kJointRObjDesc);
        if (robjdesc.has_value() && *robjdesc != Archive::kNullOffset) {
            note_unconverted(current, *robjdesc, "HSD_RObjDesc");
        }

        joints_by_offset_.emplace(current, &host);
        converted.push_back(current);
        if (*child != Archive::kNullOffset) {
            pending.push_back(*child);
        }
        if (*next != Archive::kNullOffset) {
            pending.push_back(*next);
        }
    }

    // Second pass: now that every joint has an address, link them.
    for (uint32_t current : converted) {
        HSD_Joint* host = joints_by_offset_.at(current);
        const uint32_t child = *archive_.data_pointer(current + kJointChild);
        const uint32_t next = *archive_.data_pointer(current + kJointNext);
        if (child != Archive::kNullOffset) {
            host->child = joints_by_offset_.at(child);
        }
        if (next != Archive::kNullOffset) {
            host->next = joints_by_offset_.at(next);
        }
    }

    return joints_by_offset_.at(offset);
}

} // namespace meleeboard::hsd
