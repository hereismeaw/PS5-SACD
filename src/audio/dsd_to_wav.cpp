#include "ps5_sacd/audio/dsd_to_wav.hpp"

#include "ps5_sacd/audio/dsd_to_pcm.hpp"
#include "ps5_sacd/formats/dsdiff/parser.hpp"
#include "ps5_sacd/formats/dsf/parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <utility>
#include <vector>

namespace ps5_sacd::audio {
namespace {

constexpr std::uint64_t kBytesPerChannelChunk = 4096U;
constexpr std::uint32_t kWavHeaderSize = 68U;
constexpr std::uint32_t kWavRiffOverhead = kWavHeaderSize - 8U;
constexpr std::uint32_t kPcmBitsPerSample = 24U;
constexpr std::uint32_t kBytesPerPcmSample = kPcmBitsPerSample / 8U;

struct SourceInfo {
    std::uint32_t sample_rate_hz;
    std::uint32_t channel_count;
    std::uint64_t sample_count_per_channel;
    formats::DsdBitOrder bit_order;
    std::uint32_t channel_mask;
    std::vector<std::uint32_t> output_channel_order;
};

[[nodiscard]] bool matches(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const std::array<char, 4>& id) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < id.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < id.size(); ++index) {
        if (std::to_integer<std::uint8_t>(bytes[offset + index])
            != static_cast<std::uint8_t>(id[index])) {
            return false;
        }
    }
    return true;
}

void write_u16_le(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32_le(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (unsigned int index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_id(std::span<std::byte> bytes, std::size_t offset, const std::array<char, 4>& id) noexcept {
    for (std::size_t index = 0U; index < id.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(id[index]);
    }
}

[[nodiscard]] std::expected<std::uint64_t, WavConversionError> output_frame_count(
    std::uint64_t input_sample_count,
    std::uint32_t input_rate,
    std::uint32_t output_rate) noexcept {
    const auto whole_seconds = input_sample_count / input_rate;
    const auto remaining_samples = input_sample_count % input_rate;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / output_rate) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::wav_size_limit_exceeded, 0U, "PCM frame count overflows"});
    }

    const auto whole_frames = whole_seconds * output_rate;
    const auto fractional_product = remaining_samples * static_cast<std::uint64_t>(output_rate);
    const auto fractional_frames = fractional_product / input_rate
        + (fractional_product % input_rate == 0U ? 0U : 1U);
    if (fractional_frames > std::numeric_limits<std::uint64_t>::max() - whole_frames) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::wav_size_limit_exceeded, 0U, "PCM frame count overflows"});
    }
    return whole_frames + fractional_frames;
}

[[nodiscard]] std::uint32_t choose_default_rate(std::uint32_t dsd_rate) noexcept {
    return dsd_rate % 44'100U == 0U ? 44'100U : 48'000U;
}

[[nodiscard]] std::uint32_t dsf_channel_mask(std::uint32_t channel_type) noexcept {
    switch (channel_type) {
    case 1U: return 0x00000004U; // center
    case 2U: return 0x00000003U; // front left, front right
    case 3U: return 0x00000007U; // front left, front right, center
    case 4U: return 0x00000033U; // quad
    case 5U: return 0x0000000fU; // 3.1
    case 6U: return 0x00000037U; // 5.0
    case 7U: return 0x0000003fU; // 5.1
    default: return 0U;
    }
}

[[nodiscard]] std::optional<std::uint32_t> wav_speaker_bit(
    const formats::dsdiff::ChannelId& id,
    bool include_surround) noexcept {
    if (id == formats::dsdiff::ChannelId{'S', 'L', 'F', 'T'}
        || id == formats::dsdiff::ChannelId{'M', 'L', 'F', 'T'}) {
        return 0x00000001U;
    }
    if (id == formats::dsdiff::ChannelId{'S', 'R', 'G', 'T'}
        || id == formats::dsdiff::ChannelId{'M', 'R', 'G', 'T'}) {
        return 0x00000002U;
    }
    if (id == formats::dsdiff::ChannelId{'C', ' ', ' ', ' '}) {
        return 0x00000004U;
    }
    if (id == formats::dsdiff::ChannelId{'L', 'F', 'E', ' '}) {
        return 0x00000008U;
    }
    if (include_surround && id == formats::dsdiff::ChannelId{'L', 'S', ' ', ' '}) {
        return 0x00000010U;
    }
    if (include_surround && id == formats::dsdiff::ChannelId{'R', 'S', ' ', ' '}) {
        return 0x00000020U;
    }
    return std::nullopt;
}

[[nodiscard]] SourceInfo dsdiff_source_info(const formats::dsdiff::DsdiffFile& file) {
    SourceInfo source{
        file.sample_rate_hz,
        file.channel_count(),
        file.sample_count_per_channel,
        file.bit_order(),
        0U,
        {},
    };
    const auto configuration = file.loudspeaker_configuration;
    if (configuration && *configuration != 0U && *configuration != 3U
        && *configuration != 4U && *configuration != 65'535U) {
        return source;
    }

    const auto include_surround = configuration == 3U || configuration == 4U;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> speakers;
    speakers.reserve(file.channel_ids.size());
    std::uint32_t channel_mask = 0U;
    for (std::uint32_t channel = 0U; channel < file.channel_count(); ++channel) {
        const auto speaker_bit = wav_speaker_bit(file.channel_ids[channel], include_surround);
        if (!speaker_bit || (channel_mask & *speaker_bit) != 0U) {
            return source;
        }
        channel_mask |= *speaker_bit;
        speakers.emplace_back(*speaker_bit, channel);
    }

    bool valid_layout = false;
    switch (file.channel_count()) {
    case 1U: valid_layout = channel_mask == 0x00000004U; break;
    case 2U: valid_layout = channel_mask == 0x00000003U; break;
    case 3U: valid_layout = channel_mask == 0x00000007U; break;
    case 4U: valid_layout = channel_mask == 0x0000000fU; break;
    case 5U: valid_layout = configuration == 3U && channel_mask == 0x00000037U; break;
    case 6U: valid_layout = configuration == 4U && channel_mask == 0x0000003fU; break;
    default: break;
    }
    if (!valid_layout) {
        return source;
    }

    std::sort(speakers.begin(), speakers.end());
    source.channel_mask = channel_mask;
    source.output_channel_order.reserve(speakers.size());
    for (const auto& speaker : speakers) {
        source.output_channel_order.push_back(speaker.second);
    }
    return source;
}

[[nodiscard]] std::string_view converter_error_message(ConversionErrorCode code) noexcept {
    switch (code) {
    case ConversionErrorCode::invalid_dsd_sample_rate: return "invalid DSD sample rate";
    case ConversionErrorCode::invalid_pcm_sample_rate: return "invalid PCM sample rate";
    case ConversionErrorCode::pcm_rate_must_be_lower_than_dsd_rate: return "PCM rate must be below DSD rate";
    case ConversionErrorCode::unsupported_rate_pair: return "unsupported DSD/PCM rate pair";
    case ConversionErrorCode::invalid_filter_length: return "invalid FIR filter length";
    case ConversionErrorCode::sample_count_overflow: return "sample count overflow";
    case ConversionErrorCode::invalid_final_byte_bit_count: return "invalid number of valid bits in final byte";
    case ConversionErrorCode::converter_already_finished: return "converter was already finished";
    }
    return "unknown DSD conversion error";
}

[[nodiscard]] WavConversionError make_converter_error(ConversionErrorCode code) noexcept {
    return WavConversionError{
        WavConversionErrorCode::dsd_conversion_failed,
        0U,
        converter_error_message(code),
    };
}

[[nodiscard]] std::array<std::byte, kWavHeaderSize> make_wav_header(
    std::uint32_t channel_count,
    std::uint32_t sample_rate_hz,
    std::uint32_t data_size_bytes,
    std::uint32_t channel_mask) noexcept {
    std::array<std::byte, kWavHeaderSize> header{};
    const auto bytes = std::span<std::byte>(header);
    const auto block_align = static_cast<std::uint16_t>(channel_count * kBytesPerPcmSample);
    const auto byte_rate = sample_rate_hz * static_cast<std::uint32_t>(block_align);
    write_id(bytes, 0U, {'R', 'I', 'F', 'F'});
    write_u32_le(bytes, 4U, kWavRiffOverhead + data_size_bytes);
    write_id(bytes, 8U, {'W', 'A', 'V', 'E'});
    write_id(bytes, 12U, {'f', 'm', 't', ' '});
    write_u32_le(bytes, 16U, 40U);
    write_u16_le(bytes, 20U, 0xfffeU);
    write_u16_le(bytes, 22U, static_cast<std::uint16_t>(channel_count));
    write_u32_le(bytes, 24U, sample_rate_hz);
    write_u32_le(bytes, 28U, byte_rate);
    write_u16_le(bytes, 32U, block_align);
    write_u16_le(bytes, 34U, static_cast<std::uint16_t>(kPcmBitsPerSample));
    write_u16_le(bytes, 36U, 22U);
    write_u16_le(bytes, 38U, static_cast<std::uint16_t>(kPcmBitsPerSample));
    write_u32_le(bytes, 40U, channel_mask);

    constexpr std::array<std::byte, 16> kPcmSubformat{
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00},
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0xaa},
        std::byte{0x00}, std::byte{0x38}, std::byte{0x9b}, std::byte{0x71},
    };
    std::copy(kPcmSubformat.begin(), kPcmSubformat.end(), header.begin() + 44);
    write_id(bytes, 60U, {'d', 'a', 't', 'a'});
    write_u32_le(bytes, 64U, data_size_bytes);
    return header;
}

[[nodiscard]] std::array<char, 3> encode_pcm24(float sample) noexcept {
    const auto limited = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    const auto scale = limited < 0.0 ? 8'388'608.0 : 8'388'607.0;
    const auto quantized = static_cast<std::int32_t>(std::llround(limited * scale));
    const auto bits = static_cast<std::uint32_t>(quantized);
    return {
        static_cast<char>(bits & 0xffU),
        static_cast<char>((bits >> 8U) & 0xffU),
        static_cast<char>((bits >> 16U) & 0xffU),
    };
}

[[nodiscard]] std::expected<void, WavConversionError> write_interleaved_samples(
    std::ostream& output,
    const std::vector<std::vector<float>>& channels,
    std::span<const std::uint32_t> output_channel_order) {
    if (channels.empty()) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::inconsistent_channel_output, 0U, "no channel data"});
    }
    const auto frame_count = channels.front().size();
    if (std::any_of(channels.begin(), channels.end(), [frame_count](const auto& channel) {
            return channel.size() != frame_count;
        })) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::inconsistent_channel_output, 0U, "channel frame counts differ"});
    }

    if (frame_count == 0U) {
        return {};
    }
    if (channels.size() > std::numeric_limits<std::size_t>::max() / kBytesPerPcmSample
        || frame_count > std::numeric_limits<std::size_t>::max()
            / (channels.size() * kBytesPerPcmSample)) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::wav_size_limit_exceeded, 0U, "PCM block size overflows memory limits"});
    }

    std::vector<char> pcm_bytes;
    const auto pcm_block_size = frame_count * channels.size() * kBytesPerPcmSample;
    if (pcm_block_size > pcm_bytes.max_size()
        || pcm_block_size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::wav_size_limit_exceeded, 0U, "PCM block exceeds stream limits"});
    }
    pcm_bytes.reserve(pcm_block_size);
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        for (std::size_t output_channel = 0U; output_channel < channels.size(); ++output_channel) {
            const auto source_channel = output_channel_order.empty()
                ? output_channel
                : static_cast<std::size_t>(output_channel_order[output_channel]);
            const auto sample_bytes = encode_pcm24(channels[source_channel][frame]);
            pcm_bytes.insert(pcm_bytes.end(), sample_bytes.begin(), sample_bytes.end());
        }
    }
    output.write(pcm_bytes.data(), static_cast<std::streamsize>(pcm_bytes.size()));
    if (!output) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::output_write_failed, 0U, "could not write PCM data"});
    }
    return {};
}

template <typename ChannelReader>
[[nodiscard]] std::expected<WavConversionInfo, WavConversionError> convert_source(
    const SourceInfo& source,
    std::uint64_t byte_count_per_channel,
    ChannelReader&& read_channel,
    std::ostream& output,
    std::uint32_t requested_output_rate) {
    const auto output_rate = requested_output_rate == 0U
        ? choose_default_rate(source.sample_rate_hz)
        : requested_output_rate;
    if (output_rate == 0U || output_rate >= source.sample_rate_hz) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::invalid_output_rate, 0U, "PCM rate must be nonzero and below the DSD rate"});
    }
    if (source.channel_count == 0U
        || source.channel_count > std::numeric_limits<std::uint16_t>::max()
        || source.channel_count * kBytesPerPcmSample > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::unsupported_wav_layout, 0U, "WAV channel count exceeds PCM limits"});
    }

    const auto frames_result = output_frame_count(
        source.sample_count_per_channel,
        source.sample_rate_hz,
        output_rate);
    if (!frames_result) {
        return std::unexpected(frames_result.error());
    }
    const auto block_align = static_cast<std::uint64_t>(source.channel_count) * kBytesPerPcmSample;
    if (*frames_result > (std::numeric_limits<std::uint32_t>::max() - kWavRiffOverhead) / block_align) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::wav_size_limit_exceeded, 0U, "RIFF/WAVE output would exceed 4 GiB"});
    }
    const auto data_size = *frames_result * block_align;
    if (static_cast<std::uint64_t>(output_rate) * block_align > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::unsupported_wav_layout, 0U, "WAV byte rate exceeds its 32-bit field"});
    }

    std::vector<DsdToPcmConverter> converters;
    converters.reserve(source.channel_count);
    for (std::uint32_t channel = 0U; channel < source.channel_count; ++channel) {
        auto converter = DsdToPcmConverter::create(
            source.sample_rate_hz,
            output_rate,
            source.bit_order);
        if (!converter) {
            return std::unexpected(make_converter_error(converter.error().code));
        }
        converters.push_back(std::move(*converter));
    }

    const auto header = make_wav_header(
        source.channel_count,
        output_rate,
        static_cast<std::uint32_t>(data_size),
        source.channel_mask);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!output) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::output_write_failed, 0U, "could not write WAV header"});
    }

    std::vector<std::vector<std::byte>> channel_bytes(
        source.channel_count,
        std::vector<std::byte>(static_cast<std::size_t>(kBytesPerChannelChunk)));
    std::vector<std::vector<float>> channel_samples(source.channel_count);
    std::uint64_t byte_offset = 0U;
    while (byte_offset < byte_count_per_channel) {
        const auto bytes_this_chunk = static_cast<std::size_t>(std::min(
            kBytesPerChannelChunk,
            byte_count_per_channel - byte_offset));
        const auto chunk = byte_count_per_channel - byte_offset == bytes_this_chunk;
        const auto final_bit_count = chunk && source.sample_count_per_channel % 8U != 0U
            ? static_cast<std::uint8_t>(source.sample_count_per_channel % 8U)
            : static_cast<std::uint8_t>(8U);

        for (std::uint32_t channel = 0U; channel < source.channel_count; ++channel) {
            auto channel_buffer = std::span<std::byte>(channel_bytes[channel]).first(bytes_this_chunk);
            auto read_result = read_channel(channel, byte_offset, channel_buffer);
            if (!read_result) {
                return std::unexpected(read_result.error());
            }
            if (*read_result != bytes_this_chunk) {
                return std::unexpected(WavConversionError{
                    WavConversionErrorCode::channel_read_failed, 0U, "channel reader returned a short block"});
            }

            auto converted = converters[channel].process(channel_buffer, final_bit_count);
            if (!converted) {
                return std::unexpected(make_converter_error(converted.error().code));
            }
            channel_samples[channel] = std::move(*converted);
        }

        auto write_result = write_interleaved_samples(
            output,
            channel_samples,
            source.output_channel_order);
        if (!write_result) {
            return std::unexpected(write_result.error());
        }
        byte_offset += bytes_this_chunk;
    }

    for (std::uint32_t channel = 0U; channel < source.channel_count; ++channel) {
        auto tail = converters[channel].finish();
        if (!tail) {
            return std::unexpected(make_converter_error(tail.error().code));
        }
        channel_samples[channel] = std::move(*tail);
    }
    auto tail_write_result = write_interleaved_samples(
        output,
        channel_samples,
        source.output_channel_order);
    if (!tail_write_result) {
        return std::unexpected(tail_write_result.error());
    }
    if (converters.front().output_sample_count() != *frames_result) {
        return std::unexpected(WavConversionError{
            WavConversionErrorCode::inconsistent_channel_output,
            0U,
            "converter output count differs from the WAV header"});
    }
    return WavConversionInfo{
        source.sample_rate_hz,
        output_rate,
        source.channel_count,
        *frames_result,
        data_size,
    };
}

} // namespace

std::expected<WavConversionInfo, WavConversionError> convert_to_wav(
    std::span<const std::byte> input,
    std::ostream& output,
    std::uint32_t output_sample_rate_hz) {
    if (matches(input, 0U, {'D', 'S', 'D', ' '})) {
        auto parsed = formats::dsf::parse(input);
        if (!parsed) {
            return std::unexpected(WavConversionError{
                WavConversionErrorCode::invalid_dsf,
                parsed.error().offset,
                formats::dsf::message(parsed.error().code)});
        }
        const SourceInfo source{
            parsed->format.sample_rate_hz,
            parsed->format.channel_count,
            parsed->format.sample_count_per_channel,
            parsed->format.bit_order(),
            dsf_channel_mask(parsed->format.channel_type),
            {},
        };
        const auto bytes_per_channel = parsed->format.byte_count_per_channel();
        auto reader = [&parsed](
                          std::uint32_t channel,
                          std::uint64_t offset,
                          std::span<std::byte> destination)
            -> std::expected<std::size_t, WavConversionError> {
            auto read_result = parsed->read_channel_bytes(channel, offset, destination);
            if (!read_result) {
                return std::unexpected(WavConversionError{
                    WavConversionErrorCode::channel_read_failed,
                    read_result.error().offset,
                    formats::dsf::message(read_result.error().code)});
            }
            return *read_result;
        };
        return convert_source(
            source,
            bytes_per_channel,
            reader,
            output,
            output_sample_rate_hz);
    }

    if (matches(input, 0U, {'F', 'R', 'M', '8'})) {
        auto parsed = formats::dsdiff::parse(input);
        if (!parsed) {
            return std::unexpected(WavConversionError{
                WavConversionErrorCode::invalid_dsdiff,
                parsed.error().offset,
                formats::dsdiff::message(parsed.error().code)});
        }
        const auto dsdiff_source = dsdiff_source_info(*parsed);
        const auto bytes_per_channel = parsed->sample_count_per_channel / 8U;
        auto reader = [&parsed](
                          std::uint32_t channel,
                          std::uint64_t offset,
                          std::span<std::byte> destination)
            -> std::expected<std::size_t, WavConversionError> {
            auto read_result = parsed->read_channel_bytes(channel, offset, destination);
            if (!read_result) {
                return std::unexpected(WavConversionError{
                    WavConversionErrorCode::channel_read_failed,
                    read_result.error().offset,
                    formats::dsdiff::message(read_result.error().code)});
            }
            return *read_result;
        };
        return convert_source(
            dsdiff_source,
            bytes_per_channel,
            reader,
            output,
            output_sample_rate_hz);
    }

    return std::unexpected(WavConversionError{
        WavConversionErrorCode::unknown_input_format, 0U, "expected DSF or DSDIFF data"});
}

} // namespace ps5_sacd::audio
