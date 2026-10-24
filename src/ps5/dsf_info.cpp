#include "ps5_sacd/formats/dsf/parser.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class ReadOnlyMapping {
public:
    ReadOnlyMapping() = default;
    ReadOnlyMapping(const ReadOnlyMapping&) = delete;
    ReadOnlyMapping& operator=(const ReadOnlyMapping&) = delete;

    ~ReadOnlyMapping() {
        if (data_ != MAP_FAILED) {
            munmap(data_, size_);
        }
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    bool open(const char* path) noexcept {
        descriptor_ = ::open(path, O_RDONLY);
        if (descriptor_ < 0) {
            std::fprintf(stderr, "cannot open '%s': %s\n", path, std::strerror(errno));
            return false;
        }

        struct stat status {};
        if (fstat(descriptor_, &status) != 0) {
            std::fprintf(stderr, "cannot stat '%s': %s\n", path, std::strerror(errno));
            return false;
        }
        if (status.st_size <= 0
            || static_cast<unsigned long long>(status.st_size)
                > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            std::fprintf(stderr, "'%s' has an unsupported file size\n", path);
            return false;
        }

        size_ = static_cast<std::size_t>(status.st_size);
        data_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor_, 0);
        if (data_ == MAP_FAILED) {
            std::fprintf(stderr, "cannot map '%s': %s\n", path, std::strerror(errno));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

private:
    int descriptor_{-1};
    void* data_{MAP_FAILED};
    std::size_t size_{};
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dsf_info <path-to-dsf>\n");
        return 2;
    }

    ReadOnlyMapping file;
    if (!file.open(argv[1])) {
        return 1;
    }

    const auto parsed = ps5_sacd::formats::dsf::parse(file.bytes());
    if (!parsed) {
        const auto error = parsed.error();
        const auto description = ps5_sacd::formats::dsf::message(error.code);
        std::fprintf(stderr,
                     "invalid DSF at byte %zu: %.*s\n",
                     error.offset,
                     static_cast<int>(description.size()),
                     description.data());
        return 1;
    }

    const auto& format = parsed->format;
    std::printf("valid DSF\n");
    std::printf("channels: %u\n", static_cast<unsigned>(format.channel_count));
    std::printf("sample rate: %u Hz\n", static_cast<unsigned>(format.sample_rate_hz));
    std::printf("bits per sample: %u\n", static_cast<unsigned>(format.bits_per_sample));
    std::printf("samples per channel: %llu\n",
                static_cast<unsigned long long>(format.sample_count_per_channel));
    std::printf("audio bytes: %zu\n", parsed->audio_data.size());
    return 0;
}
