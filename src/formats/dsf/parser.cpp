#include "ps5_sacd/formats/dsf/parser.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace ps5_sacd::formats::dsf {
namespace {

constexpr std::size_t kDsdHeaderSize = 28;
constexpr std::size_t kChunkHeaderSize = 12;
constexpr std::size_t kFormatChunkSize = 52;
constexpr std::size_t kId3HeaderSize = 10;

[[nodiscard]] bool has_range(
    std::size_t total_size,
    std::size_t offset,
    std::size_t length) noexcept {
    return offset <= total_size && length <= total_size - offset;
}

[[nodiscard]] std::uint8_t byte_at(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] bool matches(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::array<char, 4> identifier) noexcept {
    for (std::size_t index = 0; index < identifier.size(); ++index) {
        if (byte_at(bytes, offset + index) != static_cast<std::uint8_t>(identifier[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t read_u32_le(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(byte_at(bytes, offset + index)) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64_le(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(byte_at(bytes, offset + index)) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool is_power_of_two(std::uint32_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] bool is_dsd_sample_rate(std::uint32_t rate) noexcept {
    // Accept the established 44.1 kHz and 48 kHz DSD rate families from DSD64
    // through DSD2048. Other clock rates are rejected instead of being
    // interpreted as DSD by accident.
    constexpr std::uint32_t kMinimumRatio = 64;
    constexpr std::uint32_t kMaximumRatio = 2048;
    for (const std::uint32_t base_rate : {44'100U, 48'000U}) {
        if (rate % base_rate != 0U) {
            continue;
        }
        const auto ratio = rate / base_rate;
        if (ratio >= kMinimumRatio && ratio <= kMaximumRatio && is_power_of_two(ratio)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint32_t expected_channel_count(std::uint32_t channel_type) noexcept {
    constexpr std::array<std::uint32_t, 8> counts{0U, 1U, 2U, 3U, 4U, 4U, 5U, 6U};
    return channel_type < counts.size() ? counts[channel_type] : 0U;
}

[[nodiscard]] std::expected<std::uint64_t, ParseError> checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::size_t error_offset) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::unexpected(ParseError{ErrorCode::invalid_sample_count, error_offset});
    }
    return left * right;
}

[[nodiscard]] std::expected<std::span<const std::byte>, ParseError> validate_id3(
    std::span<const std::byte> file,
    std::size_t offset) noexcept {
    if (!has_range(file.size(), offset, kId3HeaderSize)) {
        return std::unexpected(ParseError{ErrorCode::invalid_metadata, offset});
    }
    if (byte_at(file, offset) != static_cast<std::uint8_t>('I')
        || byte_at(file, offset + 1U) != static_cast<std::uint8_t>('D')
        || byte_at(file, offset + 2U) != static_cast<std::uint8_t>('3')) {
        return std::unexpected(ParseError{ErrorCode::invalid_metadata, offset});
    }

    std::uint32_t tag_size = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const auto size_byte = byte_at(file, offset + 6U + index);
        if ((size_byte & 0x80U) != 0U) {
            return std::unexpected(ParseError{ErrorCode::invalid_metadata, offset + 6U + index});
        }
        tag_size = (tag_size << 7U) | size_byte;
    }

    const std::uint64_t footer_size = (byte_at(file, offset + 5U) & 0x10U) != 0U ? 10U : 0U;
    const std::uint64_t total_size = static_cast<std::uint64_t>(kId3HeaderSize)
        + static_cast<std::uint64_t>(tag_size)
        + footer_size;
    const auto available = static_cast<std::uint64_t>(file.size() - offset);
    if (total_size > available) {
        return std::unexpected(ParseError{ErrorCode::invalid_metadata, offset});
    }
    return file.subspan(offset, static_cast<std::size_t>(total_size));
}

} // namespace

std::string_view message(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::truncated_input: return "input is too short for the required field";
    case ErrorCode::invalid_identifier: return "invalid DSF or chunk identifier";
    case ErrorCode::invalid_file_size: return "declared file size does not match the input";
    case ErrorCode::invalid_metadata_pointer: return "metadata pointer is outside the metadata region";
    case ErrorCode::invalid_metadata: return "metadata is not a complete ID3v2 tag";
    case ErrorCode::invalid_chunk_size: return "chunk size is smaller than its header or required fields";
    case ErrorCode::chunk_out_of_bounds: return "chunk extends beyond the declared file region";
    case ErrorCode::invalid_chunk_order: return "required DSF chunks are in an invalid order";
    case ErrorCode::duplicate_chunk: return "duplicate required DSF chunk";
    case ErrorCode::missing_required_chunk: return "required DSF chunk is missing";
    case ErrorCode::unsupported_format_version: return "unsupported DSF format version";
    case ErrorCode::unsupported_format_id: return "unsupported DSF format ID";
    case ErrorCode::invalid_channel_layout: return "invalid or inconsistent channel layout";
    case ErrorCode::invalid_sample_rate: return "sample rate is not a supported DSD rate";
    case ErrorCode::unsupported_bits_per_sample: return "DSF bits-per-sample value must be 1 or 8";
    case ErrorCode::invalid_sample_count: return "sample count is zero or cannot be represented safely";
    case ErrorCode::invalid_block_size: return "per-channel block size is zero";
    case ErrorCode::invalid_reserved_field: return "reserved DSF format field is nonzero";
    case ErrorCode::data_size_mismatch: return "audio payload size does not match the sample count and block layout";
    case ErrorCode::invalid_channel_index: return "requested channel does not exist";
    case ErrorCode::channel_offset_out_of_range: return "requested channel byte offset exceeds the sample data";
    }
    return "unknown DSF parsing error";
}

std::expected<std::size_t, ParseError> DsfFile::read_channel_bytes(
    std::uint32_t channel,
    std::uint64_t first_byte,
    std::span<std::byte> destination) const noexcept {
    if (channel >= format.channel_count) {
        return std::unexpected(ParseError{ErrorCode::invalid_channel_index, 0U});
    }

    const auto logical_bytes = format.byte_count_per_channel();
    if (first_byte > logical_bytes) {
        return std::unexpected(ParseError{ErrorCode::channel_offset_out_of_range, 0U});
    }

    const auto available_bytes = logical_bytes - first_byte;
    const auto requested_bytes = std::min<std::uint64_t>(
        available_bytes,
        static_cast<std::uint64_t>(destination.size()));
    const auto block_size = static_cast<std::uint64_t>(format.block_size_per_channel);
    const auto channel_count = static_cast<std::uint64_t>(format.channel_count);
    const auto block_group_size = block_size * channel_count;

    std::uint64_t logical_offset = first_byte;
    std::uint64_t remaining = requested_bytes;
    std::size_t output_offset = 0U;
    while (remaining > 0U) {
        const auto block_index = logical_offset / block_size;
        const auto byte_in_block = logical_offset % block_size;
        const auto bytes_this_block = std::min(remaining, block_size - byte_in_block);
        const auto file_offset = block_index * block_group_size
            + static_cast<std::uint64_t>(channel) * block_size
            + byte_in_block;
        const auto source_offset = static_cast<std::size_t>(file_offset);
        const auto copy_size = static_cast<std::size_t>(bytes_this_block);
        std::copy_n(audio_data.begin() + static_cast<std::ptrdiff_t>(source_offset),
                    copy_size,
                    destination.begin() + static_cast<std::ptrdiff_t>(output_offset));
        logical_offset += bytes_this_block;
        remaining -= bytes_this_block;
        output_offset += copy_size;
    }

    return output_offset;
}

std::expected<DsfFile, ParseError> parse(std::span<const std::byte> file) noexcept {
    if (file.size() < kDsdHeaderSize) {
        return std::unexpected(ParseError{ErrorCode::truncated_input, file.size()});
    }
    if (!matches(file, 0U, {'D', 'S', 'D', ' '})) {
        return std::unexpected(ParseError{ErrorCode::invalid_identifier, 0U});
    }
    if (read_u64_le(file, 4U) != kDsdHeaderSize) {
        return std::unexpected(ParseError{ErrorCode::invalid_chunk_size, 4U});
    }

    const auto declared_file_size = read_u64_le(file, 12U);
    if (declared_file_size != static_cast<std::uint64_t>(file.size())) {
        return std::unexpected(ParseError{ErrorCode::invalid_file_size, 12U});
    }

    const auto metadata_pointer_raw = read_u64_le(file, 20U);
    const auto file_size_u64 = static_cast<std::uint64_t>(file.size());
    if (metadata_pointer_raw != 0U
        && (metadata_pointer_raw < kDsdHeaderSize || metadata_pointer_raw >= file_size_u64)) {
        return std::unexpected(ParseError{ErrorCode::invalid_metadata_pointer, 20U});
    }

    const auto chunk_region_end = metadata_pointer_raw == 0U
        ? file.size()
        : static_cast<std::size_t>(metadata_pointer_raw);
    std::optional<std::span<const std::byte>> id3_metadata;
    if (metadata_pointer_raw != 0U) {
        auto metadata_result = validate_id3(file, chunk_region_end);
        if (!metadata_result) {
            return std::unexpected(metadata_result.error());
        }
        id3_metadata = *metadata_result;
    }

    std::optional<FormatInfo> format;
    std::optional<std::span<const std::byte>> audio_data;
    std::size_t cursor = kDsdHeaderSize;
    while (cursor < chunk_region_end) {
        if (!has_range(chunk_region_end, cursor, kChunkHeaderSize)) {
            return std::unexpected(ParseError{ErrorCode::truncated_input, cursor});
        }

        const auto chunk_size_u64 = read_u64_le(file, cursor + 4U);
        if (chunk_size_u64 < kChunkHeaderSize) {
            return std::unexpected(ParseError{ErrorCode::invalid_chunk_size, cursor + 4U});
        }
        const auto available = chunk_region_end - cursor;
        if (chunk_size_u64 > static_cast<std::uint64_t>(available)) {
            return std::unexpected(ParseError{ErrorCode::chunk_out_of_bounds, cursor + 4U});
        }
        const auto chunk_size = static_cast<std::size_t>(chunk_size_u64);
        const auto payload_offset = cursor + kChunkHeaderSize;
        const auto payload_size = chunk_size - kChunkHeaderSize;

        if (matches(file, cursor, {'f', 'm', 't', ' '})) {
            if (format.has_value()) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, cursor});
            }
            if (chunk_size < kFormatChunkSize) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_size, cursor + 4U});
            }

            const auto version = read_u32_le(file, payload_offset);
            const auto format_id = read_u32_le(file, payload_offset + 4U);
            const auto channel_type = read_u32_le(file, payload_offset + 8U);
            const auto channel_count = read_u32_le(file, payload_offset + 12U);
            const auto sample_rate = read_u32_le(file, payload_offset + 16U);
            const auto bits_per_sample = read_u32_le(file, payload_offset + 20U);
            const auto sample_count = read_u64_le(file, payload_offset + 24U);
            const auto block_size = read_u32_le(file, payload_offset + 32U);
            const auto reserved = read_u32_le(file, payload_offset + 36U);

            if (version != 1U) {
                return std::unexpected(ParseError{ErrorCode::unsupported_format_version, payload_offset});
            }
            if (format_id != 0U) {
                return std::unexpected(ParseError{ErrorCode::unsupported_format_id, payload_offset + 4U});
            }
            if (channel_type == 0U || channel_type > 7U
                || expected_channel_count(channel_type) != channel_count) {
                return std::unexpected(ParseError{ErrorCode::invalid_channel_layout, payload_offset + 8U});
            }
            if (!is_dsd_sample_rate(sample_rate)) {
                return std::unexpected(ParseError{ErrorCode::invalid_sample_rate, payload_offset + 16U});
            }
            if (bits_per_sample != 1U && bits_per_sample != 8U) {
                return std::unexpected(ParseError{ErrorCode::unsupported_bits_per_sample, payload_offset + 20U});
            }
            if (sample_count == 0U) {
                return std::unexpected(ParseError{ErrorCode::invalid_sample_count, payload_offset + 24U});
            }
            if (block_size == 0U) {
                return std::unexpected(ParseError{ErrorCode::invalid_block_size, payload_offset + 32U});
            }
            if (reserved != 0U) {
                return std::unexpected(ParseError{ErrorCode::invalid_reserved_field, payload_offset + 36U});
            }

            format = FormatInfo{
                .channel_type = channel_type,
                .channel_count = channel_count,
                .sample_rate_hz = sample_rate,
                .bits_per_sample = bits_per_sample,
                .sample_count_per_channel = sample_count,
                .block_size_per_channel = block_size,
            };
        } else if (matches(file, cursor, {'d', 'a', 't', 'a'})) {
            if (audio_data.has_value()) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, cursor});
            }
            if (!format.has_value()) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_order, cursor});
            }
            audio_data = file.subspan(payload_offset, payload_size);
        }

        cursor += chunk_size;
    }

    if (!format.has_value() || !audio_data.has_value()) {
        return std::unexpected(ParseError{ErrorCode::missing_required_chunk, cursor});
    }

    const auto bytes_per_channel = format->byte_count_per_channel();
    const auto block_size = static_cast<std::uint64_t>(format->block_size_per_channel);
    const auto block_count = bytes_per_channel / block_size
        + (bytes_per_channel % block_size == 0U ? 0U : 1U);
    auto channel_block_bytes = checked_multiply(
        block_count,
        static_cast<std::uint64_t>(format->channel_count),
        cursor);
    if (!channel_block_bytes) {
        return std::unexpected(channel_block_bytes.error());
    }
    auto expected_audio_size = checked_multiply(*channel_block_bytes, block_size, cursor);
    if (!expected_audio_size) {
        return std::unexpected(expected_audio_size.error());
    }
    if (*expected_audio_size != static_cast<std::uint64_t>(audio_data->size())) {
        return std::unexpected(ParseError{ErrorCode::data_size_mismatch, cursor});
    }

    return DsfFile{
        .format = *format,
        .audio_data = *audio_data,
        .id3_metadata = id3_metadata,
    };
}

} // namespace ps5_sacd::formats::dsf
