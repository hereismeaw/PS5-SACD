#include "ps5_sacd/formats/dsdiff/parser.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace ps5_sacd::formats::dsdiff {
namespace {

constexpr std::size_t kChunkHeaderSize = 12U;
constexpr std::size_t kFormHeaderSize = 16U;
constexpr std::uint32_t kSupportedMajorVersion = 1U;
constexpr std::array<char, 4> kFormId{'F', 'R', 'M', '8'};
constexpr std::array<char, 4> kFormType{'D', 'S', 'D', ' '};
constexpr std::array<char, 4> kFverId{'F', 'V', 'E', 'R'};
constexpr std::array<char, 4> kPropId{'P', 'R', 'O', 'P'};
constexpr std::array<char, 4> kSoundType{'S', 'N', 'D', ' '};
constexpr std::array<char, 4> kSampleRateId{'F', 'S', ' ', ' '};
constexpr std::array<char, 4> kChannelsId{'C', 'H', 'N', 'L'};
constexpr std::array<char, 4> kCompressionId{'C', 'M', 'P', 'R'};
constexpr std::array<char, 4> kLoudspeakerConfigurationId{'L', 'S', 'C', 'O'};
constexpr std::array<char, 4> kDsdId{'D', 'S', 'D', ' '};
constexpr std::array<char, 4> kDstId{'D', 'S', 'T', ' '};

struct ChunkView {
    std::array<char, 4> id{};
    std::span<const std::byte> payload;
    std::size_t header_offset{};
};

struct Properties {
    std::optional<std::uint32_t> sample_rate_hz;
    std::optional<std::vector<ChannelId>> channel_ids;
    std::optional<ChannelId> compression_type;
    std::optional<std::uint16_t> loudspeaker_configuration;
};

[[nodiscard]] std::uint8_t byte_at(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] bool matches(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const std::array<char, 4>& id) noexcept {
    for (std::size_t index = 0; index < id.size(); ++index) {
        if (byte_at(bytes, offset + index) != static_cast<std::uint8_t>(id[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::array<char, 4> read_id(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::array<char, 4> id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<char>(byte_at(bytes, offset + index));
    }
    return id;
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
    std::uint32_t value = 0U;
    for (unsigned int index = 0; index < 4U; ++index) {
        value = (value << 8U) | byte_at(bytes, offset + index);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64_be(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (unsigned int index = 0; index < 8U; ++index) {
        value = (value << 8U) | byte_at(bytes, offset + index);
    }
    return value;
}

[[nodiscard]] bool is_dsd_sample_rate(std::uint32_t rate) noexcept {
    constexpr std::array<std::uint32_t, 2> base_rates{44'100U, 48'000U};
    for (const auto base_rate : base_rates) {
        if (rate % base_rate != 0U) {
            continue;
        }
        const auto ratio = rate / base_rate;
        if (ratio >= 64U && ratio <= 2048U && (ratio & (ratio - 1U)) == 0U) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::expected<ChunkView, ParseError> read_chunk(
    std::span<const std::byte> file,
    std::size_t& cursor,
    std::size_t end) noexcept {
    if (cursor > end || end - cursor < kChunkHeaderSize) {
        return std::unexpected(ParseError{ErrorCode::truncated_input, cursor});
    }

    const auto chunk_size = read_u64_be(file, cursor + 4U);
    const auto payload_offset = cursor + kChunkHeaderSize;
    const auto remaining = end - payload_offset;
    if (chunk_size > static_cast<std::uint64_t>(remaining)) {
        return std::unexpected(ParseError{ErrorCode::chunk_out_of_bounds, cursor + 4U});
    }

    const auto payload_size = static_cast<std::size_t>(chunk_size);
    const auto pad_size = payload_size & 1U;
    if (pad_size > remaining - payload_size) {
        return std::unexpected(ParseError{ErrorCode::invalid_chunk_padding, payload_offset + payload_size});
    }

    ChunkView chunk{
        read_id(file, cursor),
        file.subspan(payload_offset, payload_size),
        cursor,
    };
    cursor = payload_offset + payload_size + pad_size;
    return chunk;
}

[[nodiscard]] std::expected<void, ParseError> parse_properties(
    const ChunkView& property_chunk,
    Properties& properties) {
    if (property_chunk.payload.size() < 4U
        || !matches(property_chunk.payload, 0U, kSoundType)) {
        return std::unexpected(ParseError{ErrorCode::invalid_property, property_chunk.header_offset});
    }

    std::size_t cursor = 4U;
    const auto end = property_chunk.payload.size();
    while (cursor < end) {
        auto chunk_result = read_chunk(property_chunk.payload, cursor, end);
        if (!chunk_result) {
            return std::unexpected(chunk_result.error());
        }
        const auto& chunk = *chunk_result;

        if (chunk.id == kSampleRateId) {
            if (properties.sample_rate_hz) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (chunk.payload.size() != 4U) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_size, chunk.header_offset + 4U});
            }
            properties.sample_rate_hz = read_u32_be(chunk.payload, 0U);
        } else if (chunk.id == kChannelsId) {
            if (properties.channel_ids) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (chunk.payload.size() < 2U) {
                return std::unexpected(ParseError{ErrorCode::invalid_channel_layout, chunk.header_offset});
            }
            const auto channel_count = read_u16_be(chunk.payload, 0U);
            const auto expected_size = 2U + static_cast<std::size_t>(channel_count) * 4U;
            if (channel_count == 0U || chunk.payload.size() != expected_size) {
                return std::unexpected(ParseError{ErrorCode::invalid_channel_layout, chunk.header_offset});
            }
            std::vector<ChannelId> channel_ids;
            channel_ids.reserve(channel_count);
            for (std::size_t index = 0; index < channel_count; ++index) {
                channel_ids.push_back(read_id(chunk.payload, 2U + index * 4U));
            }
            properties.channel_ids = std::move(channel_ids);
        } else if (chunk.id == kCompressionId) {
            if (properties.compression_type) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (chunk.payload.size() < 5U) {
                return std::unexpected(ParseError{ErrorCode::invalid_compression, chunk.header_offset});
            }
            const auto name_length = byte_at(chunk.payload, 4U);
            if (chunk.payload.size() != 5U + static_cast<std::size_t>(name_length)) {
                return std::unexpected(ParseError{ErrorCode::invalid_compression, chunk.header_offset});
            }
            properties.compression_type = read_id(chunk.payload, 0U);
        } else if (chunk.id == kLoudspeakerConfigurationId) {
            if (properties.loudspeaker_configuration) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (chunk.payload.size() != 2U) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_size, chunk.header_offset + 4U});
            }
            properties.loudspeaker_configuration = read_u16_be(chunk.payload, 0U);
        }
    }

    if (!properties.sample_rate_hz || !properties.channel_ids || !properties.compression_type) {
        return std::unexpected(ParseError{ErrorCode::missing_required_chunk, property_chunk.header_offset});
    }
    if (!is_dsd_sample_rate(*properties.sample_rate_hz)) {
        return std::unexpected(ParseError{ErrorCode::invalid_sample_rate, property_chunk.header_offset});
    }
    return {};
}

} // namespace

std::string_view message(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::truncated_input: return "input ends inside a required DSDIFF field";
    case ErrorCode::invalid_identifier: return "invalid DSDIFF identifier";
    case ErrorCode::invalid_form_size: return "invalid FRM8 form size";
    case ErrorCode::invalid_file_size: return "FRM8 size does not match the file";
    case ErrorCode::invalid_chunk_size: return "invalid DSDIFF chunk size";
    case ErrorCode::chunk_out_of_bounds: return "chunk extends beyond its parent";
    case ErrorCode::invalid_chunk_padding: return "odd-sized chunk is missing its pad byte";
    case ErrorCode::invalid_chunk_order: return "required DSDIFF chunks are out of order";
    case ErrorCode::duplicate_chunk: return "duplicate required DSDIFF chunk";
    case ErrorCode::missing_required_chunk: return "required DSDIFF chunk is missing";
    case ErrorCode::unsupported_version: return "unsupported DSDIFF format version";
    case ErrorCode::invalid_property: return "invalid DSDIFF sound property";
    case ErrorCode::invalid_channel_layout: return "invalid DSDIFF channel layout";
    case ErrorCode::invalid_sample_rate: return "sample rate is not a supported DSD rate";
    case ErrorCode::invalid_compression: return "invalid DSDIFF compression metadata";
    case ErrorCode::unsupported_compression: return "DSDIFF compression is not uncompressed DSD";
    case ErrorCode::invalid_audio_size: return "DSDIFF audio size does not contain whole channel frames";
    case ErrorCode::invalid_channel_index: return "requested channel does not exist";
    case ErrorCode::channel_offset_out_of_range: return "requested channel byte offset exceeds the audio data";
    }
    return "unknown DSDIFF parsing error";
}

std::expected<std::size_t, ParseError> DsdiffFile::read_channel_bytes(
    std::uint32_t channel,
    std::uint64_t first_byte,
    std::span<std::byte> destination) const noexcept {
    if (channel >= channel_count()) {
        return std::unexpected(ParseError{ErrorCode::invalid_channel_index, 0U});
    }

    if (first_byte > sample_count_per_channel / 8U) {
        return std::unexpected(ParseError{ErrorCode::channel_offset_out_of_range, 0U});
    }

    const auto logical_bytes = sample_count_per_channel / 8U;
    const auto bytes_to_copy = std::min<std::uint64_t>(
        logical_bytes - first_byte,
        static_cast<std::uint64_t>(destination.size()));
    const auto channels = static_cast<std::uint64_t>(channel_count());
    const auto channel_index = static_cast<std::uint64_t>(channel);
    for (std::uint64_t index = 0U; index < bytes_to_copy; ++index) {
        const auto source_offset = static_cast<std::size_t>((first_byte + index) * channels + channel_index);
        destination[static_cast<std::size_t>(index)] = audio_data[source_offset];
    }
    return static_cast<std::size_t>(bytes_to_copy);
}

std::expected<DsdiffFile, ParseError> parse(std::span<const std::byte> file) {
    if (file.size() < kFormHeaderSize) {
        return std::unexpected(ParseError{ErrorCode::truncated_input, file.size()});
    }
    if (!matches(file, 0U, kFormId) || !matches(file, 12U, kFormType)) {
        return std::unexpected(ParseError{ErrorCode::invalid_identifier, 0U});
    }

    const auto declared_size = read_u64_be(file, 4U);
    if (declared_size < 4U || (declared_size & 1U) != 0U) {
        return std::unexpected(ParseError{ErrorCode::invalid_form_size, 4U});
    }
    if (declared_size != static_cast<std::uint64_t>(file.size() - 12U)) {
        return std::unexpected(ParseError{ErrorCode::invalid_file_size, 4U});
    }

    Properties properties;
    std::optional<std::span<const std::byte>> audio_data;
    bool saw_version = false;
    bool saw_property = false;
    bool saw_audio = false;
    std::size_t cursor = kFormHeaderSize;
    const auto end = file.size();
    while (cursor < end) {
        auto chunk_result = read_chunk(file, cursor, end);
        if (!chunk_result) {
            return std::unexpected(chunk_result.error());
        }
        const auto& chunk = *chunk_result;

        if (!saw_version && chunk.id != kFverId) {
            return std::unexpected(ParseError{ErrorCode::invalid_chunk_order, chunk.header_offset});
        }

        if (chunk.id == kFverId) {
            if (saw_version) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (chunk.payload.size() != 4U) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_size, chunk.header_offset + 4U});
            }
            const auto version = read_u32_be(chunk.payload, 0U);
            if ((version >> 24U) != kSupportedMajorVersion) {
                return std::unexpected(ParseError{ErrorCode::unsupported_version, chunk.header_offset + 12U});
            }
            saw_version = true;
        } else if (chunk.id == kPropId) {
            if (saw_property) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (saw_audio) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_order, chunk.header_offset});
            }
            auto property_result = parse_properties(chunk, properties);
            if (!property_result) {
                return std::unexpected(property_result.error());
            }
            if (*properties.compression_type != kDsdId) {
                return std::unexpected(ParseError{ErrorCode::unsupported_compression, chunk.header_offset});
            }
            saw_property = true;
        } else if (chunk.id == kDsdId || chunk.id == kDstId) {
            if (!saw_property) {
                return std::unexpected(ParseError{ErrorCode::invalid_chunk_order, chunk.header_offset});
            }
            if (saw_audio) {
                return std::unexpected(ParseError{ErrorCode::duplicate_chunk, chunk.header_offset});
            }
            if (!properties.compression_type || *properties.compression_type != kDsdId
                || chunk.id != kDsdId) {
                return std::unexpected(ParseError{ErrorCode::unsupported_compression, chunk.header_offset});
            }
            audio_data = chunk.payload;
            saw_audio = true;
        }
    }

    if (!saw_version || !saw_property || !saw_audio || !audio_data) {
        return std::unexpected(ParseError{ErrorCode::missing_required_chunk, cursor});
    }

    const auto channel_count = properties.channel_ids->size();
    if (audio_data->empty() || audio_data->size() % channel_count != 0U) {
        return std::unexpected(ParseError{ErrorCode::invalid_audio_size, 0U});
    }
    const auto bytes_per_channel = audio_data->size() / channel_count;
    if (bytes_per_channel > std::numeric_limits<std::uint64_t>::max() / 8U) {
        return std::unexpected(ParseError{ErrorCode::invalid_audio_size, 0U});
    }

    DsdiffFile result;
    result.sample_rate_hz = *properties.sample_rate_hz;
    result.sample_count_per_channel = static_cast<std::uint64_t>(bytes_per_channel) * 8U;
    result.channel_ids = std::move(*properties.channel_ids);
    result.loudspeaker_configuration = properties.loudspeaker_configuration;
    result.audio_data = *audio_data;
    return result;
}

} // namespace ps5_sacd::formats::dsdiff
