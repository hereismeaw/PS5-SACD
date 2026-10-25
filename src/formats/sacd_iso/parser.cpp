#include "ps5_sacd/formats/sacd_iso/parser.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace ps5_sacd::formats::sacd_iso {
namespace {

constexpr std::size_t kMasterTocHeaderOffset =
    static_cast<std::size_t>(master_toc_lsn) * logical_sector_size;
constexpr std::size_t kMasterArea1PrimaryOffset = 64;
constexpr std::size_t kMasterArea1BackupOffset = 68;
constexpr std::size_t kMasterArea2PrimaryOffset = 72;
constexpr std::size_t kMasterArea2BackupOffset = 76;
constexpr std::size_t kMasterArea1SizeOffset = 84;
constexpr std::size_t kMasterArea2SizeOffset = 86;
constexpr std::size_t kAreaHeaderSizeOffset = 10;
constexpr std::size_t kAreaChannelCountOffset = 32;
constexpr std::size_t kAreaLoudspeakerConfigOffset = 33;
constexpr std::size_t kAreaTrackCountOffset = 69;
constexpr std::size_t kAreaTrackStartOffset = 72;
constexpr std::size_t kAreaTrackEndOffset = 76;
constexpr std::size_t kTrackListHeaderSize = 8;
constexpr std::size_t kTrackStartArrayOffset = kTrackListHeaderSize;
constexpr std::size_t kTrackLengthArrayOffset =
    kTrackStartArrayOffset + maximum_track_count * sizeof(std::uint32_t);

[[nodiscard]] std::uint8_t byte_at(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint16_t read_u16_be(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte_at(bytes, offset)) << 8U)
        | static_cast<std::uint16_t>(byte_at(bytes, offset + 1U)));
}

[[nodiscard]] std::uint32_t read_u32_be(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value = (value << 8U) | byte_at(bytes, offset + index);
    }
    return value;
}

template <std::size_t Size>
[[nodiscard]] bool matches(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const std::array<char, Size>& identifier) noexcept {
    for (std::size_t index = 0; index < identifier.size(); ++index) {
        if (byte_at(bytes, offset + index)
            != static_cast<std::uint8_t>(identifier[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool sector_range_is_valid(
    std::uint64_t sector_count,
    std::uint32_t start_lsn,
    std::uint32_t length_lsn) noexcept {
    return length_lsn != 0U
        && static_cast<std::uint64_t>(start_lsn) < sector_count
        && static_cast<std::uint64_t>(length_lsn)
            <= sector_count - static_cast<std::uint64_t>(start_lsn);
}

[[nodiscard]] std::expected<AudioArea, ParseError> parse_area_copy(
    std::span<const std::byte> image,
    std::uint64_t sector_count,
    AreaKind kind,
    std::uint32_t toc_lsn,
    std::uint16_t master_toc_size,
    bool used_backup) noexcept {
    const auto pointer_offset_64 = static_cast<std::uint64_t>(toc_lsn) * logical_sector_size;
    if (toc_lsn == 0U || master_toc_size == 0U
        || master_toc_size > maximum_area_toc_sectors
        || pointer_offset_64 > image.size()
        || static_cast<std::uint64_t>(toc_lsn) > sector_count
        || static_cast<std::uint64_t>(master_toc_size)
            > sector_count - static_cast<std::uint64_t>(toc_lsn)) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_area_pointer,
            pointer_offset_64 > image.size()
                ? image.size()
                : static_cast<std::size_t>(pointer_offset_64)});
    }
    const auto pointer_offset = static_cast<std::size_t>(pointer_offset_64);

    const auto area_data = image.subspan(
        pointer_offset,
        static_cast<std::size_t>(master_toc_size) * logical_sector_size);
    const auto identifier_matches = kind == AreaKind::stereo
        ? matches(area_data, 0, std::array{'T', 'W', 'O', 'C', 'H', 'T', 'O', 'C'})
        : matches(area_data, 0, std::array{'M', 'U', 'L', 'C', 'H', 'T', 'O', 'C'});
    if (!identifier_matches) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_area_toc_signature,
            pointer_offset});
    }

    const auto major = byte_at(area_data, 8);
    const auto minor = byte_at(area_data, 9);
    if (major != 1U || minor > 20U) {
        return std::unexpected(ParseError{
            ErrorCode::unsupported_area_toc_version,
            pointer_offset + 8U});
    }

    const auto declared_size = read_u16_be(area_data, kAreaHeaderSizeOffset);
    if (declared_size == 0U
        || declared_size > maximum_area_toc_sectors
        || declared_size != master_toc_size) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_area_toc_size,
            pointer_offset + kAreaHeaderSizeOffset});
    }

    const auto channels = byte_at(area_data, kAreaChannelCountOffset);
    if ((kind == AreaKind::stereo && channels != 2U)
        || (kind == AreaKind::multichannel && (channels < 3U || channels > 6U))) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_area_metadata,
            pointer_offset + kAreaChannelCountOffset});
    }

    const auto track_start = read_u32_be(area_data, kAreaTrackStartOffset);
    const auto track_end = read_u32_be(area_data, kAreaTrackEndOffset);
    const auto track_count = byte_at(area_data, kAreaTrackCountOffset);
    if (track_count == 0U || track_start > track_end
        || static_cast<std::uint64_t>(track_end) >= sector_count) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_area_metadata,
            pointer_offset + kAreaTrackCountOffset});
    }

    AudioArea area;
    area.kind = kind;
    area.toc_lsn = toc_lsn;
    area.used_backup_toc = used_backup;
    area.channel_count = channels;
    area.sample_frequency_code = byte_at(area_data, 20);
    area.frame_format_code = static_cast<std::uint8_t>(byte_at(area_data, 21) & 0x0FU);
    area.loudspeaker_config = static_cast<std::uint8_t>(
        byte_at(area_data, kAreaLoudspeakerConfigOffset) >> 3U);
    area.track_start_lsn = track_start;
    area.track_end_lsn = track_end;
    area.track_count = track_count;

    bool found_track_list = false;
    for (std::size_t sector_index = 1; sector_index < declared_size;) {
        const auto chunk_offset = sector_index * logical_sector_size;
        std::size_t chunk_sector_count = 1;

        if (matches(area_data, chunk_offset,
                    std::array{'S', 'A', 'C', 'D', 'T', 'R', 'L', '1'})) {
            if (found_track_list) {
                return std::unexpected(ParseError{
                    ErrorCode::invalid_track_list,
                    pointer_offset + chunk_offset});
            }

            found_track_list = true;
            const auto starts_base = chunk_offset + kTrackStartArrayOffset;
            const auto lengths_base = chunk_offset + kTrackLengthArrayOffset;
            std::uint64_t previous_start = 0;
            std::uint64_t previous_end_inclusive = 0;
            for (std::size_t track_index = 0; track_index < track_count; ++track_index) {
                const auto start = read_u32_be(
                    area_data,
                    starts_base + track_index * sizeof(std::uint32_t));
                const auto length = read_u32_be(
                    area_data,
                    lengths_base + track_index * sizeof(std::uint32_t));
                const auto end_exclusive = static_cast<std::uint64_t>(start) + length;
                const auto end_inclusive = end_exclusive - (length == 0U ? 0U : 1U);

                if (length == 0U
                    || start < track_start
                    || end_inclusive > track_end
                    || !sector_range_is_valid(sector_count, start, length)
                    || (track_index > 0U
                        && (static_cast<std::uint64_t>(start) <= previous_start
                            || static_cast<std::uint64_t>(start) < previous_end_inclusive))) {
                    return std::unexpected(ParseError{
                        ErrorCode::invalid_track_extent,
                        pointer_offset + starts_base + track_index * sizeof(std::uint32_t)});
                }

                area.tracks[track_index] = TrackExtent{start, length};
                previous_start = start;
                previous_end_inclusive = end_inclusive;
            }
        } else if (matches(area_data, chunk_offset,
                           std::array{'S', 'A', 'C', 'D', '_', 'I', 'G', 'L'})) {
            chunk_sector_count = 2;
        } else if (matches(area_data, chunk_offset,
                           std::array{'S', 'A', 'C', 'D', '_', 'A', 'C', 'C'})) {
            chunk_sector_count = 32;
        } else if (matches(area_data, chunk_offset,
                           std::array{'S', 'A', 'C', 'D', 'T', 'R', 'L', '2'})) {
            chunk_sector_count = 1;
        } else if (matches(area_data, chunk_offset,
                           std::array{'S', 'A', 'C', 'D', 'T', 'T', 'x', 't'})) {
            chunk_sector_count = 1;
        } else {
            break;
        }

        if (chunk_sector_count > declared_size - sector_index) {
            return std::unexpected(ParseError{
                ErrorCode::invalid_area_toc_size,
                pointer_offset + chunk_offset});
        }
        sector_index += chunk_sector_count;
    }

    if (!found_track_list) {
        return std::unexpected(ParseError{
            ErrorCode::missing_track_list,
            pointer_offset + logical_sector_size});
    }

    return area;
}

} // namespace

std::string_view message(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::truncated_image:
        return "image ends before the complete master TOC";
    case ErrorCode::unsupported_sector_size:
        return "image is not aligned to 2048-byte logical sectors";
    case ErrorCode::invalid_master_toc_signature:
        return "master TOC contains an invalid signature";
    case ErrorCode::unsupported_master_toc_version:
        return "master TOC version is unsupported";
    case ErrorCode::invalid_area_pointer:
        return "area TOC pointer or sector range is invalid";
    case ErrorCode::invalid_area_toc_signature:
        return "area TOC signature does not match its master TOC slot";
    case ErrorCode::unsupported_area_toc_version:
        return "area TOC version is unsupported";
    case ErrorCode::invalid_area_toc_size:
        return "area TOC size is invalid or inconsistent";
    case ErrorCode::invalid_area_metadata:
        return "area TOC metadata is inconsistent";
    case ErrorCode::invalid_track_list:
        return "area TOC contains more than one track list";
    case ErrorCode::missing_track_list:
        return "area TOC does not contain a SACDTRL1 track list";
    case ErrorCode::invalid_track_extent:
        return "track sector extent is invalid or outside its audio area";
    case ErrorCode::no_audio_areas:
        return "master TOC does not describe an audio area";
    }
    return "unknown SACD ISO parser error";
}

std::expected<SacdImage, ParseError> parse(
    std::span<const std::byte> image) noexcept {
    if (image.size() % logical_sector_size != 0U) {
        return std::unexpected(ParseError{ErrorCode::unsupported_sector_size, image.size()});
    }

    const auto sector_count = image.size() / logical_sector_size;
    if (sector_count < static_cast<std::size_t>(master_toc_lsn) + master_toc_sector_count) {
        return std::unexpected(ParseError{ErrorCode::truncated_image, image.size()});
    }

    if (!matches(image, kMasterTocHeaderOffset,
                 std::array{'S', 'A', 'C', 'D', 'M', 'T', 'O', 'C'})) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_master_toc_signature,
            kMasterTocHeaderOffset});
    }

    for (std::size_t text_index = 0; text_index < 8U; ++text_index) {
        const auto text_offset = kMasterTocHeaderOffset
            + (text_index + 1U) * logical_sector_size;
        if (!matches(image, text_offset,
                     std::array{'S', 'A', 'C', 'D', 'T', 'e', 'x', 't'})) {
            return std::unexpected(ParseError{
                ErrorCode::invalid_master_toc_signature,
                text_offset});
        }
    }
    const auto manufacturer_offset = kMasterTocHeaderOffset
        + (master_toc_sector_count - 1U) * logical_sector_size;
    if (!matches(image, manufacturer_offset,
                 std::array{'S', 'A', 'C', 'D', '_', 'M', 'a', 'n'})) {
        return std::unexpected(ParseError{
            ErrorCode::invalid_master_toc_signature,
            manufacturer_offset});
    }

    const auto major = byte_at(image, kMasterTocHeaderOffset + 8U);
    const auto minor = byte_at(image, kMasterTocHeaderOffset + 9U);
    if (major != 1U || minor > 20U) {
        return std::unexpected(ParseError{
            ErrorCode::unsupported_master_toc_version,
            kMasterTocHeaderOffset + 8U});
    }

    const std::array<std::uint32_t, 2> primary_tocs{
        read_u32_be(image, kMasterTocHeaderOffset + kMasterArea1PrimaryOffset),
        read_u32_be(image, kMasterTocHeaderOffset + kMasterArea2PrimaryOffset),
    };
    const std::array<std::uint32_t, 2> backup_tocs{
        read_u32_be(image, kMasterTocHeaderOffset + kMasterArea1BackupOffset),
        read_u32_be(image, kMasterTocHeaderOffset + kMasterArea2BackupOffset),
    };
    const std::array<std::uint16_t, 2> toc_sizes{
        read_u16_be(image, kMasterTocHeaderOffset + kMasterArea1SizeOffset),
        read_u16_be(image, kMasterTocHeaderOffset + kMasterArea2SizeOffset),
    };
    constexpr std::array<AreaKind, 2> kinds{
        AreaKind::stereo,
        AreaKind::multichannel,
    };

    SacdImage result;
    result.logical_sector_count = sector_count;
    for (std::size_t area_index = 0; area_index < kinds.size(); ++area_index) {
        if (primary_tocs[area_index] == 0U && backup_tocs[area_index] == 0U
            && toc_sizes[area_index] == 0U) {
            continue;
        }

        std::expected<AudioArea, ParseError> parsed_area = std::unexpected(
            ParseError{ErrorCode::invalid_area_pointer, kMasterTocHeaderOffset});
        if (primary_tocs[area_index] != 0U) {
            parsed_area = parse_area_copy(
                image,
                sector_count,
                kinds[area_index],
                primary_tocs[area_index],
                toc_sizes[area_index],
                false);
        }
        if (!parsed_area && backup_tocs[area_index] != 0U) {
            parsed_area = parse_area_copy(
                image,
                sector_count,
                kinds[area_index],
                backup_tocs[area_index],
                toc_sizes[area_index],
                true);
        }
        if (!parsed_area) {
            return std::unexpected(parsed_area.error());
        }

        result.areas[result.area_count] = *parsed_area;
        ++result.area_count;
    }

    if (result.area_count == 0U) {
        return std::unexpected(ParseError{
            ErrorCode::no_audio_areas,
            kMasterTocHeaderOffset + kMasterArea1PrimaryOffset});
    }

    return result;
}

} // namespace ps5_sacd::formats::sacd_iso
