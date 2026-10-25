#include "ps5_sacd/formats/sacd_iso/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ps5_sacd::formats::sacd_iso {
namespace {

using ByteSpan = std::span<const std::byte>;

constexpr std::size_t kMasterOffset =
    static_cast<std::size_t>(master_toc_lsn) * logical_sector_size;
constexpr std::size_t kMasterTextCountOffset = 128U;
constexpr std::size_t kMasterLocaleOffset = 136U;
constexpr std::size_t kAreaTextCountOffset = 80U;
constexpr std::size_t kAreaLocaleOffset = 88U;
constexpr std::size_t kTrackTextHeaderSize = 8U;
constexpr std::size_t kTrackTextPositionSize = 2U;
constexpr std::size_t kMaximumTrackTextFields = 14U;

[[nodiscard]] std::uint8_t byte_at(ByteSpan bytes, std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint16_t read_u16_be(ByteSpan bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte_at(bytes, offset)) << 8U)
        | static_cast<std::uint16_t>(byte_at(bytes, offset + 1U)));
}

[[nodiscard]] bool matches(
    ByteSpan bytes,
    std::size_t offset,
    std::string_view identifier) noexcept {
    if (offset > bytes.size() || identifier.size() > bytes.size() - offset) {
        return false;
    }
    for (std::size_t index = 0; index < identifier.size(); ++index) {
        if (byte_at(bytes, offset + index)
            != static_cast<std::uint8_t>(identifier[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::expected<std::string, TextError> decode_text(
    ByteSpan encoded,
    std::uint8_t character_set,
    std::size_t source_offset) {
    if (character_set != 1U && character_set != 2U) {
        return std::unexpected(TextError{
            TextErrorCode::unsupported_character_set,
            source_offset});
    }

    std::string utf8;
    utf8.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto value = byte_at(encoded, index);
        if (character_set == 1U && value >= 0x80U) {
            return std::unexpected(TextError{
                TextErrorCode::invalid_text_metadata,
                source_offset + index});
        }
        if (value < 0x80U) {
            utf8.push_back(static_cast<char>(value));
        } else {
            utf8.push_back(static_cast<char>(0xC0U | (value >> 6U)));
            utf8.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        }
    }
    return utf8;
}

[[nodiscard]] std::expected<std::string, TextError> read_string_at(
    ByteSpan sector,
    std::uint16_t position,
    std::size_t minimum_position,
    std::uint8_t character_set,
    std::size_t sector_image_offset) {
    if (position == 0U) {
        return std::string{};
    }
    if (position < minimum_position || position >= sector.size()) {
        return std::unexpected(TextError{
            TextErrorCode::invalid_text_metadata,
            sector_image_offset + position});
    }

    const auto first = sector.begin() + static_cast<std::ptrdiff_t>(position);
    const auto terminator = std::find(first, sector.end(), std::byte{0});
    if (terminator == sector.end()) {
        return std::unexpected(TextError{
            TextErrorCode::invalid_text_metadata,
            sector_image_offset + position});
    }

    const auto length = static_cast<std::size_t>(terminator - first);
    return decode_text(
        sector.subspan(position, length),
        character_set,
        sector_image_offset + position);
}

[[nodiscard]] std::string read_language_code(ByteSpan locale) {
    std::string result;
    result.reserve(2U);
    result.push_back(static_cast<char>(byte_at(locale, 0U)));
    result.push_back(static_cast<char>(byte_at(locale, 1U)));
    return result;
}

[[nodiscard]] std::expected<void, TextError> parse_master_text(
    ByteSpan image,
    std::uint8_t language_index,
    std::uint8_t character_set,
    TextMetadata& metadata) {
    const auto sector_offset = kMasterOffset
        + (static_cast<std::size_t>(language_index) + 1U) * logical_sector_size;
    const auto sector = image.subspan(sector_offset, logical_sector_size);
    if (!matches(sector, 0U, "SACDText")) {
        return std::unexpected(TextError{
            TextErrorCode::invalid_text_metadata,
            sector_offset});
    }

    constexpr std::array<std::pair<std::size_t, std::string TextMetadata::*>, 8> fields{{
        {16U, &TextMetadata::album_title},
        {18U, &TextMetadata::album_artist},
        {20U, &TextMetadata::album_publisher},
        {22U, &TextMetadata::album_copyright},
        {32U, &TextMetadata::disc_title},
        {34U, &TextMetadata::disc_artist},
        {36U, &TextMetadata::disc_publisher},
        {38U, &TextMetadata::disc_copyright},
    }};

    for (const auto& [field_offset, target] : fields) {
        const auto string_position = read_u16_be(sector, field_offset);
        auto value = read_string_at(
            sector,
            string_position,
            48U,
            character_set,
            sector_offset);
        if (!value) {
            return std::unexpected(value.error());
        }
        metadata.*target = std::move(*value);
    }
    return {};
}

void set_track_field(TrackText& track, std::uint8_t type, std::string value) {
    switch (type) {
    case 0x01U: track.title = std::move(value); break;
    case 0x02U: track.performer = std::move(value); break;
    case 0x03U: track.songwriter = std::move(value); break;
    case 0x04U: track.composer = std::move(value); break;
    case 0x05U: track.arranger = std::move(value); break;
    case 0x06U: track.message = std::move(value); break;
    case 0x07U: track.extra_message = std::move(value); break;
    case 0x81U: track.title_phonetic = std::move(value); break;
    case 0x82U: track.performer_phonetic = std::move(value); break;
    case 0x83U: track.songwriter_phonetic = std::move(value); break;
    case 0x84U: track.composer_phonetic = std::move(value); break;
    case 0x85U: track.arranger_phonetic = std::move(value); break;
    case 0x86U: track.message_phonetic = std::move(value); break;
    case 0x87U: track.extra_message_phonetic = std::move(value); break;
    default: break;
    }
}

[[nodiscard]] std::expected<void, TextError> parse_track_text_sector(
    ByteSpan sector,
    std::size_t sector_image_offset,
    std::uint8_t character_set,
    std::vector<TrackText>& tracks) {
    if (!matches(sector, 0U, "SACDTTxt")) {
        return std::unexpected(TextError{
            TextErrorCode::invalid_text_metadata,
            sector_image_offset});
    }

    for (std::size_t track_index = 0; track_index < tracks.size(); ++track_index) {
        const auto position_offset = kTrackTextHeaderSize
            + track_index * kTrackTextPositionSize;
        const auto position = read_u16_be(sector, position_offset);
        if (position == 0U) {
            continue;
        }
        const auto minimum_position = kTrackTextHeaderSize
            + tracks.size() * kTrackTextPositionSize;
        if (position < minimum_position || position >= sector.size()) {
            return std::unexpected(TextError{
                TextErrorCode::invalid_text_metadata,
                sector_image_offset + position});
        }

        const auto field_count = static_cast<std::size_t>(byte_at(sector, position));
        if (field_count > kMaximumTrackTextFields
            || position + 4U > sector.size()) {
            return std::unexpected(TextError{
                TextErrorCode::invalid_text_metadata,
                sector_image_offset + position});
        }

        auto field_offset = static_cast<std::size_t>(position) + 4U;
        for (std::size_t field_index = 0; field_index < field_count; ++field_index) {
            if (field_offset > sector.size() || sector.size() - field_offset < 2U) {
                return std::unexpected(TextError{
                    TextErrorCode::invalid_text_metadata,
                    sector_image_offset + field_offset});
            }
            const auto type = byte_at(sector, field_offset);
            const auto string_offset = field_offset + 2U;
            const auto first = sector.begin() + static_cast<std::ptrdiff_t>(string_offset);
            const auto terminator = std::find(first, sector.end(), std::byte{0});
            if (terminator == sector.end()) {
                return std::unexpected(TextError{
                    TextErrorCode::invalid_text_metadata,
                    sector_image_offset + string_offset});
            }

            const auto string_size = static_cast<std::size_t>(terminator - first);
            if ((type >= 0x01U && type <= 0x07U)
                || (type >= 0x81U && type <= 0x87U)) {
                auto value = decode_text(
                    sector.subspan(string_offset, string_size),
                    character_set,
                    sector_image_offset + string_offset);
                if (!value) {
                    return std::unexpected(value.error());
                }
                set_track_field(tracks[track_index], type, std::move(*value));
            }

            field_offset = static_cast<std::size_t>(terminator - sector.begin()) + 1U;
            while (field_offset < sector.size() && byte_at(sector, field_offset) == 0U) {
                ++field_offset;
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, TextError> parse_area_text(
    ByteSpan image,
    const AudioArea& area,
    std::uint8_t language_index,
    std::uint8_t character_set,
    TextMetadata& metadata) {
    const auto toc_offset = static_cast<std::size_t>(area.toc_lsn) * logical_sector_size;
    const auto toc = image.subspan(toc_offset, logical_sector_size);
    const auto toc_sector_count = static_cast<std::size_t>(read_u16_be(toc, 10U));
    metadata.tracks.resize(area.track_count);

    std::uint8_t text_index = 0U;
    for (std::size_t sector_index = 1U; sector_index < toc_sector_count; ++sector_index) {
        const auto sector_offset = toc_offset + sector_index * logical_sector_size;
        const auto sector = image.subspan(sector_offset, logical_sector_size);
        if (!matches(sector, 0U, "SACDTTxt")) {
            continue;
        }
        if (text_index == language_index) {
            return parse_track_text_sector(
                sector,
                sector_offset,
                character_set,
                metadata.tracks);
        }
        ++text_index;
    }
    return {};
}

} // namespace

std::string_view message(TextErrorCode code) noexcept {
    switch (code) {
    case TextErrorCode::invalid_image:
        return "SACD image is invalid";
    case TextErrorCode::area_not_found:
        return "requested audio area is not present";
    case TextErrorCode::language_not_available:
        return "requested SACD-Text language is not available";
    case TextErrorCode::unsupported_character_set:
        return "SACD-Text character set is not supported (only ISO-646 and Latin-1 are decoded)";
    case TextErrorCode::invalid_text_metadata:
        return "SACD-Text metadata contains an invalid offset or string";
    }
    return "unknown SACD-Text error";
}

std::expected<TextMetadata, TextError> parse_text(
    std::span<const std::byte> image,
    AreaKind area_kind,
    std::uint8_t language_index) {
    const auto parsed_image = parse(image);
    if (!parsed_image) {
        return std::unexpected(TextError{
            TextErrorCode::invalid_image,
            parsed_image.error().byte_offset});
    }

    const AudioArea* selected_area = nullptr;
    for (std::size_t index = 0; index < parsed_image->area_count; ++index) {
        if (parsed_image->areas[index].kind == area_kind) {
            selected_area = &parsed_image->areas[index];
            break;
        }
    }
    if (selected_area == nullptr) {
        return std::unexpected(TextError{TextErrorCode::area_not_found});
    }

    const auto master_text_count = byte_at(image, kMasterOffset + kMasterTextCountOffset);
    const auto area_toc_offset = static_cast<std::size_t>(selected_area->toc_lsn)
        * logical_sector_size;
    const auto area_text_count = byte_at(image, area_toc_offset + kAreaTextCountOffset);
    if (master_text_count > 8U || area_text_count > 10U) {
        return std::unexpected(TextError{
            TextErrorCode::invalid_text_metadata,
            area_text_count > 10U
                ? area_toc_offset + kAreaTextCountOffset
                : kMasterOffset + kMasterTextCountOffset});
    }
    if (master_text_count == 0U && area_text_count == 0U) {
        return TextMetadata{};
    }
    if (language_index >= master_text_count && language_index >= area_text_count) {
        return std::unexpected(TextError{
            TextErrorCode::language_not_available,
            kMasterOffset + kMasterTextCountOffset});
    }

    TextMetadata metadata;
    if (language_index < master_text_count) {
        const auto locale_offset = kMasterOffset + kMasterLocaleOffset
            + static_cast<std::size_t>(language_index) * 4U;
        const auto locale = image.subspan(locale_offset, 4U);
        metadata.language_code = read_language_code(locale);
        const auto character_set = static_cast<std::uint8_t>(byte_at(locale, 2U) & 0x07U);
        auto parsed_master = parse_master_text(image, language_index, character_set, metadata);
        if (!parsed_master) {
            return std::unexpected(parsed_master.error());
        }
    }

    if (language_index < area_text_count) {
        const auto locale_offset = area_toc_offset + kAreaLocaleOffset
            + static_cast<std::size_t>(language_index) * 4U;
        const auto locale = image.subspan(locale_offset, 4U);
        if (metadata.language_code.empty()) {
            metadata.language_code = read_language_code(locale);
        }
        const auto character_set = static_cast<std::uint8_t>(byte_at(locale, 2U) & 0x07U);
        auto parsed_area = parse_area_text(
            image,
            *selected_area,
            language_index,
            character_set,
            metadata);
        if (!parsed_area) {
            return std::unexpected(parsed_area.error());
        }
    }
    return metadata;
}

} // namespace ps5_sacd::formats::sacd_iso
