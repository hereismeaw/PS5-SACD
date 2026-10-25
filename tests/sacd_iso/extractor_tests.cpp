#include "ps5_sacd/audio/dsd_to_wav.hpp"
#include "ps5_sacd/formats/dsdiff/parser.hpp"
#include "ps5_sacd/formats/sacd_iso/extractor.hpp"
#include "ps5_sacd/formats/sacd_iso/parser.hpp"
#include "ps5_sacd/formats/sacd_iso/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dsdiff = ps5_sacd::formats::dsdiff;
namespace audio = ps5_sacd::audio;
namespace sacd_iso = ps5_sacd::formats::sacd_iso;

namespace {

using Bytes = std::vector<std::byte>;
constexpr std::size_t kSectorSize = sacd_iso::logical_sector_size;
constexpr std::size_t kMasterOffset = sacd_iso::master_toc_lsn * kSectorSize;
constexpr std::size_t kImageSectorCount = 700U;

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

void write_u16_be(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>(value & 0xFFU);
}

void write_u32_be(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned int>((sizeof(value) - index - 1U) * 8U);
        bytes[offset + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write_id(Bytes& bytes, std::size_t offset, std::string_view id) {
    for (std::size_t index = 0; index < id.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(id[index]);
    }
}

void make_toc(
    Bytes& image,
    std::uint32_t toc_lsn,
    std::uint8_t frame_format,
    sacd_iso::AreaKind area_kind,
    std::uint8_t channel_count,
    std::uint32_t track_start_lsn,
    std::uint32_t track_sector_count) {
    const auto offset = static_cast<std::size_t>(toc_lsn) * kSectorSize;
    write_id(image, offset, area_kind == sacd_iso::AreaKind::stereo
        ? "TWOCHTOC"
        : "MULCHTOC");
    image[offset + 8U] = std::byte{1};
    image[offset + 9U] = std::byte{20};
    write_u16_be(image, offset + 10U, 4U);
    image[offset + 20U] = std::byte{4};
    image[offset + 21U] = static_cast<std::byte>(frame_format);
    image[offset + 32U] = static_cast<std::byte>(channel_count);
    image[offset + 69U] = std::byte{1};
    image[offset + 80U] = std::byte{1};
    write_id(image, offset + 88U, "en");
    image[offset + 90U] = std::byte{2};
    write_u32_be(image, offset + 72U, track_start_lsn);
    write_u32_be(image, offset + 76U, track_start_lsn + track_sector_count - 1U);

    const auto track_list = offset + kSectorSize;
    write_id(image, track_list, "SACDTRL1");
    write_u32_be(image, track_list + 8U, track_start_lsn);
    write_u32_be(
        image,
        track_list + 8U + 255U * sizeof(std::uint32_t),
        track_sector_count);

    const auto text_sector = offset + 2U * kSectorSize;
    write_id(image, text_sector, "SACDTTxt");
    write_u16_be(image, text_sector + 8U, 16U);
    image[text_sector + 16U] = std::byte{2};
    image[text_sector + 20U] = std::byte{1};
    image[text_sector + 21U] = std::byte{0x20};
    constexpr std::string_view track_title = "Track One";
    write_id(image, text_sector + 22U, track_title);
    const auto performer_record = text_sector + 22U + track_title.size() + 1U;
    image[performer_record] = std::byte{2};
    image[performer_record + 1U] = std::byte{0x20};
    write_id(image, performer_record + 2U, "Artist");
}

Bytes make_sector(
    std::span<const std::byte> payload,
    bool frame_start,
    bool dst = false) {
    Bytes sector(kSectorSize);
    sector[0] = static_cast<std::byte>(
        (1U << 5U) | (frame_start ? 1U << 2U : 0U) | (dst ? 1U : 0U));
    const auto length_high = static_cast<std::uint8_t>((payload.size() >> 8U) & 0x07U);
    sector[1] = static_cast<std::byte>(
        (frame_start ? 0x80U : 0U) | (2U << 3U) | length_high);
    sector[2] = static_cast<std::byte>(payload.size() & 0xFFU);
    const std::size_t data_offset = frame_start ? 6U : 3U;
    if (frame_start) {
        sector[3] = std::byte{0};
        sector[4] = std::byte{0};
        sector[5] = std::byte{0};
    }
    std::copy(payload.begin(), payload.end(), sector.begin() + static_cast<std::ptrdiff_t>(data_offset));
    return sector;
}

std::vector<Bytes> make_audio_sectors(std::uint8_t channel_count) {
    Bytes frame(static_cast<std::size_t>(channel_count)
        * sacd_iso::dsd64_frame_bytes_per_channel);
    for (std::size_t index = 0; index < frame.size(); ++index) {
        frame[index] = static_cast<std::byte>((index * 29U + 11U) & 0xFFU);
    }

    std::vector<Bytes> sectors;
    std::size_t frame_offset = 0U;
    while (frame_offset < frame.size()) {
        const std::size_t capacity = sectors.empty() ? 2042U : 2045U;
        const auto length = std::min(capacity, frame.size() - frame_offset);
        sectors.push_back(make_sector(
            std::span<const std::byte>(frame).subspan(frame_offset, length),
            sectors.empty()));
        frame_offset += length;
    }
    return sectors;
}

Bytes make_fixture(
    std::uint8_t channel_count = 2U,
    sacd_iso::AreaKind area_kind = sacd_iso::AreaKind::stereo,
    std::uint8_t frame_format = 2U) {
    Bytes image(kImageSectorCount * kSectorSize);
    const auto primary_toc_lsn = area_kind == sacd_iso::AreaKind::stereo ? 520U : 540U;
    const auto backup_toc_lsn = primary_toc_lsn + 10U;
    constexpr std::uint32_t track_start_lsn = 600U;
    const auto audio_sectors = make_audio_sectors(channel_count);
    write_id(image, kMasterOffset, "SACDMTOC");
    image[kMasterOffset + 8U] = std::byte{1};
    image[kMasterOffset + 9U] = std::byte{20};
    image[kMasterOffset + 128U] = std::byte{1};
    write_id(image, kMasterOffset + 136U, "en");
    image[kMasterOffset + 138U] = std::byte{2};
    for (std::size_t index = 0; index < 8U; ++index) {
        write_id(image, kMasterOffset + (index + 1U) * kSectorSize, "SACDText");
    }
    const auto master_text = kMasterOffset + kSectorSize;
    write_u16_be(image, master_text + 16U, 48U);
    write_u16_be(image, master_text + 18U, 64U);
    write_u16_be(image, master_text + 32U, 80U);
    write_id(image, master_text + 48U, "Album Title");
    write_id(image, master_text + 64U, std::string_view("Caf\xE9", 4U));
    write_id(image, master_text + 80U, "Disc Title");
    write_id(image, kMasterOffset + 9U * kSectorSize, "SACD_Man");
    const auto area_pointer_offset = area_kind == sacd_iso::AreaKind::stereo ? 64U : 72U;
    const auto area_size_offset = area_kind == sacd_iso::AreaKind::stereo ? 84U : 86U;
    write_u32_be(image, kMasterOffset + area_pointer_offset, primary_toc_lsn);
    write_u32_be(image, kMasterOffset + area_pointer_offset + 4U, backup_toc_lsn);
    write_u16_be(image, kMasterOffset + area_size_offset, 4U);
    make_toc(
        image, primary_toc_lsn, frame_format, area_kind, channel_count,
        track_start_lsn, static_cast<std::uint32_t>(audio_sectors.size()));
    make_toc(
        image, backup_toc_lsn, frame_format, area_kind, channel_count,
        track_start_lsn, static_cast<std::uint32_t>(audio_sectors.size()));

    for (std::size_t index = 0; index < audio_sectors.size(); ++index) {
        const auto sector_offset = static_cast<std::size_t>(track_start_lsn + index) * kSectorSize;
        std::copy(
            audio_sectors[index].begin(),
            audio_sectors[index].end(),
            image.begin() + static_cast<std::ptrdiff_t>(sector_offset));
    }
    return image;
}

Bytes as_bytes(const std::string& text) {
    Bytes result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

bool test_extracts_plain_stereo_dsd_as_dsdiff() {
    const auto image = make_fixture();
    std::ostringstream output(std::ios::binary | std::ios::out);
    const auto extracted = sacd_iso::extract_track_to_dsdiff(
        image,
        sacd_iso::AreaKind::stereo,
        1U,
        output);
    if (!check(extracted.has_value(), "plain stereo SACD track extracts")) {
        return false;
    }

    const auto parsed = dsdiff::parse(as_bytes(output.str()));
    if (!check(parsed.has_value(), "extracted DSDIFF passes the project parser")) {
        return false;
    }
    Bytes expected(2U * sacd_iso::dsd64_frame_bytes_per_channel);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<std::byte>((index * 29U + 11U) & 0xFFU);
    }
    return check(extracted->frame_count == 1U, "one multiplexed DSD frame was extracted")
        && check(extracted->sector_count == 5U, "only the selected track sectors were read")
        && check(parsed->sample_rate_hz == 2'822'400U, "output DSD sample rate is correct")
        && check(parsed->audio_data.size() == expected.size(), "output frame size is correct")
        && check(std::equal(parsed->audio_data.begin(), parsed->audio_data.end(), expected.begin()),
                 "SACD audio packets are reassembled in order");
}

bool test_dst_and_invalid_track_are_rejected() {
    const auto image = make_fixture();
    std::ostringstream output(std::ios::binary | std::ios::out);
    const auto bad_track = sacd_iso::extract_track_to_dsdiff(
        image,
        sacd_iso::AreaKind::stereo,
        2U,
        output);
    if (!check(!bad_track
                   && bad_track.error().code == sacd_iso::ExtractionErrorCode::invalid_track_number,
               "track numbers are checked")) {
        return false;
    }

    auto dst_image = image;
    constexpr std::size_t first_audio_sector = 600U * kSectorSize;
    dst_image[first_audio_sector] = std::byte{0x21};
    const auto dst_result = sacd_iso::extract_track_to_dsdiff(
        dst_image,
        sacd_iso::AreaKind::stereo,
        1U,
        output);
    if (!check(!dst_result
                     && dst_result.error().code == sacd_iso::ExtractionErrorCode::sector_frame_error
                     && dst_result.error().audio_error
                         == sacd_iso::AudioFrameErrorCode::unsupported_dst_frame,
                 "DST-marked audio is rejected instead of being copied as DSD")) {
        return false;
    }

    const auto dst_area_image = make_fixture(2U, sacd_iso::AreaKind::stereo, 0U);
    std::ostringstream dst_area_output(std::ios::binary | std::ios::out);
    const auto dst_area = sacd_iso::extract_track_to_dsdiff(
        dst_area_image,
        sacd_iso::AreaKind::stereo,
        1U,
        dst_area_output);
    if (!check(!dst_area
                   && dst_area.error().code == sacd_iso::ExtractionErrorCode::dst_not_supported,
               "DST area format code is rejected before writing audio")) {
        return false;
    }

    const auto alternate_plain_image = make_fixture(2U, sacd_iso::AreaKind::stereo, 3U);
    std::ostringstream alternate_plain_output(std::ios::binary | std::ios::out);
    const auto alternate_plain = sacd_iso::extract_track_to_dsdiff(
        alternate_plain_image,
        sacd_iso::AreaKind::stereo,
        1U,
        alternate_plain_output);
    if (!check(alternate_plain.has_value(), "plain DSD frame format code 3 extracts")) {
        return false;
    }

    const auto unsupported_layout_image = make_fixture(4U, sacd_iso::AreaKind::multichannel);
    std::ostringstream unsupported_layout_output(std::ios::binary | std::ios::out);
    const auto unsupported_layout = sacd_iso::extract_track_to_dsdiff(
        unsupported_layout_image,
        sacd_iso::AreaKind::multichannel,
        1U,
        unsupported_layout_output);
    return check(!unsupported_layout
                     && unsupported_layout.error().code
                         == sacd_iso::ExtractionErrorCode::unsupported_channel_layout,
                 "nonstandard four-channel layout is rejected explicitly");
}

bool test_sacd_text_decodes_supported_character_sets_and_bounds_offsets() {
    auto image = make_fixture();
    const auto text = sacd_iso::parse_text(image, sacd_iso::AreaKind::stereo);
    if (!check(text.has_value(), "SACD-Text metadata is parsed")) {
        return false;
    }
    if (!check(text->language_code == "en", "SACD-Text language is reported")
        || !check(text->album_title == "Album Title", "album title is decoded")
        || !check(text->album_artist == "Caf\xC3\xA9", "Latin-1 is converted to UTF-8")
        || !check(text->disc_title == "Disc Title", "disc title is decoded")
        || !check(text->tracks.size() == 1U, "track text follows the area track count")
        || !check(text->tracks[0].title == "Track One", "track title is decoded")
        || !check(text->tracks[0].performer == "Artist", "track performer is decoded")) {
        return false;
    }

    auto unsupported_charset_image = image;
    unsupported_charset_image[kMasterOffset + 138U] = std::byte{3};
    const auto unsupported = sacd_iso::parse_text(
        unsupported_charset_image,
        sacd_iso::AreaKind::stereo);
    if (!check(!unsupported
                   && unsupported.error().code
                       == sacd_iso::TextErrorCode::unsupported_character_set,
               "unsupported legacy charset is reported explicitly")) {
        return false;
    }

    auto invalid_offset_image = image;
    write_u16_be(invalid_offset_image, kMasterOffset + kSectorSize + 16U, 2048U);
    const auto invalid_offset = sacd_iso::parse_text(
        invalid_offset_image,
        sacd_iso::AreaKind::stereo);
    return check(!invalid_offset
                     && invalid_offset.error().code
                         == sacd_iso::TextErrorCode::invalid_text_metadata,
                 "out-of-sector SACD-Text offsets are rejected");
}

bool test_extracts_standard_multichannel_layouts() {
    constexpr std::array<std::uint8_t, 2> channel_counts{5U, 6U};
    constexpr std::array<dsdiff::ChannelId, 6> five_channel_ids{
        dsdiff::ChannelId{'M', 'L', 'F', 'T'},
        dsdiff::ChannelId{'M', 'R', 'G', 'T'},
        dsdiff::ChannelId{'C', ' ', ' ', ' '},
        dsdiff::ChannelId{'L', 'S', ' ', ' '},
        dsdiff::ChannelId{'R', 'S', ' ', ' '},
        dsdiff::ChannelId{},
    };
    constexpr std::array<dsdiff::ChannelId, 6> six_channel_ids{
        dsdiff::ChannelId{'M', 'L', 'F', 'T'},
        dsdiff::ChannelId{'M', 'R', 'G', 'T'},
        dsdiff::ChannelId{'C', ' ', ' ', ' '},
        dsdiff::ChannelId{'L', 'F', 'E', ' '},
        dsdiff::ChannelId{'L', 'S', ' ', ' '},
        dsdiff::ChannelId{'R', 'S', ' ', ' '},
    };

    for (const auto channel_count : channel_counts) {
        const auto image = make_fixture(channel_count, sacd_iso::AreaKind::multichannel);
        std::ostringstream output(std::ios::binary | std::ios::out);
        const auto extracted = sacd_iso::extract_track_to_dsdiff(
            image,
            sacd_iso::AreaKind::multichannel,
            1U,
            output);
        if (!check(extracted.has_value(), "standard SACD multichannel track extracts")) {
            return false;
        }

        const auto dsdiff_bytes = as_bytes(output.str());
        const auto parsed = dsdiff::parse(dsdiff_bytes);
        if (!check(parsed.has_value(), "multichannel extraction is valid DSDIFF")) {
            return false;
        }
        const auto& expected_ids = channel_count == 5U ? five_channel_ids : six_channel_ids;
        const auto expected_loudspeaker_configuration = channel_count == 5U ? 3U : 4U;
        if (!check(parsed->channel_count() == channel_count,
                   "multichannel count survives DSDIFF serialization")
            || !check(parsed->loudspeaker_configuration
                          == std::optional<std::uint16_t>{expected_loudspeaker_configuration},
                      "standard surround configuration is written")
            || !check(std::equal(
                          parsed->channel_ids.begin(),
                          parsed->channel_ids.end(),
                          expected_ids.begin()),
                      "SACD channel order maps to DSDIFF speaker identifiers")
            || !check(parsed->audio_data.size()
                          == static_cast<std::size_t>(channel_count)
                              * sacd_iso::dsd64_frame_bytes_per_channel,
                      "complete multichannel frame data is preserved")) {
            return false;
        }

        std::ostringstream wav_output(std::ios::binary | std::ios::out);
        const auto converted = audio::convert_to_wav(dsdiff_bytes, wav_output);
        if (!check(converted.has_value(), "extracted multichannel DSDIFF converts to PCM WAV")
            || !check(converted->channel_count == channel_count,
                      "PCM conversion retains the SACD channel count")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--make-fixture") {
        const auto image = make_fixture();
        std::ofstream output(argv[2], std::ios::binary | std::ios::out | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));
        return output ? 0 : 2;
    }

    const bool passed = test_extracts_plain_stereo_dsd_as_dsdiff()
        && test_dst_and_invalid_track_are_rejected()
        && test_sacd_text_decodes_supported_character_sets_and_bounds_offsets()
        && test_extracts_standard_multichannel_layouts();
    if (!passed) {
        return 1;
    }
    std::cout << "All SACD extractor tests passed.\n";
    return 0;
}
