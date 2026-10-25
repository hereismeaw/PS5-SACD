#include "ps5_sacd/audio/dsd_to_wav.hpp"
#include "mapped_input_file.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
using ps5_sacd::cli_detail::MappedInputFile;

void print_usage(std::ostream& output) {
    output << "Usage: dsd2wav [--rate HZ] INPUT.dsf|INPUT.dff OUTPUT.wav\n"
              "Convert DSF or uncompressed DSDIFF to 24-bit PCM WAV.\n"
              "Default rate: 44100 Hz for the 44.1 kHz DSD family, otherwise 48000 Hz.\n"
              "DST-compressed DSDIFF is not supported.\n";
}

[[nodiscard]] bool parse_rate(std::string_view text, std::uint32_t& rate) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), rate);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && rate != 0U;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }

    std::uint32_t output_rate = 0U;
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    std::size_t positional_count = 0U;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--rate") {
            if (index + 1 >= argc || !parse_rate(argv[index + 1], output_rate)) {
                std::cerr << "--rate expects a positive integer sample rate in Hz.\n";
                print_usage(std::cerr);
                return 2;
            }
            ++index;
        } else if (argument.starts_with("--")) {
            std::cerr << "Unknown option: " << argument << '\n';
            print_usage(std::cerr);
            return 2;
        } else if (positional_count == 0U) {
            input_path = std::filesystem::path(argv[index]);
            ++positional_count;
        } else if (positional_count == 1U) {
            output_path = std::filesystem::path(argv[index]);
            ++positional_count;
        } else {
            std::cerr << "Too many positional arguments.\n";
            print_usage(std::cerr);
            return 2;
        }
    }
    if (positional_count != 2U) {
        print_usage(std::cerr);
        return 2;
    }

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

    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::out);
        if (!output) {
            std::cerr << "Could not create output file: " << temporary_path.string() << '\n';
            return 1;
        }
        const auto conversion = ps5_sacd::audio::convert_to_wav(input_file.bytes(), output, output_rate);
        if (!conversion) {
            std::cerr << conversion.error().detail;
            if (conversion.error().input_offset != 0U) {
                std::cerr << " at input byte " << conversion.error().input_offset;
            }
            std::cerr << '\n';
            output.close();
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }
        output.flush();
        if (!output) {
            std::cerr << "Could not finish writing the WAV file.\n";
            output.close();
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }
        output.close();
        if (!output) {
            std::cerr << "Could not close the WAV file cleanly.\n";
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }

        std::filesystem::rename(temporary_path, output_path, filesystem_error);
        if (filesystem_error) {
            std::cerr << "Could not publish WAV output: " << filesystem_error.message() << '\n';
            std::filesystem::remove(temporary_path, filesystem_error);
            return 1;
        }
        std::cout << "Wrote " << conversion->output_frame_count << " frames, "
                  << conversion->channel_count << " channels at "
                  << conversion->output_sample_rate_hz << " Hz: " << output_path.string() << '\n';
    }
    return 0;
}
