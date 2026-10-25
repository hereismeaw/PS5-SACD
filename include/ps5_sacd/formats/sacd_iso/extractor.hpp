#pragma once

#include "ps5_sacd/formats/sacd_iso/audio_frames.hpp"
#include "ps5_sacd/formats/sacd_iso/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <ostream>
#include <span>
#include <string_view>

namespace ps5_sacd::formats::sacd_iso {

enum class ExtractionErrorCode {
    invalid_image,
    area_not_found,
    invalid_track_number,
    unsupported_sample_frequency,
    unsupported_frame_format,
    dst_not_supported,
    unsupported_channel_layout,
    sector_frame_error,
    output_error,
};

struct ExtractionError {
    ExtractionErrorCode code;
    std::uint32_t lsn{};
    std::size_t byte_offset{};
    AudioFrameErrorCode audio_error{};
};

[[nodiscard]] std::string_view message(ExtractionErrorCode code) noexcept;

struct ExtractionInfo {
    std::uint32_t track_number{};
    std::uint32_t first_lsn{};
    std::uint32_t sector_count{};
    std::uint64_t frame_count{};
    std::uint64_t audio_byte_count{};
};

// Extracts a 44.1 kHz-family, plain-DSD track using the standard 2, 5, or 6
// channel layout to DSDIFF. The supplied stream must be seekable and positioned
// at byte zero. DST and nonstandard channel counts are rejected explicitly.
[[nodiscard]] std::expected<ExtractionInfo, ExtractionError> extract_track_to_dsdiff(
    std::span<const std::byte> image,
    AreaKind area_kind,
    std::uint32_t track_number,
    std::ostream& output);

} // namespace ps5_sacd::formats::sacd_iso
