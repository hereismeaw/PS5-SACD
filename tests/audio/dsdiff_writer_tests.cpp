#include "ps5_sacd/audio/dsdiff_writer.hpp"
#include "ps5_sacd/formats/dsdiff/parser.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace audio = ps5_sacd::audio;
namespace dsdiff = ps5_sacd::formats::dsdiff;

namespace {

using Bytes = std::vector<std::byte>;

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

Bytes as_bytes(const std::string& text) {
    Bytes result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

bool test_streaming_writer_round_trips_through_parser() {
    std::ostringstream output(std::ios::binary | std::ios::out);
    constexpr std::array<dsdiff::ChannelId, 2> channels{
        dsdiff::ChannelId{'S', 'L', 'F', 'T'},
        dsdiff::ChannelId{'S', 'R', 'G', 'T'},
    };
    auto writer = audio::DsdiffWriter::create(output, 2'822'400U, channels, 0U);
    if (!check(writer.has_value(), "writer creates a stereo DSDIFF header")) {
        return false;
    }

    Bytes audio_data(18'816U);
    for (std::size_t index = 0; index < audio_data.size(); ++index) {
        audio_data[index] = static_cast<std::byte>((index * 13U + 7U) & 0xFFU);
    }
    const auto first_half = writer->write_audio(
        std::span<const std::byte>(audio_data).first(8'000U));
    const auto second_half = writer->write_audio(
        std::span<const std::byte>(audio_data).subspan(8'000U));
    if (!check(first_half.has_value() && second_half.has_value(), "interleaved blocks stream to file")) {
        return false;
    }
    const auto finished = writer->finish();
    if (!check(finished.has_value(), "writer patches the final DSDIFF chunk sizes")) {
        return false;
    }

    const auto file_bytes = as_bytes(output.str());
    const auto parsed = dsdiff::parse(file_bytes);
    if (!check(parsed.has_value(), "written output parses as DSDIFF")) {
        return false;
    }
    return check(parsed->sample_rate_hz == 2'822'400U, "DSD sample rate is stored")
        && check(parsed->channel_count() == 2U, "stereo layout is stored")
        && check(parsed->channel_ids[0] == channels[0] && parsed->channel_ids[1] == channels[1],
                 "channel identifiers are stored in order")
        && check(parsed->loudspeaker_configuration == std::optional<std::uint16_t>{0U},
                 "stereo loudspeaker configuration is stored")
        && check(parsed->audio_data.size() == audio_data.size(), "audio chunk size is finalized")
        && check(std::equal(parsed->audio_data.begin(), parsed->audio_data.end(), audio_data.begin()),
                 "interleaved DSD bytes are preserved");
}

bool test_writer_rejects_invalid_blocks_and_empty_output() {
    std::ostringstream output(std::ios::binary | std::ios::out);
    constexpr std::array<dsdiff::ChannelId, 2> channels{
        dsdiff::ChannelId{'S', 'L', 'F', 'T'},
        dsdiff::ChannelId{'S', 'R', 'G', 'T'},
    };
    auto writer = audio::DsdiffWriter::create(output, 2'822'400U, channels);
    if (!check(writer.has_value(), "writer creates for error-path test")) {
        return false;
    }

    const std::array<std::byte, 1> odd_block{std::byte{0xA5}};
    const auto misaligned = writer->write_audio(odd_block);
    if (!check(!misaligned
                   && misaligned.error().code
                       == audio::DsdiffWriterErrorCode::invalid_audio_block_size,
               "partial interleaved channel cluster is rejected")) {
        return false;
    }

    const auto empty_finish = writer->finish();
    if (!check(!empty_finish
                   && empty_finish.error().code == audio::DsdiffWriterErrorCode::empty_audio,
               "empty DSDIFF is not finalized")) {
        return false;
    }

    const std::array<std::byte, 4> valid_block{
        std::byte{0x11}, std::byte{0x91}, std::byte{0x12}, std::byte{0x92},
    };
    const auto write = writer->write_audio(valid_block);
    if (!check(write.has_value(), "writer accepts a complete channel cluster")) {
        return false;
    }
    return check(writer->finish().has_value(), "writer recovers after rejected calls");
}

bool test_writer_requires_a_seekable_stream_at_start() {
    std::ostringstream output(std::ios::binary | std::ios::out);
    output.put('x');
    constexpr std::array<dsdiff::ChannelId, 1> channel{
        dsdiff::ChannelId{'M', 'O', 'N', 'O'},
    };
    const auto writer = audio::DsdiffWriter::create(output, 2'822'400U, channel);
    return check(!writer
                     && writer.error().code == audio::DsdiffWriterErrorCode::stream_error,
                 "writer requires an output stream positioned at byte zero");
}

} // namespace

int main() {
    const bool passed = test_streaming_writer_round_trips_through_parser()
        && test_writer_rejects_invalid_blocks_and_empty_output()
        && test_writer_requires_a_seekable_stream_at_start();
    if (!passed) {
        return 1;
    }
    std::cout << "All DSDIFF writer tests passed.\n";
    return 0;
}
