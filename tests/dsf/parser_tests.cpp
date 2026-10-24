#include "ps5_sacd/formats/dsf/parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace dsf = ps5_sacd::formats::dsf;

namespace {

struct FixtureOptions {
    std::uint32_t channel_type{2U};
    std::uint32_t channel_count{2U};
    std::uint32_t sample_rate{2'822'400U};
    std::uint32_t bits_per_sample{1U};
    std::uint64_t sample_count{48U};
    std::uint32_t block_size{4U};
    bool unknown_chunk{};
    bool metadata{};
    bool data_before_format{};
};

void append_u32_le(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void append_u64_le(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void write_u32_le(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void write_u64_le(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

std::vector<std::byte> make_format_chunk(const FixtureOptions& options) {
    std::vector<std::byte> chunk;
    for (const char value : std::array{'f', 'm', 't', ' '}) {
        chunk.push_back(static_cast<std::byte>(value));
    }
    append_u64_le(chunk, 52U);
    append_u32_le(chunk, 1U);
    append_u32_le(chunk, 0U);
    append_u32_le(chunk, options.channel_type);
    append_u32_le(chunk, options.channel_count);
    append_u32_le(chunk, options.sample_rate);
    append_u32_le(chunk, options.bits_per_sample);
    append_u64_le(chunk, options.sample_count);
    append_u32_le(chunk, options.block_size);
    append_u32_le(chunk, 0U);
    return chunk;
}

std::vector<std::byte> make_data_chunk(const FixtureOptions& options) {
    const auto bytes_per_channel = options.sample_count / 8U
        + (options.sample_count % 8U == 0U ? 0U : 1U);
    const auto blocks = bytes_per_channel / options.block_size
        + (bytes_per_channel % options.block_size == 0U ? 0U : 1U);
    const auto payload_size = blocks * options.channel_count * options.block_size;

    std::vector<std::byte> chunk;
    for (const char value : std::array{'d', 'a', 't', 'a'}) {
        chunk.push_back(static_cast<std::byte>(value));
    }
    append_u64_le(chunk, 12U + payload_size);

    // A recognizable channel-block pattern makes deinterleaving testable.
    for (std::uint64_t block = 0; block < blocks; ++block) {
        for (std::uint32_t channel = 0; channel < options.channel_count; ++channel) {
            for (std::uint32_t byte = 0; byte < options.block_size; ++byte) {
                const auto marker = static_cast<std::uint8_t>(
                    1U + channel * 16U + static_cast<std::uint32_t>(block) * options.block_size + byte);
                chunk.push_back(static_cast<std::byte>(marker));
            }
        }
    }
    return chunk;
}

std::vector<std::byte> make_fixture(FixtureOptions options = {}) {
    const auto format = make_format_chunk(options);
    const auto data = make_data_chunk(options);

    std::vector<std::byte> bytes;
    for (const char value : std::array{'D', 'S', 'D', ' '}) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    append_u64_le(bytes, 28U);
    append_u64_le(bytes, 0U); // patched with the total size below
    append_u64_le(bytes, 0U); // patched with the ID3 pointer when present

    if (options.data_before_format) {
        bytes.insert(bytes.end(), data.begin(), data.end());
        bytes.insert(bytes.end(), format.begin(), format.end());
    } else {
        bytes.insert(bytes.end(), format.begin(), format.end());
        if (options.unknown_chunk) {
            for (const char value : std::array{'J', 'U', 'N', 'K'}) {
                bytes.push_back(static_cast<std::byte>(value));
            }
            append_u64_le(bytes, 15U);
            bytes.push_back(std::byte{0xA1});
            bytes.push_back(std::byte{0xB2});
            bytes.push_back(std::byte{0xC3});
        }
        bytes.insert(bytes.end(), data.begin(), data.end());
    }

    if (options.metadata) {
        write_u64_le(bytes, 20U, static_cast<std::uint64_t>(bytes.size()));
        for (const char value : std::array{'I', 'D', '3'}) {
            bytes.push_back(static_cast<std::byte>(value));
        }
        bytes.push_back(std::byte{4}); // ID3v2.4
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        bytes.insert(bytes.end(), 4U, std::byte{0}); // synchsafe payload size = 0
    }

    write_u64_le(bytes, 12U, static_cast<std::uint64_t>(bytes.size()));
    return bytes;
}

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

bool expect_error(
    std::vector<std::byte> bytes,
    dsf::ErrorCode expected,
    std::string_view label) {
    const auto result = dsf::parse(bytes);
    return check(!result && result.error().code == expected, label);
}

bool test_valid_file_and_partial_blocks() {
    auto bytes = make_fixture();
    const auto parsed = dsf::parse(bytes);
    if (!check(parsed.has_value(), "valid DSF parses")) {
        return false;
    }
    if (!check(parsed->format.channel_count == 2U, "channel count parsed")) {
        return false;
    }
    if (!check(parsed->format.sample_rate_hz == 2'822'400U, "sample rate parsed")) {
        return false;
    }
    if (!check(parsed->format.sample_count_per_channel == 48U, "sample count parsed")) {
        return false;
    }
    if (!check(parsed->format.bit_order() == dsf::DsdBitOrder::least_significant_bit_first,
               "DSF bit order is preserved")) {
        return false;
    }

    std::array<std::byte, 8> channel_bytes{};
    const auto read = parsed->read_channel_bytes(0U, 0U, channel_bytes);
    if (!check(read && *read == 6U, "partial final channel block omits padding")) {
        return false;
    }
    constexpr std::array<std::uint8_t, 6> expected{1U, 2U, 3U, 4U, 5U, 6U};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!check(std::to_integer<std::uint8_t>(channel_bytes[index]) == expected[index],
                   "channel zero is deinterleaved across blocks")) {
            return false;
        }
    }

    std::array<std::byte, 6> second_channel{};
    const auto second_read = parsed->read_channel_bytes(1U, 0U, second_channel);
    if (!check(second_read && *second_read == second_channel.size(), "second channel reads")) {
        return false;
    }
    constexpr std::array<std::uint8_t, 6> expected_second{17U, 18U, 19U, 20U, 21U, 22U};
    for (std::size_t index = 0; index < expected_second.size(); ++index) {
        if (!check(std::to_integer<std::uint8_t>(second_channel[index]) == expected_second[index],
                   "channel one is deinterleaved across blocks")) {
            return false;
        }
    }
    return true;
}

bool test_unknown_chunk_and_metadata() {
    FixtureOptions options;
    options.unknown_chunk = true;
    options.metadata = true;
    auto bytes = make_fixture(options);
    const auto parsed = dsf::parse(bytes);
    return check(parsed.has_value(), "well-sized unknown chunk and trailing ID3 tag are accepted")
        && check(parsed->id3_metadata.has_value(), "metadata span is exposed")
        && check(parsed->id3_metadata->size() == 10U, "metadata tag bounds are validated");
}

bool test_truncated_and_identifiers() {
    auto bytes = make_fixture();
    if (!expect_error(
            std::vector<std::byte>(bytes.begin(), bytes.begin() + 27),
            dsf::ErrorCode::truncated_input,
            "truncated DSF header rejected")) {
        return false;
    }

    bytes[0] = std::byte{'X'};
    if (!expect_error(bytes, dsf::ErrorCode::invalid_identifier, "invalid DSF identifier rejected")) {
        return false;
    }

    bytes = make_fixture();
    bytes[28] = std::byte{'x'};
    if (!expect_error(bytes, dsf::ErrorCode::invalid_chunk_order, "audio without preceding fmt rejected")) {
        return false;
    }

    bytes = make_fixture();
    bytes.resize(35U);
    write_u64_le(bytes, 12U, static_cast<std::uint64_t>(bytes.size()));
    if (!expect_error(bytes, dsf::ErrorCode::truncated_input, "truncated chunk header rejected")) {
        return false;
    }

    bytes = make_fixture();
    bytes.resize(28U);
    write_u64_le(bytes, 12U, 28U);
    return expect_error(bytes, dsf::ErrorCode::missing_required_chunk, "missing required chunks rejected");
}

bool test_invalid_format_fields() {
    auto bytes = make_fixture();
    constexpr std::size_t format_payload_offset = 28U + 12U;
    write_u32_le(bytes, format_payload_offset + 16U, 1234U);
    if (!expect_error(bytes, dsf::ErrorCode::invalid_sample_rate, "invalid sample rate rejected")) {
        return false;
    }

    FixtureOptions options;
    options.channel_count = 0U;
    bytes = make_fixture(options);
    if (!expect_error(bytes, dsf::ErrorCode::invalid_channel_layout, "invalid channel count rejected")) {
        return false;
    }

    bytes = make_fixture();
    write_u32_le(bytes, format_payload_offset + 20U, 2U);
    return expect_error(bytes,
                        dsf::ErrorCode::unsupported_bits_per_sample,
                        "unsupported bit depth rejected");
}

bool test_sizes_pointers_and_overflow_attempts() {
    auto bytes = make_fixture();
    write_u64_le(bytes, 12U, static_cast<std::uint64_t>(bytes.size()) + 1U);
    if (!expect_error(bytes, dsf::ErrorCode::invalid_file_size, "incorrect file size rejected")) {
        return false;
    }

    FixtureOptions options;
    options.metadata = true;
    bytes = make_fixture(options);
    write_u64_le(bytes, 20U, std::numeric_limits<std::uint64_t>::max());
    if (!expect_error(bytes,
                      dsf::ErrorCode::invalid_metadata_pointer,
                      "out-of-range metadata pointer rejected")) {
        return false;
    }

    write_u64_le(bytes, 20U, 1U);
    if (!expect_error(bytes,
                      dsf::ErrorCode::invalid_metadata_pointer,
                      "metadata pointer into DSF header rejected")) {
        return false;
    }

    bytes = make_fixture();
    const auto data_chunk_offset = 28U + 52U;
    bytes.pop_back();
    write_u64_le(bytes, 12U, static_cast<std::uint64_t>(bytes.size()));
    write_u64_le(bytes, data_chunk_offset + 4U, 27U);
    if (!expect_error(bytes, dsf::ErrorCode::data_size_mismatch, "short DSD payload rejected")) {
        return false;
    }

    bytes = make_fixture();
    write_u64_le(bytes, 28U + 4U, std::numeric_limits<std::uint64_t>::max());
    if (!expect_error(bytes,
                      dsf::ErrorCode::chunk_out_of_bounds,
                      "maximum chunk size cannot wrap past file end")) {
        return false;
    }

    bytes = make_fixture();
    write_u64_le(bytes, data_chunk_offset + 4U, std::numeric_limits<std::uint64_t>::max());
    return expect_error(bytes,
                        dsf::ErrorCode::chunk_out_of_bounds,
                        "maximum audio chunk size cannot wrap past file end");
}

bool test_chunk_order_and_channel_bounds() {
    FixtureOptions options;
    options.data_before_format = true;
    auto bytes = make_fixture(options);
    if (!expect_error(bytes, dsf::ErrorCode::invalid_chunk_order, "data before fmt rejected")) {
        return false;
    }

    bytes = make_fixture();
    const auto parsed = dsf::parse(bytes);
    if (!check(parsed.has_value(), "fixture parses before channel bounds checks")) {
        return false;
    }
    std::array<std::byte, 1> one_byte{};
    const auto bad_channel = parsed->read_channel_bytes(2U, 0U, one_byte);
    if (!check(!bad_channel && bad_channel.error().code == dsf::ErrorCode::invalid_channel_index,
               "out-of-range channel rejected")) {
        return false;
    }
    const auto bad_offset = parsed->read_channel_bytes(0U, 7U, one_byte);
    return check(!bad_offset && bad_offset.error().code == dsf::ErrorCode::channel_offset_out_of_range,
                 "out-of-range byte offset rejected");
}

} // namespace

int main() {
    const bool passed = test_valid_file_and_partial_blocks()
        && test_unknown_chunk_and_metadata()
        && test_truncated_and_identifiers()
        && test_invalid_format_fields()
        && test_sizes_pointers_and_overflow_attempts()
        && test_chunk_order_and_channel_bounds();
    if (!passed) {
        return EXIT_FAILURE;
    }
    std::cout << "All DSF parser tests passed.\n";
    return EXIT_SUCCESS;
}
