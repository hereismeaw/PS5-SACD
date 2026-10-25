#include "ps5_sacd/formats/sacd_iso/audio_frames.hpp"
#include "ps5_sacd/formats/sacd_iso/parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace sacd_iso = ps5_sacd::formats::sacd_iso;

namespace {

using Sector = std::array<std::byte, sacd_iso::logical_sector_size>;
using Bytes = std::vector<std::byte>;

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

Sector make_audio_sector(
    std::span<const std::byte> payload,
    bool frame_start,
    sacd_iso::FrameTimecode timecode,
    bool dst = false,
    std::size_t declared_payload_size = 0U,
    std::size_t frame_info_count_override = 99U) {
    Sector sector{};
    const auto frame_info_count = frame_info_count_override == 99U
        ? static_cast<std::uint8_t>(frame_start ? 1U : 0U)
        : static_cast<std::uint8_t>(frame_info_count_override);
    sector[0] = static_cast<std::byte>(
        (1U << 5U) | (static_cast<unsigned int>(frame_info_count) << 2U) | (dst ? 1U : 0U));

    const auto packet_info_offset = 1U;
    const auto first_payload_offset = 3U
        + static_cast<std::size_t>(frame_info_count) * 3U;
    const auto length = declared_payload_size == 0U ? payload.size() : declared_payload_size;
    const auto length_high = static_cast<std::uint8_t>((length >> 8U) & 0x07U);
    const auto packet_flags = static_cast<std::uint8_t>(
        (frame_start ? 0x80U : 0U) | (2U << 3U) | length_high);
    sector[packet_info_offset] = static_cast<std::byte>(packet_flags);
    sector[packet_info_offset + 1U] = static_cast<std::byte>(length & 0xFFU);

    if (frame_info_count > 0U) {
        sector[3U] = static_cast<std::byte>(timecode.minutes);
        sector[4U] = static_cast<std::byte>(timecode.seconds);
        sector[5U] = static_cast<std::byte>(timecode.frames);
    }

    const auto available = sector.size() - first_payload_offset;
    const auto copy_size = payload.size() < available ? payload.size() : available;
    for (std::size_t index = 0; index < copy_size; ++index) {
        sector[first_payload_offset + index] = payload[index];
    }
    return sector;
}

struct CollectedFrames {
    std::vector<Bytes> audio;
    std::vector<sacd_iso::FrameTimecode> timecodes;
};

bool collect_frame(
    std::span<const std::byte> frame,
    sacd_iso::FrameTimecode timecode,
    void* context) {
    auto& collected = *static_cast<CollectedFrames*>(context);
    collected.audio.emplace_back(frame.begin(), frame.end());
    collected.timecodes.push_back(timecode);
    return true;
}

bool test_reassembles_multiple_plain_dsd_frames() {
    sacd_iso::AudioFrameAssembler assembler(2U);
    if (!check(assembler.frame_size_bytes() == 2U * sacd_iso::dsd64_frame_bytes_per_channel,
               "stereo frame has 4704 bytes per channel")) {
        return false;
    }

    const auto frame_size = assembler.frame_size_bytes();
    Bytes first_frame(frame_size);
    Bytes second_frame(frame_size);
    for (std::size_t index = 0; index < frame_size; ++index) {
        first_frame[index] = static_cast<std::byte>(index & 0xFFU);
        second_frame[index] = static_cast<std::byte>((index * 17U + 3U) & 0xFFU);
    }

    CollectedFrames collected;
    constexpr std::array<std::size_t, 5> packet_lengths{2042U, 2045U, 2045U, 2045U, 1231U};
    std::size_t frame_offset = 0U;
    for (const auto length : packet_lengths) {
        const auto sector = make_audio_sector(
            std::span<const std::byte>(first_frame).subspan(frame_offset, length),
            frame_offset == 0U,
            {0U, 0U, 0U});
        const auto parsed = assembler.process_sector(sector, collect_frame, &collected);
        if (!check(parsed.has_value(), "first frame packet sector parses")) {
            return false;
        }
        frame_offset += length;
    }
    if (!check(frame_offset == frame_size, "packet sizes cover one full stereo DSD frame")) {
        return false;
    }

    frame_offset = 0U;
    for (const auto length : packet_lengths) {
        const auto sector = make_audio_sector(
            std::span<const std::byte>(second_frame).subspan(frame_offset, length),
            frame_offset == 0U,
            {0U, 0U, 1U});
        const auto parsed = assembler.process_sector(sector, collect_frame, &collected);
        if (!check(parsed.has_value(), "second frame packet sector parses")) {
            return false;
        }
        frame_offset += length;
    }

    const auto finished = assembler.finish(collect_frame, &collected);
    if (!check(finished.has_value(), "last complete frame flushes")) {
        return false;
    }
    return check(collected.audio.size() == 2U, "both completed frames are delivered")
        && check(collected.audio[0] == first_frame, "first frame payload is preserved")
        && check(collected.audio[1] == second_frame, "second frame payload is preserved")
        && check(collected.timecodes[0].frames == 0U && collected.timecodes[1].frames == 1U,
                 "frame timecodes follow each frame start");
}

bool test_rejects_dst_and_malformed_sectors() {
    sacd_iso::AudioFrameAssembler assembler(2U);
    const Bytes payload{std::byte{0x12}};
    auto sector = make_audio_sector(payload, true, {0U, 0U, 0U}, true);
    auto result = assembler.process_sector(sector, collect_frame, nullptr);
    if (!check(!result
                   && result.error().code == sacd_iso::AudioFrameErrorCode::unsupported_dst_frame,
               "DST-marked sector is not treated as raw DSD")) {
        return false;
    }

    sector = make_audio_sector(payload, true, {0U, 0U, 0U}, false, 2043U);
    result = assembler.process_sector(sector, collect_frame, nullptr);
    if (!check(!result
                   && result.error().code == sacd_iso::AudioFrameErrorCode::packet_out_of_bounds,
               "packet payload past sector end is rejected")) {
        return false;
    }

    sector = make_audio_sector(payload, true, {0U, 0U, 0U}, false, 1U, 0U);
    result = assembler.process_sector(sector, collect_frame, nullptr);
    return check(!result
                     && result.error().code
                         == sacd_iso::AudioFrameErrorCode::truncated_packet_headers,
                 "frame start count must match frame-info records");
}

bool test_rejects_incomplete_or_oversized_frames() {
    sacd_iso::AudioFrameAssembler incomplete_assembler(1U);
    Bytes partial(100U, std::byte{0xA5});
    auto sector = make_audio_sector(partial, true, {0U, 0U, 0U});
    auto parsed = incomplete_assembler.process_sector(sector, collect_frame, nullptr);
    if (!check(parsed.has_value(), "partial frame sector parses before the end")) {
        return false;
    }
    CollectedFrames collected;
    const auto incomplete = incomplete_assembler.finish(collect_frame, &collected);
    if (!check(!incomplete
                   && incomplete.error().code == sacd_iso::AudioFrameErrorCode::incomplete_frame,
               "partial final frame is rejected")) {
        return false;
    }

    sacd_iso::AudioFrameAssembler oversized_assembler(1U);
    Bytes large_payload(2042U, std::byte{0x55});
    for (int index = 0; index < 2; ++index) {
        sector = make_audio_sector(large_payload, index == 0, {0U, 0U, 0U});
        parsed = oversized_assembler.process_sector(sector, collect_frame, &collected);
        if (!check(parsed.has_value(), "large frame prefix remains within its bound")) {
            return false;
        }
    }
    Bytes last_payload(700U, std::byte{0x33});
    sector = make_audio_sector(last_payload, false, {0U, 0U, 0U});
    parsed = oversized_assembler.process_sector(sector, collect_frame, &collected);
    return check(!parsed
                     && parsed.error().code == sacd_iso::AudioFrameErrorCode::frame_size_exceeded,
                 "oversized DSD frame is rejected");
}

} // namespace

int main() {
    const bool passed = test_reassembles_multiple_plain_dsd_frames()
        && test_rejects_dst_and_malformed_sectors()
        && test_rejects_incomplete_or_oversized_frames();
    if (!passed) {
        return 1;
    }
    std::cout << "All SACD audio-frame tests passed.\n";
    return 0;
}
