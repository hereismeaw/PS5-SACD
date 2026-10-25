#pragma once

#include "ps5_sacd/formats/dsd_bit_order.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ps5_sacd::formats::dsdiff {

enum class ErrorCode {
    truncated_input,
    invalid_identifier,
    invalid_form_size,
    invalid_file_size,
    invalid_chunk_size,
    chunk_out_of_bounds,
    invalid_chunk_padding,
    invalid_chunk_order,
    duplicate_chunk,
    missing_required_chunk,
    unsupported_version,
    invalid_property,
    invalid_channel_layout,
    invalid_sample_rate,
    invalid_compression,
    unsupported_compression,
    invalid_audio_size,
    invalid_channel_index,
    channel_offset_out_of_range,
};

struct ParseError {
    ErrorCode code;
    std::size_t offset;
};

[[nodiscard]] std::string_view message(ErrorCode code) noexcept;

using ChannelId = std::array<char, 4>;

// A DSDIFF file is a non-owning audio view. Its input byte buffer must remain
// alive and unchanged while this object or any returned span is in use.
struct DsdiffFile {
    std::uint32_t sample_rate_hz{};
    std::uint64_t sample_count_per_channel{};
    std::vector<ChannelId> channel_ids;
    std::optional<std::uint16_t> loudspeaker_configuration;
    std::span<const std::byte> audio_data;

    [[nodiscard]] std::uint32_t channel_count() const noexcept {
        return static_cast<std::uint32_t>(channel_ids.size());
    }

    [[nodiscard]] constexpr DsdBitOrder bit_order() const noexcept {
        return DsdBitOrder::most_significant_bit_first;
    }

    // DSDIFF stores one byte per channel in each interleaved cluster frame.
    [[nodiscard]] std::expected<std::size_t, ParseError> read_channel_bytes(
        std::uint32_t channel,
        std::uint64_t first_byte,
        std::span<std::byte> destination) const noexcept;
};

// Parses uncompressed DSDIFF audio. DST and unknown compression types are
// reported explicitly so callers cannot mistake them for decoded DSD.
[[nodiscard]] std::expected<DsdiffFile, ParseError> parse(
    std::span<const std::byte> file);

} // namespace ps5_sacd::formats::dsdiff
