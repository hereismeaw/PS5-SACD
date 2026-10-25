#include "ps5_sacd/audio/dsd_to_wav.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

namespace audio = ps5_sacd::audio;

namespace {

using Bytes = std::vector<std::byte>;

constexpr std::uint32_t kDsdRate = 2'822'400U;

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

void append_id(Bytes& bytes, std::string_view id) {
    for (const auto character : id) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

void append_u16_be(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32_be(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64_be(Bytes& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u32_le(Bytes& bytes, std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64_le(Bytes& bytes, std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void store_u64_le(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void append_chunk(Bytes& bytes, std::string_view id, const Bytes& payload) {
    append_id(bytes, id);
    append_u64_be(bytes, static_cast<std::uint64_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) {
        bytes.push_back(std::byte{0});
    }
}

Bytes make_dsdiff_fixture(bool dst_compression = false, bool reversed_stereo = false) {
    Bytes form;
    append_id(form, "DSD ");
    Bytes version;
    append_u32_be(version, 0x01050000U);
    append_chunk(form, "FVER", version);

    Bytes properties;
    append_id(properties, "SND ");
    Bytes sample_rate;
    append_u32_be(sample_rate, kDsdRate);
    append_chunk(properties, "FS  ", sample_rate);
    Bytes channels;
    append_u16_be(channels, 2U);
    append_id(channels, reversed_stereo ? "SRGT" : "SLFT");
    append_id(channels, reversed_stereo ? "SLFT" : "SRGT");
    append_chunk(properties, "CHNL", channels);
    Bytes compression;
    append_id(compression, dst_compression ? "DST " : "DSD ");
    constexpr std::string_view name = "uncompressed test fixture";
    compression.push_back(static_cast<std::byte>(name.size()));
    for (const auto character : name) {
        compression.push_back(static_cast<std::byte>(character));
    }
    append_chunk(properties, "CMPR", compression);
    append_chunk(form, "PROP", properties);

    Bytes audio;
    for (std::size_t index = 0U; index < 512U; ++index) {
        audio.push_back(reversed_stereo ? std::byte{0x00} : std::byte{0xff});
        audio.push_back(reversed_stereo ? std::byte{0xff} : std::byte{0x00});
    }
    append_chunk(form, dst_compression ? "DST " : "DSD ", audio);

    Bytes file;
    append_id(file, "FRM8");
    append_u64_be(file, static_cast<std::uint64_t>(form.size()));
    file.insert(file.end(), form.begin(), form.end());
    return file;
}

Bytes make_six_channel_dsdiff_fixture(bool include_lsco = false, bool reversed_order = false) {
    Bytes form;
    append_id(form, "DSD ");
    Bytes version;
    append_u32_be(version, 0x01050000U);
    append_chunk(form, "FVER", version);

    Bytes properties;
    append_id(properties, "SND ");
    Bytes sample_rate;
    append_u32_be(sample_rate, kDsdRate);
    append_chunk(properties, "FS  ", sample_rate);
    Bytes channels;
    append_u16_be(channels, 6U);
    constexpr std::array<std::string_view, 6> channel_ids{
        "MLFT", "MRGT", "C   ", "LFE ", "LS  ", "RS  "};
    for (std::size_t index = 0U; index < channel_ids.size(); ++index) {
        const auto speaker_index = reversed_order ? channel_ids.size() - index - 1U : index;
        append_id(channels, channel_ids[speaker_index]);
    }
    append_chunk(properties, "CHNL", channels);
    Bytes compression;
    append_id(compression, "DSD ");
    constexpr std::string_view name = "uncompressed test fixture";
    compression.push_back(static_cast<std::byte>(name.size()));
    for (const auto character : name) {
        compression.push_back(static_cast<std::byte>(character));
    }
    append_chunk(properties, "CMPR", compression);
    if (include_lsco) {
        Bytes loudspeaker_configuration;
        append_u16_be(loudspeaker_configuration, 4U);
        append_chunk(properties, "LSCO", loudspeaker_configuration);
    }
    append_chunk(form, "PROP", properties);

    Bytes audio;
    for (std::size_t frame = 0U; frame < 2U; ++frame) {
        for (std::size_t index = 0U; index < channel_ids.size(); ++index) {
            const auto speaker_index = reversed_order ? channel_ids.size() - index - 1U : index;
            audio.push_back((speaker_index & 1U) == 0U ? std::byte{0xff} : std::byte{0x00});
        }
    }
    append_chunk(form, "DSD ", audio);

    Bytes file;
    append_id(file, "FRM8");
    append_u64_be(file, static_cast<std::uint64_t>(form.size()));
    file.insert(file.end(), form.begin(), form.end());
    return file;
}

Bytes make_dsf_fixture() {
    constexpr std::uint64_t sample_count = 4097U;
    constexpr std::uint32_t block_size = 512U;
    Bytes file;
    append_id(file, "DSD ");
    append_u64_le(file, 28U);
    append_u64_le(file, 0U);
    append_u64_le(file, 0U);

    append_id(file, "fmt ");
    append_u64_le(file, 52U);
    append_u32_le(file, 1U);
    append_u32_le(file, 0U);
    append_u32_le(file, 2U);
    append_u32_le(file, 2U);
    append_u32_le(file, kDsdRate);
    append_u32_le(file, 1U);
    append_u64_le(file, sample_count);
    append_u32_le(file, block_size);
    append_u32_le(file, 0U);

    Bytes audio;
    audio.insert(audio.end(), block_size, std::byte{0xff});
    audio.insert(audio.end(), block_size, std::byte{0x00});
    audio.push_back(std::byte{0x01});
    audio.insert(audio.end(), block_size - 1U, std::byte{0x00});
    audio.push_back(std::byte{0xfe});
    audio.insert(audio.end(), block_size - 1U, std::byte{0xff});
    append_id(file, "data");
    append_u64_le(file, static_cast<std::uint64_t>(audio.size() + 12U));
    file.insert(file.end(), audio.begin(), audio.end());
    store_u64_le(file, 12U, static_cast<std::uint64_t>(file.size()));
    return file;
}

Bytes make_dsf_layout_fixture(std::uint32_t channel_type, std::uint32_t channel_count) {
    Bytes file;
    append_id(file, "DSD ");
    append_u64_le(file, 28U);
    append_u64_le(file, 0U);
    append_u64_le(file, 0U);

    append_id(file, "fmt ");
    append_u64_le(file, 52U);
    append_u32_le(file, 1U);
    append_u32_le(file, 0U);
    append_u32_le(file, channel_type);
    append_u32_le(file, channel_count);
    append_u32_le(file, kDsdRate);
    append_u32_le(file, 1U);
    append_u64_le(file, 8U);
    append_u32_le(file, 1U);
    append_u32_le(file, 0U);

    Bytes audio;
    for (std::uint32_t channel = 0U; channel < channel_count; ++channel) {
        audio.push_back((channel & 1U) == 0U ? std::byte{0xff} : std::byte{0x00});
    }
    append_id(file, "data");
    append_u64_le(file, static_cast<std::uint64_t>(audio.size() + 12U));
    file.insert(file.end(), audio.begin(), audio.end());
    store_u64_le(file, 12U, static_cast<std::uint64_t>(file.size()));
    return file;
}

std::uint16_t read_u16_le(const std::string& bytes, std::size_t offset) {
    const auto low = static_cast<std::uint8_t>(bytes[offset]);
    const auto high = static_cast<std::uint8_t>(bytes[offset + 1U]);
    return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8U));
}

std::uint32_t read_u32_le(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (unsigned int index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + index]))
            << (index * 8U);
    }
    return value;
}

std::int32_t read_pcm24(const std::string& bytes, std::size_t offset) {
    const auto raw = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset]))
        | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1U])) << 8U)
        | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2U])) << 16U);
    if ((raw & 0x00800000U) != 0U) {
        return static_cast<std::int32_t>(raw | 0xff000000U);
    }
    return static_cast<std::int32_t>(raw);
}

bool test_dsdiff_to_stereo_pcm_wav() {
    const auto input = make_dsdiff_fixture();
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto converted = audio::convert_to_wav(input, output);
    if (!check(converted.has_value(), "uncompressed DSDIFF converts to WAV")) {
        if (!converted) {
            std::cerr << "  detail: " << converted.error().detail << '\n';
        }
        return false;
    }
    const auto wav = output.str();
    if (!check(converted->input_sample_rate_hz == kDsdRate
                   && converted->output_sample_rate_hz == 44'100U,
               "default PCM rate follows the DSD rate family")) {
        return false;
    }
    if (!check(converted->channel_count == 2U && converted->output_frame_count == 64U,
               "stereo channel and frame counts are reported")) {
        return false;
    }
    if (!check(wav.size() == 68U + 64U * 2U * 3U, "WAV contains the declared PCM payload")) {
        return false;
    }
    if (!check(wav.substr(0U, 4U) == "RIFF" && wav.substr(8U, 4U) == "WAVE"
                   && wav.substr(12U, 4U) == "fmt " && wav.substr(60U, 4U) == "data",
               "WAV chunk identifiers are written")) {
        return false;
    }
    if (!check(read_u32_le(wav, 4U) == wav.size() - 8U
                   && read_u16_le(wav, 20U) == 0xfffeU
                   && read_u16_le(wav, 22U) == 2U
                   && read_u32_le(wav, 24U) == 44'100U
                   && read_u16_le(wav, 34U) == 24U
                   && read_u32_le(wav, 40U) == 3U
                   && read_u32_le(wav, 64U) == 64U * 2U * 3U,
               "WAVE_FORMAT_EXTENSIBLE fields match the PCM stream")) {
        return false;
    }
    const auto frame_offset = 68U + 32U * 6U;
    return check(read_pcm24(wav, frame_offset) > 8'000'000,
                 "left channel DSD ones become positive PCM")
        && check(read_pcm24(wav, frame_offset + 3U) < -8'000'000,
                 "right channel DSD zeroes become negative PCM");
}

bool test_dsdiff_stereo_is_reordered_to_speaker_order() {
    const auto input = make_dsdiff_fixture(false, true);
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto converted = audio::convert_to_wav(input, output);
    if (!check(converted.has_value(), "reversed DSDIFF stereo converts")) {
        return false;
    }
    const auto wav = output.str();
    const auto frame_offset = 68U + 32U * 6U;
    return check(read_u32_le(wav, 40U) == 3U,
                 "recognized DSDIFF stereo receives a left/right WAV speaker mask")
        && check(read_pcm24(wav, frame_offset) > 8'000'000,
                 "WAV left channel follows the DSDIFF channel identifier")
        && check(read_pcm24(wav, frame_offset + 3U) < -8'000'000,
                 "WAV right channel follows the DSDIFF channel identifier");
}

bool test_multichannel_wav_preserves_source_order_without_guessing_speakers() {
    const auto input = make_six_channel_dsdiff_fixture();
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto converted = audio::convert_to_wav(input, output);
    if (!check(converted.has_value(), "six-channel DSDIFF converts")) {
        return false;
    }
    const auto wav = output.str();
    return check(converted->channel_count == 6U && converted->output_frame_count == 1U,
                 "multichannel frame layout is retained")
        && check(read_u16_le(wav, 22U) == 6U && read_u32_le(wav, 40U) == 0U,
                 "unmapped multichannel output has no guessed speaker mask")
        && check(read_u32_le(wav, 64U) == 18U && wav.size() == 86U,
                 "six-channel PCM payload matches its WAV header");
}

bool test_dsdiff_itu_51_uses_lsco_and_channel_ids() {
    const auto input = make_six_channel_dsdiff_fixture(true, true);
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto converted = audio::convert_to_wav(input, output);
    if (!check(converted.has_value(), "DSDIFF 5.1 with LSCO converts")) {
        return false;
    }
    const auto wav = output.str();
    const auto frame_offset = 68U;
    if (!check(read_u32_le(wav, 40U) == 0x3fU,
               "LSCO 5.1 maps to the standard WAVE speaker mask")) {
        return false;
    }

    constexpr std::array<bool, 6> expected_positive{true, false, true, false, true, false};
    for (std::size_t channel = 0U; channel < expected_positive.size(); ++channel) {
        const auto sample = read_pcm24(wav, frame_offset + channel * 3U);
        if (!check(expected_positive[channel] ? sample > 0 : sample < 0,
                   "5.1 samples are ordered by their speaker identifiers")) {
            return false;
        }
    }
    return true;
}

bool test_dsf_partial_sample_and_custom_rate() {
    const auto input = make_dsf_fixture();
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto converted = audio::convert_to_wav(input, output, 48'000U);
    if (!check(converted.has_value(), "DSF with a partial final byte converts")) {
        if (!converted) {
            std::cerr << "  detail: " << converted.error().detail << '\n';
        }
        return false;
    }
    const auto wav = output.str();
    return check(converted->output_sample_rate_hz == 48'000U,
                 "explicit PCM rate is retained")
        && check(converted->output_frame_count == 70U,
                 "DSF sample count excludes unused final-byte bits")
        && check(converted->output_data_size_bytes == 70U * 2U * 3U,
                 "DSF output byte count is correct")
        && check(wav.size() == 68U + 70U * 2U * 3U,
                 "DSF WAV data size matches its header")
        && check(read_u32_le(wav, 24U) == 48'000U,
                 "custom rate is stored in the WAV header");
}

bool test_dsf_channel_types_get_standard_wav_layouts() {
    constexpr std::array<std::uint32_t, 7> channel_counts{1U, 2U, 3U, 4U, 4U, 5U, 6U};
    constexpr std::array<std::uint32_t, 7> channel_masks{
        0x04U, 0x03U, 0x07U, 0x33U, 0x0fU, 0x37U, 0x3fU};

    for (std::uint32_t channel_type = 1U; channel_type <= channel_counts.size(); ++channel_type) {
        const auto input = make_dsf_layout_fixture(channel_type, channel_counts[channel_type - 1U]);
        std::ostringstream output(std::ios::out | std::ios::binary);
        const auto converted = audio::convert_to_wav(input, output);
        if (!check(converted.has_value(), "DSF channel type converts to WAV")) {
            return false;
        }
        const auto wav = output.str();
        if (!check(read_u32_le(wav, 40U) == channel_masks[channel_type - 1U],
                   "DSF channel type receives its documented speaker mask")) {
            return false;
        }
        if (!check(read_u16_le(wav, 22U) == channel_counts[channel_type - 1U]
                       && converted->output_frame_count == 1U,
                   "DSF channel type retains its sample layout")) {
            return false;
        }
    }
    return true;
}

bool test_unsupported_and_invalid_inputs() {
    const auto dst_input = make_dsdiff_fixture(true);
    std::ostringstream dst_output(std::ios::out | std::ios::binary);
    const auto dst_result = audio::convert_to_wav(dst_input, dst_output);
    if (!check(!dst_result && dst_result.error().code == audio::WavConversionErrorCode::invalid_dsdiff,
               "DST DSDIFF is reported as unsupported input")) {
        return false;
    }

    const Bytes unknown{std::byte{0}, std::byte{1}, std::byte{2}};
    std::ostringstream unknown_output(std::ios::out | std::ios::binary);
    const auto unknown_result = audio::convert_to_wav(unknown, unknown_output);
    if (!check(!unknown_result
                   && unknown_result.error().code == audio::WavConversionErrorCode::unknown_input_format,
               "unknown input container is rejected")) {
        return false;
    }

    const auto dsdiff = make_dsdiff_fixture();
    std::ostringstream invalid_rate_output(std::ios::out | std::ios::binary);
    const auto invalid_rate = audio::convert_to_wav(dsdiff, invalid_rate_output, kDsdRate);
    return check(!invalid_rate
                     && invalid_rate.error().code == audio::WavConversionErrorCode::invalid_output_rate,
                 "PCM output rate must be below the DSD rate");
}

} // namespace

int main() {
    const bool passed = test_dsdiff_to_stereo_pcm_wav()
        && test_dsdiff_stereo_is_reordered_to_speaker_order()
        && test_multichannel_wav_preserves_source_order_without_guessing_speakers()
        && test_dsdiff_itu_51_uses_lsco_and_channel_ids()
        && test_dsf_partial_sample_and_custom_rate()
        && test_dsf_channel_types_get_standard_wav_layouts()
        && test_unsupported_and_invalid_inputs();
    if (!passed) {
        return 1;
    }
    std::cout << "All DSD-to-WAV conversion tests passed.\n";
    return 0;
}
