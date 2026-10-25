#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <span>
#include <string_view>

namespace ps5_sacd::audio {

enum class WavConversionErrorCode {
    unknown_input_format,
    invalid_dsf,
    invalid_dsdiff,
    invalid_output_rate,
    unsupported_wav_layout,
    wav_size_limit_exceeded,
    channel_read_failed,
    inconsistent_channel_output,
    dsd_conversion_failed,
    output_write_failed,
};

struct WavConversionError {
    WavConversionErrorCode code;
    std::size_t input_offset{};
    std::string_view detail;
};

struct WavConversionInfo {
    std::uint32_t input_sample_rate_hz{};
    std::uint32_t output_sample_rate_hz{};
    std::uint32_t channel_count{};
    std::uint64_t output_frame_count{};
    std::uint64_t output_data_size_bytes{};
};

// Converts a complete DSF or uncompressed DSDIFF buffer to 24-bit PCM WAV.
// The input remains a non-owning view and must outlive this call. A zero output
// rate selects 44.1 kHz or 48 kHz to match the source DSD rate family.
[[nodiscard]] std::expected<WavConversionInfo, WavConversionError> convert_to_wav(
    std::span<const std::byte> input,
    std::ostream& output,
    std::uint32_t output_sample_rate_hz = 0U);

} // namespace ps5_sacd::audio
