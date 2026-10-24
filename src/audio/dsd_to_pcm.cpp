#include "ps5_sacd/audio/dsd_to_pcm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace ps5_sacd::audio {
namespace {

constexpr std::uint32_t kMinimumFilterTaps = 15U;
constexpr std::uint32_t kMaximumFilterTaps = 4095U;
constexpr std::uint64_t kMaximumCoefficientCount = 4'000'000U;
constexpr double kPi = 3.14159265358979323846264338327950288;

[[nodiscard]] double sinc(double value) noexcept {
    if (std::abs(value) < 1.0e-12) {
        return 1.0;
    }
    const auto angle = kPi * value;
    return std::sin(angle) / angle;
}

[[nodiscard]] std::uint32_t default_filter_tap_count(
    std::uint32_t dsd_rate,
    std::uint32_t pcm_rate) noexcept {
    const auto ratio = static_cast<double>(dsd_rate) / static_cast<double>(pcm_rate);
    const auto requested = std::ceil(ratio * 16.0) + 1.0;
    auto count = requested >= static_cast<double>(kMaximumFilterTaps)
        ? kMaximumFilterTaps
        : static_cast<std::uint32_t>(requested);
    count = std::max(count, kMinimumFilterTaps);
    if ((count & 1U) == 0U) {
        --count;
    }
    return count;
}

[[nodiscard]] std::vector<float> make_phase_coefficients(
    std::uint32_t tap_count,
    std::uint32_t phase_remainder,
    std::uint32_t pcm_rate,
    std::uint32_t dsd_rate) {
    const auto center = static_cast<double>(tap_count - 1U) / 2.0;
    const auto fractional_delay = phase_remainder == 0U
        ? 0.0
        : 1.0 - static_cast<double>(phase_remainder) / static_cast<double>(pcm_rate);
    const auto cutoff = 0.45 * static_cast<double>(pcm_rate) / static_cast<double>(dsd_rate);
    std::vector<float> coefficients(tap_count);
    double coefficient_sum = 0.0;

    for (std::uint32_t lag = 0U; lag < tap_count; ++lag) {
        const auto position = static_cast<double>(lag);
        const auto distance = position - fractional_delay - center;
        const auto window_position = position / static_cast<double>(tap_count - 1U);
        const auto window = 0.42
            - 0.5 * std::cos(2.0 * kPi * window_position)
            + 0.08 * std::cos(4.0 * kPi * window_position);
        const auto coefficient = 2.0 * cutoff * sinc(2.0 * cutoff * distance) * window;
        coefficients[lag] = static_cast<float>(coefficient);
        coefficient_sum += coefficient;
    }

    if (std::abs(coefficient_sum) > 1.0e-15) {
        for (auto& coefficient : coefficients) {
            coefficient = static_cast<float>(static_cast<double>(coefficient) / coefficient_sum);
        }
    }
    return coefficients;
}

} // namespace

DsdToPcmConverter::DsdToPcmConverter(
    std::uint32_t dsd_sample_rate_hz,
    std::uint32_t pcm_sample_rate_hz,
    formats::DsdBitOrder bit_order,
    std::vector<std::vector<float>> filter_coefficients)
    : dsd_sample_rate_hz_(dsd_sample_rate_hz),
      pcm_sample_rate_hz_(pcm_sample_rate_hz),
      bit_order_(bit_order),
      filter_coefficients_(std::move(filter_coefficients)),
      history_(filter_coefficients_.front().size(), 0.0F),
      next_output_input_index_(filter_coefficients_.front().size() / 2U) {}

std::expected<DsdToPcmConverter, ConversionError> DsdToPcmConverter::create(
    std::uint32_t dsd_sample_rate_hz,
    std::uint32_t pcm_sample_rate_hz,
    formats::DsdBitOrder bit_order,
    std::uint32_t filter_tap_count) {
    if (dsd_sample_rate_hz == 0U) {
        return std::unexpected(ConversionError{ConversionErrorCode::invalid_dsd_sample_rate});
    }
    if (pcm_sample_rate_hz == 0U) {
        return std::unexpected(ConversionError{ConversionErrorCode::invalid_pcm_sample_rate});
    }
    if (pcm_sample_rate_hz >= dsd_sample_rate_hz) {
        return std::unexpected(ConversionError{
            ConversionErrorCode::pcm_rate_must_be_lower_than_dsd_rate});
    }

    if (filter_tap_count == 0U) {
        filter_tap_count = default_filter_tap_count(dsd_sample_rate_hz, pcm_sample_rate_hz);
    }
    if (filter_tap_count < kMinimumFilterTaps || filter_tap_count > kMaximumFilterTaps
        || (filter_tap_count & 1U) == 0U) {
        return std::unexpected(ConversionError{ConversionErrorCode::invalid_filter_length});
    }

    const auto phase_count = pcm_sample_rate_hz
        / std::gcd(dsd_sample_rate_hz, pcm_sample_rate_hz);
    if (static_cast<std::uint64_t>(phase_count) * filter_tap_count > kMaximumCoefficientCount) {
        return std::unexpected(ConversionError{ConversionErrorCode::unsupported_rate_pair});
    }

    std::vector<std::vector<float>> coefficients;
    coefficients.reserve(phase_count);
    std::uint64_t phase_remainder = 0U;
    for (std::uint32_t phase = 0U; phase < phase_count; ++phase) {
        coefficients.push_back(make_phase_coefficients(
            filter_tap_count,
            static_cast<std::uint32_t>(phase_remainder),
            pcm_sample_rate_hz,
            dsd_sample_rate_hz));
        phase_remainder = (phase_remainder + dsd_sample_rate_hz) % pcm_sample_rate_hz;
    }

    return DsdToPcmConverter(
        dsd_sample_rate_hz,
        pcm_sample_rate_hz,
        bit_order,
        std::move(coefficients));
}

std::expected<std::vector<float>, ConversionError> DsdToPcmConverter::process(
    std::span<const std::byte> dsd_bytes) {
    if (finished_) {
        return std::unexpected(ConversionError{ConversionErrorCode::converter_already_finished});
    }
    if (dsd_bytes.size() > (std::numeric_limits<std::uint64_t>::max() - input_sample_count_) / 8U
        || dsd_bytes.size() > (std::numeric_limits<std::uint64_t>::max() - processed_sample_count_) / 8U) {
        return std::unexpected(ConversionError{ConversionErrorCode::sample_count_overflow});
    }

    std::vector<float> output;
    for (const auto byte : dsd_bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const auto shift = bit_order_ == formats::DsdBitOrder::most_significant_bit_first
                ? 7U - bit
                : bit;
            const auto is_one = ((value >> shift) & 1U) != 0U;
            push_sample(is_one ? 1.0F : -1.0F, output, true);
        }
    }
    return output;
}

std::expected<std::vector<float>, ConversionError> DsdToPcmConverter::finish() {
    if (finished_) {
        return std::unexpected(ConversionError{ConversionErrorCode::converter_already_finished});
    }
    const auto target_count_result = expected_output_count();
    if (!target_count_result) {
        return std::unexpected(target_count_result.error());
    }
    const auto target_count = *target_count_result;
    std::vector<float> output;
    while (output_sample_count_ < target_count) {
        if (processed_sample_count_ == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(ConversionError{ConversionErrorCode::sample_count_overflow});
        }
        push_sample(0.0F, output, false);
    }
    finished_ = true;
    return output;
}

void DsdToPcmConverter::push_sample(
    float sample,
    std::vector<float>& output,
    bool is_dsd_input) {
    history_[static_cast<std::size_t>(processed_sample_count_ % history_.size())] = sample;
    ++processed_sample_count_;
    if (is_dsd_input) {
        ++input_sample_count_;
    }
    append_ready_samples(output);
}

void DsdToPcmConverter::append_ready_samples(std::vector<float>& output) {
    while (next_output_input_index_ < processed_sample_count_) {
        output.push_back(filtered_sample(next_output_input_index_));
        ++output_sample_count_;
        advance_output_position();
    }
}

float DsdToPcmConverter::filtered_sample(std::uint64_t input_index) const noexcept {
    const auto& coefficients = filter_coefficients_[static_cast<std::size_t>(
        output_sample_count_ % filter_coefficients_.size())];
    const auto newest_index = processed_sample_count_ - 1U;
    double sum = 0.0;
    for (std::size_t lag = 0U; lag < coefficients.size(); ++lag) {
        if (static_cast<std::uint64_t>(lag) > input_index) {
            continue;
        }
        const auto sample_index = input_index - static_cast<std::uint64_t>(lag);
        if (sample_index > newest_index || newest_index - sample_index >= history_.size()) {
            continue;
        }
        const auto ring_index = static_cast<std::size_t>(sample_index % history_.size());
        sum += static_cast<double>(history_[ring_index]) * coefficients[lag];
    }
    return static_cast<float>(sum);
}

void DsdToPcmConverter::advance_output_position() noexcept {
    const auto rate_remainder = static_cast<std::uint64_t>(dsd_sample_rate_hz_)
        % pcm_sample_rate_hz_;
    const auto remainder_sum = phase_remainder_ + rate_remainder;
    output_position_floor_ += static_cast<std::uint64_t>(dsd_sample_rate_hz_)
        / pcm_sample_rate_hz_
        + remainder_sum / pcm_sample_rate_hz_;
    phase_remainder_ = remainder_sum % pcm_sample_rate_hz_;

    const auto center = static_cast<std::uint64_t>(filter_coefficients_.front().size() / 2U);
    next_output_input_index_ = center + output_position_floor_ + (phase_remainder_ == 0U ? 0U : 1U);
}

std::expected<std::uint64_t, ConversionError> DsdToPcmConverter::expected_output_count() const noexcept {
    const auto whole_seconds = input_sample_count_ / dsd_sample_rate_hz_;
    const auto remaining_samples = input_sample_count_ % dsd_sample_rate_hz_;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / pcm_sample_rate_hz_) {
        return std::unexpected(ConversionError{ConversionErrorCode::sample_count_overflow});
    }
    const auto whole_output_samples = whole_seconds * pcm_sample_rate_hz_;
    const auto remaining_product = remaining_samples * static_cast<std::uint64_t>(pcm_sample_rate_hz_);
    const auto remaining_output_samples = remaining_product / dsd_sample_rate_hz_
        + (remaining_product % dsd_sample_rate_hz_ == 0U ? 0U : 1U);
    if (remaining_output_samples > std::numeric_limits<std::uint64_t>::max() - whole_output_samples) {
        return std::unexpected(ConversionError{ConversionErrorCode::sample_count_overflow});
    }
    return whole_output_samples + remaining_output_samples;
}

} // namespace ps5_sacd::audio
