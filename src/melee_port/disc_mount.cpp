#include <melee/port/disc_mount.hpp>

#include <nod.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace meleeboard::disc {

namespace {

struct NodDeleter {
    void operator()(NodHandle* handle) const { nod_free(handle); }
};

struct MountedDisc {
    std::unique_ptr<NodHandle, NodDeleter> disc;
    std::unique_ptr<NodHandle, NodDeleter> partition;
    std::string path;
};

std::mutex gMutex;
std::unique_ptr<MountedDisc> gMountedDisc;

bool is_target_disc(const NodDiscHeader& header)
{
    return std::memcmp(header.game_id, "GALE01", 6) == 0 &&
        header.disc_num == 0 && header.disc_version == 2;
}

uint32_t find_file_locked(std::string_view path, NodNodeKind* kind,
                          uint32_t* length)
{
    if (gMountedDisc == nullptr || path.empty() ||
        path.find('\0') != std::string_view::npos) {
        return NOD_FST_STOP;
    }

    const std::string normalized_path(path);
    return nod_partition_find_file(gMountedDisc->partition.get(),
                                   normalized_path.c_str(), kind, length);
}

} // namespace

bool mount(std::string_view image_path)
{
    if (image_path.empty() || image_path.find('\0') != std::string_view::npos) {
        return false;
    }

    const std::string path(image_path);
    NodHandle* raw_disc = nullptr;
    if (nod_disc_open(path.c_str(), nullptr, &raw_disc) != NOD_RESULT_OK ||
        raw_disc == nullptr) {
        return false;
    }
    std::unique_ptr<NodHandle, NodDeleter> disc(raw_disc);

    NodDiscHeader header{};
    if (nod_disc_header(disc.get(), &header) != NOD_RESULT_OK ||
        !is_target_disc(header)) {
        return false;
    }

    NodHandle* raw_partition = nullptr;
    if (nod_disc_open_partition(disc.get(), 0, nullptr, &raw_partition) !=
            NOD_RESULT_OK ||
        raw_partition == nullptr) {
        return false;
    }

    auto mounted = std::make_unique<MountedDisc>();
    mounted->disc = std::move(disc);
    mounted->partition.reset(raw_partition);
    mounted->path = path;

    std::scoped_lock lock(gMutex);
    gMountedDisc = std::move(mounted);
    return true;
}

void unmount()
{
    std::scoped_lock lock(gMutex);
    gMountedDisc.reset();
}

bool is_mounted()
{
    std::scoped_lock lock(gMutex);
    return gMountedDisc != nullptr;
}

std::string mounted_path()
{
    std::scoped_lock lock(gMutex);
    return gMountedDisc == nullptr ? std::string{} : gMountedDisc->path;
}

bool has_file(std::string_view path)
{
    std::scoped_lock lock(gMutex);
    NodNodeKind kind{};
    uint32_t length = 0;
    return find_file_locked(path, &kind, &length) != NOD_FST_STOP &&
        kind == NOD_NODE_KIND_FILE;
}

bool read_file(std::string_view path, std::vector<unsigned char>& out)
{
    std::scoped_lock lock(gMutex);
    NodNodeKind kind{};
    uint32_t length = 0;
    const uint32_t file_index = find_file_locked(path, &kind, &length);
    if (file_index == NOD_FST_STOP || kind != NOD_NODE_KIND_FILE) {
        return false;
    }

    NodHandle* raw_file = nullptr;
    if (nod_partition_open_file(gMountedDisc->partition.get(), file_index,
                                &raw_file) != NOD_RESULT_OK ||
        raw_file == nullptr) {
        return false;
    }
    std::unique_ptr<NodHandle, NodDeleter> file(raw_file);

    out.assign(length, 0);
    size_t read_total = 0;
    while (read_total < out.size()) {
        const int64_t read = nod_read(file.get(), out.data() + read_total,
                                      out.size() - read_total);
        if (read <= 0) {
            out.clear();
            return false;
        }
        read_total += static_cast<size_t>(read);
    }
    return true;
}

} // namespace meleeboard::disc
