#include "mapped_input_file.hpp"
#include "ps5_sacd/formats/sacd_iso/parser.hpp"
#include "ps5_sacd/formats/sacd_iso/text.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace sacd_iso = ps5_sacd::formats::sacd_iso;

namespace {

using ps5_sacd::cli_detail::MappedInputFile;

void print_usage(std::ostream& output) {
    output << "Usage: sacdinfo INPUT.iso\n"
              "Inspect TOCs, supported SACD-Text, audio areas, and track extents.\n"
              "Reads raw 2048-byte logical sectors. Does not extract or decode audio.\n";
}

[[nodiscard]] std::string_view area_name(sacd_iso::AreaKind kind) noexcept {
    return kind == sacd_iso::AreaKind::stereo ? "Stereo" : "Multichannel";
}

[[nodiscard]] std::string_view frame_format_name(std::uint8_t code) noexcept {
    switch (code) {
    case 0U:
        return "DST (decoder not implemented)";
    case 2U:
        return "plain DSD, 3 frames in 14 sectors";
    case 3U:
        return "plain DSD, 3 frames in 16 sectors";
    default:
        return "unknown";
    }
}

void print_area(const sacd_iso::AudioArea& area, std::size_t index) {
    std::cout << "\nArea " << index + 1U << ": " << area_name(area.kind)
              << " (" << static_cast<unsigned int>(area.channel_count) << " channels)\n"
              << "  TOC LSN: " << area.toc_lsn
              << (area.used_backup_toc ? " (backup)\n" : " (primary)\n")
              << "  Sample-frequency code: "
              << static_cast<unsigned int>(area.sample_frequency_code);
    if (area.sample_frequency_code == 4U) {
        std::cout << " (DSD64, 2,822,400 Hz)";
    }
    std::cout << "\n  Frame format: " << frame_format_name(area.frame_format_code)
              << " [" << static_cast<unsigned int>(area.frame_format_code) << "]\n"
              << "  Loudspeaker configuration: "
              << static_cast<unsigned int>(area.loudspeaker_config) << "\n"
              << "  Audio area LSN range: " << area.track_start_lsn << ".."
              << area.track_end_lsn << "\n"
              << "  Tracks: " << static_cast<unsigned int>(area.track_count) << "\n";

    for (std::size_t track_index = 0; track_index < area.track_count; ++track_index) {
        const auto& track = area.tracks[track_index];
        const auto end_lsn = track.end_lsn_exclusive() - 1U;
        std::cout << "    " << track_index + 1U << ". LSN " << track.start_lsn
                  << ".." << end_lsn << " (" << track.length_lsn << " sectors)\n";
    }
}

void print_text(const sacd_iso::TextMetadata& text) {
    const auto has_master_text = !text.album_title.empty()
        || !text.album_artist.empty()
        || !text.album_publisher.empty()
        || !text.album_copyright.empty()
        || !text.disc_title.empty()
        || !text.disc_artist.empty()
        || !text.disc_publisher.empty()
        || !text.disc_copyright.empty();
    if (!has_master_text && text.tracks.empty()) {
        std::cout << "  SACD-Text: no labels in language slot 1\n";
        return;
    }

    std::cout << "  SACD-Text language: "
              << (text.language_code.empty() ? "unknown" : text.language_code) << '\n';
    if (!text.album_title.empty()) {
        std::cout << "  Album: " << text.album_title << '\n';
    }
    if (!text.album_artist.empty()) {
        std::cout << "  Album artist: " << text.album_artist << '\n';
    }
    if (!text.disc_title.empty()) {
        std::cout << "  Disc: " << text.disc_title << '\n';
    }
    if (!text.disc_artist.empty()) {
        std::cout << "  Disc artist: " << text.disc_artist << '\n';
    }

    for (std::size_t track_index = 0; track_index < text.tracks.size(); ++track_index) {
        const auto& track = text.tracks[track_index];
        if (!track.title.empty()) {
            std::cout << "  Track " << track_index + 1U << ": " << track.title << '\n';
        }
        if (!track.performer.empty()) {
            std::cout << "    Performer: " << track.performer << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }
    if (argc != 2) {
        print_usage(std::cerr);
        return 2;
    }

    const std::filesystem::path input_path(argv[1]);
    MappedInputFile input_file;
    std::string_view input_error;
    if (!MappedInputFile::open(input_path, input_file, input_error)) {
        std::cerr << input_error << ": " << input_path.string() << '\n';
        return 1;
    }

    const auto parsed = sacd_iso::parse(input_file.bytes());
    if (!parsed) {
        std::cerr << sacd_iso::message(parsed.error().code)
                  << " at byte " << parsed.error().byte_offset << '\n';
        return 1;
    }

    std::cout << "PS5-SACD image report\n"
              << "Logical sectors: " << parsed->logical_sector_count << "\n"
              << "Audio areas: " << static_cast<unsigned int>(parsed->area_count) << "\n";
    for (std::size_t area_index = 0; area_index < parsed->area_count; ++area_index) {
        const auto& area = parsed->areas[area_index];
        print_area(area, area_index);
        const auto text = sacd_iso::parse_text(input_file.bytes(), area.kind);
        if (!text) {
            std::cout << "  SACD-Text: " << sacd_iso::message(text.error().code)
                      << " at byte " << text.error().byte_offset << '\n';
        } else {
            print_text(*text);
        }
    }
    std::cout << "\nNo audio was extracted.\n";
    return 0;
}
