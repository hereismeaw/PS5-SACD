#pragma once

#include "ps5_sacd/formats/dsd_bit_order.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace ps5_sacd::formats::dsf {

using DsdBitOrder = ps5_sacd::formats::DsdBitOrder;

enum class ErrorCode {
    truncated_input,
    invalid_identifier,
    invalid_file_size,
    invalid_metadata_pointer,
    invalid_metadata,
    invalid_chunk_size,
    chunk_out_of_bounds,
    invalid_chunk_order,
    duplicate_chunk,
    missing_required_chunk,
    unsupported_format_version,
    unsupported_format_id,
    invalid_channel_layout,
    invalid_sample_rate,
    unsupported_bits_per_sample,
    invalid_sample_count,
    invalid_block_size,
    invalid_reserved_field,
    data_size_mismatch,
    invalid_channel_index,
    channel_offset_out_of_range,
};

struct ParseError {
    ErrorCode code;
    std::size_t offset;
};

[[nodiscard]] std::string_view message(ErrorCode code) noexcept;

struct FormatInfo {
    std::uint32_t channel_type{};
    std::uint32_t channel_count{};
    std::uint32_t sample_rate_hz{};
    std::uint32_t bits_per_sample{};
    std::uint64_t sample_count_per_channel{};
    std::uint32_t block_size_per_channel{};

    // The DSF specification defines bitsPerSample=1 as LSB-first and 8 as
    // MSB-first. Bytes returned by read_channel_bytes retain this file order.
    [[nodiscard]] constexpr DsdBitOrder bit_order() const noexcept {
        return bits_per_sample == 1
            ? DsdBitOrder::least_significant_bit_first
            : DsdBitOrder::most_significant_bit_first;
    }

    [[nodiscard]] constexpr std::uint64_t byte_count_per_channel() const noexcept {
        return sample_count_per_channel / 8U
            + (sample_count_per_channel % 8U == 0U ? 0U : 1U);
    }
};

// A parsed file is a non-owning view. The input byte buffer must remain alive
// and unchanged for the lifetime of this object and every returned span.
struct DsfFile {
    FormatInfo format;
    std::span<const std::byte> audio_data;
    std::optional<std::span<const std::byte>> id3_metadata;

    // Copy a channel's logical DSD bytes into caller-owned storage. The DSF
    // channel blocks are deinterleaved across block groups while preserving
    // their encoded bit order. The final block's zero padding is omitted.
    [[nodiscard]] std::expected<std::size_t, ParseError> read_channel_bytes(
        std::uint32_t channel,
        std::uint64_t first_byte,
        std::span<std::byte> destination) const noexcept;
};

// Parses the standard little-endian DSF container and validates its fixed
// channel-block layout. Unknown, well-bounded chunks are skipped.
[[nodiscard]] std::expected<DsfFile, ParseError> parse(
    std::span<const std::byte> file) noexcept;

} // namespace ps5_sacd::formats::dsf
