#include "mapped_input_file.hpp"
#include "ps5_sacd/formats/sacd_iso/extractor.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>

namespace sacd_iso = ps5_sacd::formats::sacd_iso;

namespace {

using ps5_sacd::cli_detail::MappedInputFile;

void print_usage(std::ostream& output) {
    output << "Usage: sacdextract [--area stereo|multichannel] INPUT.iso TRACK OUTPUT.dff\n"
              "Extract one uncompressed DSD64 SACD track to DSDIFF.\n"
              "Supports standard 2, 5, or 6 channel layouts; DST decoding is not supported.\n";
}

[[nodiscard]] bool parse_track_number(std::string_view text, std::uint32_t& track_number) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), track_number);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
        && track_number != 0U;
}

void remove_partial(const std::filesystem::path& path) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }

    sacd_iso::AreaKind area_kind = sacd_iso::AreaKind::stereo;
    std::array<std::string_view, 3> positional{};
    std::size_t positional_count = 0U;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--area") {
            if (index + 1 >= argc) {
                std::cerr << "--area expects stereo or multichannel.\n";
                print_usage(std::cerr);
                return 2;
            }
            const std::string_view value(argv[++index]);
            if (value == "stereo") {
                area_kind = sacd_iso::AreaKind::stereo;
            } else if (value == "multichannel") {
                area_kind = sacd_iso::AreaKind::multichannel;
            } else {
                std::cerr << "Unknown audio area: " << value << '\n';
                print_usage(std::cerr);
                return 2;
            }
        } else if (argument.starts_with("--")) {
            std::cerr << "Unknown option: " << argument << '\n';
            print_usage(std::cerr);
            return 2;
        } else if (positional_count < positional.size()) {
            positional[positional_count++] = argument;
        } else {
            std::cerr << "Too many positional arguments.\n";
            print_usage(std::cerr);
            return 2;
        }
    }

    if (positional_count != positional.size()) {
        print_usage(std::cerr);
        return 2;
    }

    std::uint32_t track_number = 0U;
    if (!parse_track_number(positional[1], track_number)) {
        std::cerr << "Track number must be a positive integer.\n";
        return 2;
    }

    const std::filesystem::path input_path(positional[0]);
    const std::filesystem::path output_path(positional[2]);
    std::error_code filesystem_error;
    const auto absolute_input = std::filesystem::absolute(input_path, filesystem_error).lexically_normal();
    if (filesystem_error) {
        std::cerr << "Could not resolve input path: " << filesystem_error.message() << '\n';
        return 2;
    }
    const auto absolute_output = std::filesystem::absolute(output_path, filesystem_error).lexically_normal();
    if (filesystem_error) {
        std::cerr << "Could not resolve output path: " << filesystem_error.message() << '\n';
        return 2;
    }
    if (absolute_input == absolute_output) {
        std::cerr << "Input and output paths must be different.\n";
        return 2;
    }
    if (std::filesystem::exists(output_path, filesystem_error)) {
        std::cerr << "Output already exists; choose another path: " << output_path.string() << '\n';
        return 2;
    }
    if (filesystem_error) {
        std::cerr << "Could not inspect output path: " << filesystem_error.message() << '\n';
        return 2;
    }

    MappedInputFile input_file;
    std::string_view input_error;
    if (!MappedInputFile::open(input_path, input_file, input_error)) {
        std::cerr << input_error << ": " << input_path.string() << '\n';
        return 1;
    }

    auto temporary_path = output_path;
    temporary_path += ".partial";
    if (std::filesystem::exists(temporary_path, filesystem_error) || filesystem_error) {
        std::cerr << "Temporary output path already exists or cannot be checked: "
                  << temporary_path.string() << '\n';
        return 1;
    }

    std::ofstream output(temporary_path, std::ios::binary | std::ios::out);
    if (!output) {
        std::cerr << "Could not create output file: " << temporary_path.string() << '\n';
        return 1;
    }

    const auto extracted = sacd_iso::extract_track_to_dsdiff(
        input_file.bytes(),
        area_kind,
        track_number,
        output);
    if (!extracted) {
        std::cerr << sacd_iso::message(extracted.error().code);
        if (extracted.error().lsn != 0U) {
            std::cerr << " at LSN " << extracted.error().lsn;
        }
        if (extracted.error().code == sacd_iso::ExtractionErrorCode::invalid_image) {
            std::cerr << " at image byte " << extracted.error().byte_offset;
        } else if (extracted.error().code == sacd_iso::ExtractionErrorCode::sector_frame_error) {
            std::cerr << ": " << sacd_iso::message(extracted.error().audio_error)
                      << " (sector byte " << extracted.error().byte_offset << ')';
        }
        std::cerr << '\n';
        output.close();
        remove_partial(temporary_path);
        return 1;
    }

    output.flush();
    if (!output) {
        std::cerr << "Could not finish writing DSDIFF output.\n";
        output.close();
        remove_partial(temporary_path);
        return 1;
    }
    output.close();
    if (!output) {
        std::cerr << "Could not close DSDIFF output cleanly.\n";
        remove_partial(temporary_path);
        return 1;
    }

    std::filesystem::rename(temporary_path, output_path, filesystem_error);
    if (filesystem_error) {
        std::cerr << "Could not publish DSDIFF output: " << filesystem_error.message() << '\n';
        remove_partial(temporary_path);
        return 1;
    }

    std::cout << "Wrote track " << extracted->track_number << ": "
              << extracted->frame_count << " DSD frames, "
              << extracted->sector_count << " sectors, "
              << extracted->audio_byte_count << " audio bytes to "
              << output_path.string() << '\n';
    return 0;
}
