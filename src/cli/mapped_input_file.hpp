#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
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

namespace ps5_sacd::cli_detail {

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

        result.mapping_handle_ = CreateFileMappingW(
            result.file_handle_, nullptr, PAGE_READONLY, 0U, 0U, nullptr);
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

} // namespace ps5_sacd::cli_detail
