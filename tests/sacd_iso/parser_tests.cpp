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

using Bytes = std::vector<std::byte>;
constexpr std::size_t kImageSectors = 700U;
constexpr std::size_t kMasterOffset = sacd_iso::master_toc_lsn * sacd_iso::logical_sector_size;
constexpr std::size_t kAreaSize = 3U;
constexpr std::uint32_t kTrackStart = 600U;

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

void write_identifier(
    Bytes& bytes,
    std::size_t offset,
    std::array<char, 8> identifier) {
    for (std::size_t index = 0; index < identifier.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(identifier[index]);
    }
}

void make_area(
    Bytes& image,
    std::uint32_t toc_lsn,
    sacd_iso::AreaKind kind,
    bool include_track_list = true) {
    const auto area_offset = static_cast<std::size_t>(toc_lsn) * sacd_iso::logical_sector_size;
    if (kind == sacd_iso::AreaKind::stereo) {
        write_identifier(image, area_offset, {'T', 'W', 'O', 'C', 'H', 'T', 'O', 'C'});
    } else {
        write_identifier(image, area_offset, {'M', 'U', 'L', 'C', 'H', 'T', 'O', 'C'});
    }
    image[area_offset + 8U] = std::byte{1};
    image[area_offset + 9U] = std::byte{20};
    write_u16_be(image, area_offset + 10U, static_cast<std::uint16_t>(kAreaSize));
    image[area_offset + 20U] = std::byte{4};
    image[area_offset + 21U] = std::byte{2};
    image[area_offset + 32U] = kind == sacd_iso::AreaKind::stereo
        ? std::byte{2}
        : std::byte{6};
    image[area_offset + 33U] = kind == sacd_iso::AreaKind::stereo
        ? std::byte{0}
        : std::byte{static_cast<unsigned char>(6U << 3U)};
    image[area_offset + 69U] = std::byte{2};
    write_u32_be(image, area_offset + 72U, kTrackStart);
    write_u32_be(image, area_offset + 76U, 699U);

    if (!include_track_list) {
        return;
    }

    const auto list_offset = area_offset + sacd_iso::logical_sector_size;
    write_identifier(image, list_offset, {'S', 'A', 'C', 'D', 'T', 'R', 'L', '1'});
    const std::size_t track_starts_offset = list_offset + 8U;
    const std::size_t track_lengths_offset = track_starts_offset
        + sacd_iso::maximum_track_count * sizeof(std::uint32_t);
    write_u32_be(image, track_starts_offset, 600U);
    write_u32_be(image, track_starts_offset + sizeof(std::uint32_t), 620U);
    write_u32_be(image, track_lengths_offset, 20U);
    write_u32_be(image, track_lengths_offset + sizeof(std::uint32_t), 80U);
}

Bytes make_fixture(bool include_multichannel = false) {
    Bytes image(kImageSectors * sacd_iso::logical_sector_size);
    write_identifier(image, kMasterOffset, {'S', 'A', 'C', 'D', 'M', 'T', 'O', 'C'});
    image[kMasterOffset + 8U] = std::byte{1};
    image[kMasterOffset + 9U] = std::byte{20};
    for (std::size_t text_index = 0; text_index < 8U; ++text_index) {
        write_identifier(
            image,
            kMasterOffset + (text_index + 1U) * sacd_iso::logical_sector_size,
            {'S', 'A', 'C', 'D', 'T', 'e', 'x', 't'});
    }
    write_identifier(
        image,
        kMasterOffset + (sacd_iso::master_toc_sector_count - 1U)
            * sacd_iso::logical_sector_size,
        {'S', 'A', 'C', 'D', '_', 'M', 'a', 'n'});
    write_u32_be(image, kMasterOffset + 64U, 520U);
    write_u32_be(image, kMasterOffset + 68U, 530U);
    write_u16_be(image, kMasterOffset + 84U, static_cast<std::uint16_t>(kAreaSize));
    make_area(image, 520U, sacd_iso::AreaKind::stereo);
    make_area(image, 530U, sacd_iso::AreaKind::stereo);

    if (include_multichannel) {
        write_u32_be(image, kMasterOffset + 72U, 540U);
        write_u32_be(image, kMasterOffset + 76U, 550U);
        write_u16_be(image, kMasterOffset + 86U, static_cast<std::uint16_t>(kAreaSize));
        make_area(image, 540U, sacd_iso::AreaKind::multichannel);
        make_area(image, 550U, sacd_iso::AreaKind::multichannel);
    }
    return image;
}

bool test_stereo_area_and_sector_extents() {
    const auto image = make_fixture();
    const auto parsed = sacd_iso::parse(image);
    if (!check(parsed.has_value(), "valid stereo image parses")) {
        return false;
    }
    if (!check(parsed->logical_sector_count == kImageSectors, "image sector count is retained")
            || !check(parsed->area_count == 1U, "one area is reported")
            || !check(parsed->areas[0].kind == sacd_iso::AreaKind::stereo, "stereo area is identified")
            || !check(parsed->areas[0].toc_lsn == 520U, "primary area TOC is selected")
            || !check(!parsed->areas[0].used_backup_toc, "primary TOC status is retained")
            || !check(parsed->areas[0].channel_count == 2U, "channel count is parsed")
            || !check(parsed->areas[0].sample_frequency_code == 4U, "sample-frequency code is parsed")
            || !check(parsed->areas[0].frame_format_code == 2U, "frame-format nibble is parsed")
            || !check(parsed->areas[0].track_count == 2U, "track count is parsed")
            || !check(parsed->areas[0].tracks[0].start_lsn == 600U, "first track start is parsed")
            || !check(parsed->areas[0].tracks[0].length_lsn == 20U, "first track length is parsed")
            || !check(parsed->areas[0].tracks[0].end_lsn_exclusive() == 620U,
                      "track end is represented as an exclusive bound")
            || !check(parsed->areas[0].tracks[1].end_lsn_exclusive() == 700U,
                      "last track ends at the image boundary")) {
        return false;
    }
    return true;
}

bool test_backup_area_toc_and_multichannel_metadata() {
    auto image = make_fixture(true);
    const auto primary_offset = 520U * sacd_iso::logical_sector_size;
    write_identifier(image, primary_offset, {'B', 'A', 'D', 'T', 'O', 'C', '!', '!'});

    const auto parsed = sacd_iso::parse(image);
    if (!check(parsed.has_value(), "usable backup TOC recovers invalid primary")) {
        return false;
    }
    return check(parsed->areas[0].used_backup_toc, "backup selection is reported")
        && check(parsed->areas[0].toc_lsn == 530U, "backup TOC address is retained")
        && check(parsed->area_count == 2U, "stereo and multichannel areas are retained")
        && check(parsed->areas[1].kind == sacd_iso::AreaKind::multichannel,
                 "multichannel area is identified")
        && check(parsed->areas[1].channel_count == 6U, "multichannel channel count is retained")
        && check(parsed->areas[1].loudspeaker_config == 6U,
                 "multichannel speaker configuration bits are parsed");
}

bool test_rejects_truncated_and_malformed_images() {
    auto image = make_fixture();
    const auto truncated = std::span<const std::byte>(image).first(image.size() - 1U);
    const auto wrong_sector_size = sacd_iso::parse(truncated);
    if (!check(!wrong_sector_size
                   && wrong_sector_size.error().code == sacd_iso::ErrorCode::unsupported_sector_size,
               "non-sector-aligned image is rejected")) {
        return false;
    }

    const auto short_image = std::span<const std::byte>(image).first(
        (static_cast<std::size_t>(sacd_iso::master_toc_lsn) + 9U)
        * sacd_iso::logical_sector_size);
    const auto truncated_toc = sacd_iso::parse(short_image);
    if (!check(!truncated_toc
                   && truncated_toc.error().code == sacd_iso::ErrorCode::truncated_image,
               "truncated master TOC extent is rejected")) {
        return false;
    }

    write_identifier(image, kMasterOffset, {'N', 'O', 'T', 'S', 'A', 'C', 'D', '!'});
    const auto bad_master = sacd_iso::parse(image);
    if (!check(!bad_master
                   && bad_master.error().code == sacd_iso::ErrorCode::invalid_master_toc_signature,
               "invalid master TOC signature is rejected")) {
        return false;
    }

    image = make_fixture();
    image[kMasterOffset + 9U] = std::byte{21};
    const auto new_version = sacd_iso::parse(image);
    if (!check(!new_version
                   && new_version.error().code
                       == sacd_iso::ErrorCode::unsupported_master_toc_version,
               "unsupported master TOC version is rejected")) {
        return false;
    }

    image = make_fixture();
    write_identifier(image, kMasterOffset + sacd_iso::logical_sector_size,
                     {'B', 'A', 'D', 'T', 'e', 'x', 't', '!'});
    const auto bad_text_block = sacd_iso::parse(image);
    return check(!bad_text_block
                     && bad_text_block.error().code
                         == sacd_iso::ErrorCode::invalid_master_toc_signature,
                 "invalid master TOC text block is rejected");
}

bool test_rejects_unsafe_area_and_track_extents() {
    auto image = make_fixture();
    write_u32_be(image, kMasterOffset + 64U, 699U);
    write_u32_be(image, kMasterOffset + 68U, 0U);
    const auto outside_image = sacd_iso::parse(image);
    if (!check(!outside_image
                   && outside_image.error().code == sacd_iso::ErrorCode::invalid_area_pointer,
               "area TOC outside the image is rejected")) {
        return false;
    }

    image = make_fixture();
    constexpr std::size_t primary_track_length_offset =
        520U * sacd_iso::logical_sector_size + sacd_iso::logical_sector_size
        + 8U + sacd_iso::maximum_track_count * sizeof(std::uint32_t);
    constexpr std::size_t backup_track_length_offset =
        530U * sacd_iso::logical_sector_size + sacd_iso::logical_sector_size
        + 8U + sacd_iso::maximum_track_count * sizeof(std::uint32_t);
    write_u32_be(image, primary_track_length_offset, 1'000U);
    write_u32_be(image, backup_track_length_offset, 1'000U);
    const auto bad_extent = sacd_iso::parse(image);
    return check(!bad_extent
                     && bad_extent.error().code == sacd_iso::ErrorCode::invalid_track_extent,
                 "track extent outside its audio area is rejected");
}

bool test_requires_track_list_and_valid_area_size() {
    auto image = make_fixture();
    for (const auto toc_lsn : {520U, 530U}) {
        const auto list_sector = static_cast<std::size_t>(toc_lsn) * sacd_iso::logical_sector_size
            + sacd_iso::logical_sector_size;
        for (std::size_t index = 0; index < sacd_iso::logical_sector_size; ++index) {
            image[list_sector + index] = std::byte{};
        }
    }
    make_area(image, 520U, sacd_iso::AreaKind::stereo, false);
    make_area(image, 530U, sacd_iso::AreaKind::stereo, false);
    const auto no_track_list = sacd_iso::parse(image);
    if (!check(!no_track_list
                   && no_track_list.error().code == sacd_iso::ErrorCode::missing_track_list,
               "area without SACDTRL1 is rejected")) {
        return false;
    }

    image = make_fixture();
    write_u16_be(image, 520U * sacd_iso::logical_sector_size + 10U, 2U);
    write_u16_be(image, 530U * sacd_iso::logical_sector_size + 10U, 2U);
    const auto inconsistent_size = sacd_iso::parse(image);
    return check(!inconsistent_size
                     && inconsistent_size.error().code == sacd_iso::ErrorCode::invalid_area_toc_size,
                 "area TOC size must agree with the master TOC");
}

} // namespace

int main() {
    const bool passed = test_stereo_area_and_sector_extents()
        && test_backup_area_toc_and_multichannel_metadata()
        && test_rejects_truncated_and_malformed_images()
        && test_rejects_unsafe_area_and_track_extents()
        && test_requires_track_list_and_valid_area_size();
    if (!passed) {
        return 1;
    }
    std::cout << "All SACD ISO parser tests passed.\n";
    return 0;
}
