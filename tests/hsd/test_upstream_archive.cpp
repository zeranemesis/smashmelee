#include "harness.hpp"

// Upstream's DAT loader, and the reason phase 2 of docs/PLAN.md needs
// converters rather than a straight swap.
//
// HSD_ArchiveParse reads the container header with native loads and compares
// the file size it finds against the one it was given.  On a GameCube that
// works because the data and the console agree on byte order.  On a
// little-endian host the same read comes back byteswapped, the comparison
// fails, and the loader refuses the archive -- which is upstream telling us,
// in its own code, that a host build has to convert before it parses.

#include <cstdint>
#include <cstring>
#include <vector>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/archive.h>
}

namespace {

// The 0x20-byte container header, in the layout HSD_ArchiveHeader describes.
std::vector<uint8_t> container(bool big_endian)
{
    constexpr uint32_t kHeaderSize = 0x20;
    constexpr uint32_t kDataSize = 0x20;
    std::vector<uint8_t> bytes(kHeaderSize + kDataSize, 0);
    const uint32_t fields[5] = {
        static_cast<uint32_t>(bytes.size()), // file_size
        kDataSize,                           // data_size
        0,                                   // nb_reloc
        0,                                   // nb_public
        0,                                   // nb_extern
    };
    for (uint32_t index = 0; index < 5; ++index) {
        const uint32_t value = fields[index];
        uint8_t* at = bytes.data() + index * 4;
        if (big_endian) {
            at[0] = static_cast<uint8_t>(value >> 24);
            at[1] = static_cast<uint8_t>(value >> 16);
            at[2] = static_cast<uint8_t>(value >> 8);
            at[3] = static_cast<uint8_t>(value);
        } else {
            std::memcpy(at, &value, sizeof(value));
        }
    }
    return bytes;
}

} // namespace

MELEE_TEST(UpstreamArchive, RefusesABigEndianContainerOnAHost)
{
    // A real GALE01 DAT is laid out exactly like this.
    std::vector<uint8_t> bytes = container(/*big_endian=*/true);
    HSD_Archive archive;
    std::memset(&archive, 0, sizeof(archive));

    CHECK_EQ(HSD_ArchiveParse(&archive, bytes.data(), bytes.size()), -1);
}

MELEE_TEST(UpstreamArchive, AcceptsAHostEndianContainer)
{
    // The same container with its header in host order is accepted, which is
    // what the *32b converters have to produce.
    std::vector<uint8_t> bytes = container(/*big_endian=*/false);
    HSD_Archive archive;
    std::memset(&archive, 0, sizeof(archive));

    REQUIRE_EQ(HSD_ArchiveParse(&archive, bytes.data(), bytes.size()), 0);
    CHECK_EQ(archive.header.file_size, bytes.size());
    CHECK_EQ(archive.header.data_size, 0x20U);
    CHECK(archive.data == bytes.data() + 0x20);
    CHECK(archive.top_ptr == bytes.data());
    // With no relocation records there is nothing for Locate() to rewrite,
    // which is the one place the loader would otherwise fold a 64-bit address
    // into a 32-bit field.
    CHECK_EQ(archive.header.nb_reloc, 0U);
}
