#include "ps5_sacd/audio/dsd_to_wav.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class MappedInputFile {
public:
    MappedInputFile() = default;
    MappedInputFile(const MappedInputFile&) = delete;
    MappedInputFile& operator=(const MappedInputFile&) = delete;

    MappedInputFile(MappedInputFile&& other) noexcept {
        move_from(std::move(other));
    }

    MappedInputFile& operator=(MappedInputFile&& other) noexcept {
        if (this != &other) {
            close();
            move_from(std::move(other));
        }
        return *this;
    }

    ~MappedInputFile() {
        close();
    }

    [[nodiscard]] static bool open(
        const std::filesystem::path& path,
        MappedInputFile& result,
        std::string_view& error) noexcept {
#if defined(_WIN32)
        result.file_handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (result.file_handle_ == INVALID_HANDLE_VALUE) {
            error = "could not open input file";
            return false;
        }

        LARGE_INTEGER file_size{};
        if (GetFileSizeEx(result.file_handle_, &file_size) == 0 || file_size.QuadPart < 0
            || static_cast<unsigned long long>(file_size.QuadPart)
                > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            error = "input file size is not supported";
            return false;
        }
        result.size_ = static_cast<std::size_t>(file_size.QuadPart);
        if (result.size_ == 0U) {
            return true;
        }

        result.mapping_handle_ = CreateFileMappingW(result.file_handle_, nullptr, PAGE_READONLY, 0U, 0U, nullptr);
        if (result.mapping_handle_ == nullptr) {
            error = "could not create an input file mapping";
            return false;
        }
        result.data_ = static_cast<const std::byte*>(
            MapViewOfFile(result.mapping_handle_, FILE_MAP_READ, 0U, 0U, 0U));
        if (result.data_ == nullptr) {
            error = "could not map the input file";
            return false;
        }
#else
        result.file_descriptor_ = ::open(path.c_str(), O_RDONLY);
        if (result.file_descriptor_ < 0) {
            error = "could not open input file";
            return false;
        }

        struct stat file_status {};
        if (::fstat(result.file_descriptor_, &file_status) != 0 || file_status.st_size < 0
            || static_cast<unsigned long long>(file_status.st_size)
                > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            error = "input file size is not supported";
            return false;
        }
        result.size_ = static_cast<std::size_t>(file_status.st_size);
        if (result.size_ == 0U) {
            return true;
        }

        const auto* mapped = ::mmap(nullptr, result.size_, PROT_READ, MAP_PRIVATE, result.file_descriptor_, 0);
        if (mapped == MAP_FAILED) {
            error = "could not map the input file";
            return false;
        }
        result.data_ = static_cast<const std::byte*>(mapped);
#endif
        return true;
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data_, size_};
    }

private:
    void close() noexcept {
#if defined(_WIN32)
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
        }
        if (mapping_handle_ != nullptr) {
            CloseHandle(mapping_handle_);
        }
        if (file_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle_);
        }
        data_ = nullptr;
        mapping_handle_ = nullptr;
        file_handle_ = INVALID_HANDLE_VALUE;
#else
        if (data_ != nullptr) {
            ::munmap(const_cast<std::byte*>(data_), size_);
        }
        if (file_descriptor_ >= 0) {
            ::close(file_descriptor_);
        }
        data_ = nullptr;
        file_descriptor_ = -1;
#endif
        size_ = 0U;
    }

    void move_from(MappedInputFile&& other) noexcept {
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0U);
#if defined(_WIN32)
        file_handle_ = std::exchange(other.file_handle_, INVALID_HANDLE_VALUE);
        mapping_handle_ = std::exchange(other.mapping_handle_, nullptr);
#else
        file_descriptor_ = std::exchange(other.file_descriptor_, -1);
#endif
    }

    const std::byte* data_{};
    std::size_t size_{};
#if defined(_WIN32)
    HANDLE file_handle_{INVALID_HANDLE_VALUE};
    HANDLE mapping_handle_{};
#else
    int file_descriptor_{-1};
#endif
};

void print_usage(std::ostream& output) {
    output << "Usage: dsd2wav [--rate HZ] INPUT.dsf|INPUT.dff OUTPUT.wav\n"
              "Convert DSF or uncompressed DSDIFF to 24-bit PCM WAV.\n"
              "Default rate: 44100 Hz for the 44.1 kHz DSD family, otherwise 48000 Hz.\n"
              "DST-compressed DSDIFF is not supported.\n";
}

[[nodiscard]] bool parse_rate(std::string_view text, std::uint32_t& rate) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), rate);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && rate != 0U;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }

    std::uint32_t output_rate = 0U;
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    std::size_t positional_count = 0U;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--rate") {
            if (index + 1 >= argc || !parse_rate(argv[index + 1], output_rate)) {
                std::cerr << "--rate expects a positive integer sample rate in Hz.\n";
                print_usage(std::cerr);
                return 2;
            }
            ++index;
        } else if (argument.starts_with("--")) {
            std::cerr << "Unknown option: " << argument << '\n';
            print_usage(std::cerr);
            return 2;
        } else if (positional_count == 0U) {
            input_path = std::filesystem::path(argv[index]);
            ++positional_count;
        } else if (positional_count == 1U) {
            output_path = std::filesystem::path(argv[index]);
            ++positional_count;
        } else {
            std::cerr << "Too many positional arguments.\n";
            print_usage(std::cerr);
            return 2;
        }
    }
    if (positional_count != 2U) {
        print_usage(std::cerr);
        return 2;
    }

    std::error_code filesystem_error;
    const auto absolute_input = std::filesystem::absolute(input_path, filesystem_error).lexically_normal();
    if (filesystem_error) {
        std::cerr << "Could not resolve input path: " << filesystem_error.message() << '\n';
        return 2;
    }
    const auto absolute_output = std::filesystem::absolute(output_path, filesystem_error).lexically_normal();
    if (filesystem_error) {
        std::cerr << "Could not resolve output path: " << filesystem_error.message() << '\n';
        return 2;
    }
    if (absolute_input == absolute_output) {
        std::cerr << "Input and output paths must be different.\n";
        return 2;
    }
    if (std::filesystem::exists(output_path, filesystem_error)) {
        std::cerr << "Output already exists; choose another path: " << output_path.string() << '\n';
        return 2;
    }
    if (filesystem_error) {
        std::cerr << "Could not inspect output path: " << filesystem_error.message() << '\n';
        return 2;
    }

    MappedInputFile input_file;
    std::string_view input_error;
    if (!MappedInputFile::open(input_path, input_file, input_error)) {
        std::cerr << input_error << ": " << input_path.string() << '\n';
        return 1;
    }

    auto temporary_path = output_path;
    temporary_path += ".partial";
    if (std::filesystem::exists(temporary_path, filesystem_error) || filesystem_error) {
        std::cerr << "Temporary output path already exists or cannot be checked: "
                  << temporary_path.string() << '\n';
        return 1;
    }

    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::out);
        if (!output) {
            std::cerr << "Could not create output file: " << temporary_path.string() << '\n';
            return 1;
        }
        const auto conversion = ps5_sacd::audio::convert_to_wav(input_file.bytes(), output, output_rate);
        if (!conversion) {
            std::cerr << conversion.error().detail;
            if (conversion.error().input_offset != 0U) {
                std::cerr << " at input byte " << conversion.error().input_offset;
            }
            std::cerr << '\n';
            output.close();
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }
        output.flush();
        if (!output) {
            std::cerr << "Could not finish writing the WAV file.\n";
            output.close();
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }
        output.close();
        if (!output) {
            std::cerr << "Could not close the WAV file cleanly.\n";
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }

        std::filesystem::rename(temporary_path, output_path, filesystem_error);
        if (filesystem_error) {
            std::cerr << "Could not publish WAV output: " << filesystem_error.message() << '\n';
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }
        std::cout << "Wrote " << conversion->output_frame_count << " frames, "
                  << conversion->channel_count << " channels at "
                  << conversion->output_sample_rate_hz << " Hz: " << output_path.string() << '\n';
    }
    return 0;
}
