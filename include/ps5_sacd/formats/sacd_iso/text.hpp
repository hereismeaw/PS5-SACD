#pragma once

#include "ps5_sacd/formats/sacd_iso/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps5_sacd::formats::sacd_iso {

enum class TextErrorCode {
    invalid_image,
    area_not_found,
    language_not_available,
    unsupported_character_set,
    invalid_text_metadata,
};

struct TextError {
    TextErrorCode code;
    std::size_t byte_offset{};
};

struct TrackText {
    std::string title;
    std::string performer;
    std::string songwriter;
    std::string composer;
    std::string arranger;
    std::string message;
    std::string extra_message;
    std::string title_phonetic;
    std::string performer_phonetic;
    std::string songwriter_phonetic;
    std::string composer_phonetic;
    std::string arranger_phonetic;
    std::string message_phonetic;
    std::string extra_message_phonetic;
};

struct TextMetadata {
    std::string language_code;
    std::string album_title;
    std::string album_artist;
    std::string album_publisher;
    std::string album_copyright;
    std::string disc_title;
    std::string disc_artist;
    std::string disc_publisher;
    std::string disc_copyright;
    std::vector<TrackText> tracks;
};

[[nodiscard]] std::string_view message(TextErrorCode code) noexcept;

// Reads one SACD-Text language. ISO-646 and ISO-8859-1 are decoded to UTF-8;
// legacy multibyte character sets return an explicit unsupported-charset error.
[[nodiscard]] std::expected<TextMetadata, TextError> parse_text(
    std::span<const std::byte> image,
    AreaKind area_kind,
    std::uint8_t language_index = 0U);

} // namespace ps5_sacd::formats::sacd_iso
