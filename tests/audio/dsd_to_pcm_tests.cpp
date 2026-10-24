#include "ps5_sacd/audio/dsd_to_pcm.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace audio = ps5_sacd::audio;
namespace formats = ps5_sacd::formats;

namespace {

using Bytes = std::vector<std::byte>;
using Samples = std::vector<float>;

constexpr std::uint32_t kDsdRate = 2'822'400U;

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

void append(Samples& destination, Samples source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

bool test_rate_and_filter_validation() {
    const auto no_dsd_rate = audio::DsdToPcmConverter::create(
        0U, 44'100U, formats::DsdBitOrder::most_significant_bit_first);
    if (!check(!no_dsd_rate
                   && no_dsd_rate.error().code == audio::ConversionErrorCode::invalid_dsd_sample_rate,
               "zero DSD rate is rejected")) {
        return false;
    }

    const auto upsample = audio::DsdToPcmConverter::create(
        44'100U, 48'000U, formats::DsdBitOrder::most_significant_bit_first);
    if (!check(!upsample
                   && upsample.error().code
                       == audio::ConversionErrorCode::pcm_rate_must_be_lower_than_dsd_rate,
               "PCM rate must be below the DSD bit rate")) {
        return false;
    }

    const auto even_taps = audio::DsdToPcmConverter::create(
        kDsdRate, 44'100U, formats::DsdBitOrder::most_significant_bit_first, 64U);
    return check(!even_taps
                     && even_taps.error().code == audio::ConversionErrorCode::invalid_filter_length,
                 "FIR length must be odd and within supported bounds");
}

bool test_bit_order_and_output_count() {
    Bytes msb_bytes(512U, std::byte{0x80});
    Bytes lsb_bytes(512U, std::byte{0x01});
    auto msb = audio::DsdToPcmConverter::create(
        kDsdRate, 44'100U, formats::DsdBitOrder::most_significant_bit_first);
    auto lsb = audio::DsdToPcmConverter::create(
        kDsdRate, 44'100U, formats::DsdBitOrder::least_significant_bit_first);
    if (!check(msb.has_value() && lsb.has_value(), "both bit orders create converters")) {
        return false;
    }

    auto msb_output = msb->process(msb_bytes);
    auto lsb_output = lsb->process(lsb_bytes);
    if (!check(msb_output.has_value() && lsb_output.has_value(), "bit streams process")) {
        return false;
    }
    auto msb_tail = msb->finish();
    auto lsb_tail = lsb->finish();
    if (!check(msb_tail.has_value() && lsb_tail.has_value(), "FIR tails flush")) {
        return false;
    }
    append(*msb_output, std::move(*msb_tail));
    append(*lsb_output, std::move(*lsb_tail));

    if (!check(msb_output->size() == 64U, "DSD duration yields the expected PCM frame count")) {
        return false;
    }
    if (!check(msb->input_sample_count() == 4096U && msb->output_sample_count() == 64U,
               "converter reports consumed and emitted sample counts")) {
        return false;
    }
    if (!check(msb_output->size() == lsb_output->size(), "opposite bit orders retain frame count")) {
        return false;
    }
    for (std::size_t index = 0U; index < msb_output->size(); ++index) {
        if (std::abs((*msb_output)[index] - (*lsb_output)[index]) > 1.0e-6F) {
            return check(false, "bit order selects matching DSD samples");
        }
    }
    const auto processed_after_finish = msb->process(msb_bytes);
    return check(!processed_after_finish
                     && processed_after_finish.error().code
                         == audio::ConversionErrorCode::converter_already_finished,
                 "processing after finish is rejected");
}

bool test_chunked_streaming_matches_single_block() {
    Bytes dsd_bytes(4096U);
    for (std::size_t index = 0U; index < dsd_bytes.size(); ++index) {
        dsd_bytes[index] = static_cast<std::byte>((index * 73U + 19U) & 0xffU);
    }

    auto single = audio::DsdToPcmConverter::create(
        kDsdRate, 48'000U, formats::DsdBitOrder::most_significant_bit_first);
    auto chunked = audio::DsdToPcmConverter::create(
        kDsdRate, 48'000U, formats::DsdBitOrder::most_significant_bit_first);
    if (!check(single.has_value() && chunked.has_value(), "48 kHz rational resamplers create")) {
        return false;
    }

    auto one_block = single->process(dsd_bytes);
    if (!check(one_block.has_value(), "single block is processed")) {
        return false;
    }
    auto one_tail = single->finish();
    if (!check(one_tail.has_value(), "single block FIR tail flushes")) {
        return false;
    }
    append(*one_block, std::move(*one_tail));

    Samples many_blocks;
    constexpr std::size_t chunk_size = 137U;
    for (std::size_t offset = 0U; offset < dsd_bytes.size(); offset += chunk_size) {
        const auto length = std::min(chunk_size, dsd_bytes.size() - offset);
        auto chunk = chunked->process(std::span<const std::byte>(dsd_bytes).subspan(offset, length));
        if (!check(chunk.has_value(), "stream chunk processes")) {
            return false;
        }
        append(many_blocks, std::move(*chunk));
    }
    auto many_tail = chunked->finish();
    if (!check(many_tail.has_value(), "chunked FIR tail flushes")) {
        return false;
    }
    append(many_blocks, std::move(*many_tail));

    if (!check(one_block->size() == many_blocks.size(), "chunk boundaries do not change frame count")) {
        return false;
    }
    for (std::size_t index = 0U; index < one_block->size(); ++index) {
        if (std::abs((*one_block)[index] - many_blocks[index]) > 1.0e-7F) {
            return check(false, "chunk boundaries do not change PCM samples");
        }
    }
    return true;
}

bool test_low_pass_rejects_alternating_dsd() {
    Bytes constant_ones(2048U, std::byte{0xff});
    Bytes alternating(2048U, std::byte{0xaa});
    auto dc_converter = audio::DsdToPcmConverter::create(
        kDsdRate, 44'100U, formats::DsdBitOrder::most_significant_bit_first);
    auto noise_converter = audio::DsdToPcmConverter::create(
        kDsdRate, 44'100U, formats::DsdBitOrder::most_significant_bit_first);
    if (!check(dc_converter.has_value() && noise_converter.has_value(), "filter converters create")) {
        return false;
    }

    auto dc = dc_converter->process(constant_ones);
    auto noise = noise_converter->process(alternating);
    if (!check(dc.has_value() && noise.has_value(), "test signals process")) {
        return false;
    }
    auto dc_tail = dc_converter->finish();
    auto noise_tail = noise_converter->finish();
    if (!check(dc_tail.has_value() && noise_tail.has_value(), "test signals flush")) {
        return false;
    }
    append(*dc, std::move(*dc_tail));
    append(*noise, std::move(*noise_tail));

    double dc_sum = 0.0;
    double alternating_magnitude = 0.0;
    std::size_t count = 0U;
    for (std::size_t index = 30U; index + 30U < dc->size(); ++index) {
        dc_sum += (*dc)[index];
        alternating_magnitude += std::abs((*noise)[index]);
        ++count;
    }
    if (!check(count > 0U, "steady-state section exists")) {
        return false;
    }
    const auto dc_mean = dc_sum / static_cast<double>(count);
    const auto alternating_mean = alternating_magnitude / static_cast<double>(count);
    return check(dc_mean > 0.95, "low-pass preserves a constant DSD signal")
        && check(alternating_mean < 0.05, "low-pass attenuates alternating high-frequency DSD");
}

} // namespace

int main() {
    const bool passed = test_rate_and_filter_validation()
        && test_bit_order_and_output_count()
        && test_chunked_streaming_matches_single_block()
        && test_low_pass_rejects_alternating_dsd();
    if (!passed) {
        return 1;
    }
    std::cout << "All DSD-to-PCM conversion tests passed.\n";
    return 0;
}
