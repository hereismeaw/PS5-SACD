#pragma once

#include "ps5_sacd/formats/dsd_bit_order.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ps5_sacd::audio {

enum class ConversionErrorCode {
    invalid_dsd_sample_rate,
    invalid_pcm_sample_rate,
    pcm_rate_must_be_lower_than_dsd_rate,
    unsupported_rate_pair,
    invalid_filter_length,
    sample_count_overflow,
    converter_already_finished,
};

struct ConversionError {
    ConversionErrorCode code;
};

// A stateful mono DSD decimator. Each input bit is mapped to -1/+1, filtered
// by a windowed-sinc FIR, then sampled at the requested PCM rate. Callers can
// create one converter per channel and feed it channel bytes in file bit order.
class DsdToPcmConverter {
public:
    [[nodiscard]] static std::expected<DsdToPcmConverter, ConversionError> create(
        std::uint32_t dsd_sample_rate_hz,
        std::uint32_t pcm_sample_rate_hz,
        formats::DsdBitOrder bit_order,
        std::uint32_t filter_tap_count = 0U);

    [[nodiscard]] std::expected<std::vector<float>, ConversionError> process(
        std::span<const std::byte> dsd_bytes);

    // Flushes the FIR delay line with zeroes and returns the remaining PCM
    // frames. The total output count is ceil(input_bits * pcm_rate / dsd_rate).
    [[nodiscard]] std::expected<std::vector<float>, ConversionError> finish();

    [[nodiscard]] std::uint64_t input_sample_count() const noexcept {
        return input_sample_count_;
    }

    [[nodiscard]] std::uint64_t output_sample_count() const noexcept {
        return output_sample_count_;
    }

    [[nodiscard]] std::uint32_t filter_tap_count() const noexcept {
        return static_cast<std::uint32_t>(filter_coefficients_.front().size());
    }

private:
    DsdToPcmConverter(
        std::uint32_t dsd_sample_rate_hz,
        std::uint32_t pcm_sample_rate_hz,
        formats::DsdBitOrder bit_order,
        std::vector<std::vector<float>> filter_coefficients);

    void push_sample(float sample, std::vector<float>& output, bool is_dsd_input);
    void append_ready_samples(std::vector<float>& output);
    [[nodiscard]] float filtered_sample(std::uint64_t input_index) const noexcept;
    void advance_output_position() noexcept;
    [[nodiscard]] std::expected<std::uint64_t, ConversionError> expected_output_count() const noexcept;

    std::uint32_t dsd_sample_rate_hz_;
    std::uint32_t pcm_sample_rate_hz_;
    formats::DsdBitOrder bit_order_;
    std::vector<std::vector<float>> filter_coefficients_;
    std::vector<float> history_;
    std::uint64_t input_sample_count_{};
    std::uint64_t processed_sample_count_{};
    std::uint64_t output_sample_count_{};
    std::uint64_t output_position_floor_{};
    std::uint64_t next_output_input_index_{};
    std::uint64_t phase_remainder_{};
    bool finished_{};
};

} // namespace ps5_sacd::audio
