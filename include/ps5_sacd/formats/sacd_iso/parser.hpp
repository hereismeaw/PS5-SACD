#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace ps5_sacd::formats::sacd_iso {

inline constexpr std::size_t logical_sector_size = 2048;
inline constexpr std::uint32_t master_toc_lsn = 510;
inline constexpr std::size_t master_toc_sector_count = 10;
inline constexpr std::size_t maximum_area_toc_sectors = 96;
inline constexpr std::size_t maximum_track_count = 255;

enum class ErrorCode {
    truncated_image,
    unsupported_sector_size,
    invalid_master_toc_signature,
    unsupported_master_toc_version,
    invalid_area_pointer,
    invalid_area_toc_signature,
    unsupported_area_toc_version,
    invalid_area_toc_size,
    invalid_area_metadata,
    invalid_track_list,
    missing_track_list,
    invalid_track_extent,
    no_audio_areas,
};

[[nodiscard]] std::string_view message(ErrorCode code) noexcept;

struct ParseError {
    ErrorCode code;
    std::size_t byte_offset;
};

enum class AreaKind : std::uint8_t {
    stereo,
    multichannel,
};

struct TrackExtent {
    std::uint32_t start_lsn{};
    std::uint32_t length_lsn{};

    [[nodiscard]] constexpr std::uint64_t end_lsn_exclusive() const noexcept {
        return static_cast<std::uint64_t>(start_lsn) + length_lsn;
    }
};

struct AudioArea {
    AreaKind kind{};
    std::uint32_t toc_lsn{};
    bool used_backup_toc{};
    std::uint8_t channel_count{};
    std::uint8_t sample_frequency_code{};
    std::uint8_t frame_format_code{};
    std::uint8_t loudspeaker_config{};
    std::uint32_t track_start_lsn{};
    std::uint32_t track_end_lsn{};
    std::uint8_t track_count{};
    std::array<TrackExtent, maximum_track_count> tracks{};
};

struct SacdImage {
    std::uint64_t logical_sector_count{};
    std::uint8_t area_count{};
    std::array<AudioArea, 2> areas{};
};

// Parses metadata and bounded sector extents from a raw 2048-byte-sector
// SACD image. This does not read/decode audio sectors or support 2064-byte
// raw-sector dumps.
[[nodiscard]] std::expected<SacdImage, ParseError> parse(
    std::span<const std::byte> image) noexcept;

} // namespace ps5_sacd::formats::sacd_iso
