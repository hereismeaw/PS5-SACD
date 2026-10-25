#include "ps5_sacd/formats/dsdiff/parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace dsdiff = ps5_sacd::formats::dsdiff;

namespace {

using Bytes = std::vector<std::byte>;

struct FixtureOptions {
    std::vector<dsdiff::ChannelId> channels{{'S', 'L', 'F', 'T'}, {'S', 'R', 'G', 'T'}};
    std::uint32_t sample_rate{2'822'400U};
    std::optional<std::uint16_t> loudspeaker_configuration;
    dsdiff::ChannelId compression{'D', 'S', 'D', ' '};
    Bytes audio{std::byte{0x11}, std::byte{0x91}, std::byte{0x12}, std::byte{0x92}};
    bool unknown_top_chunk{};
    bool unknown_property_chunk{};
    bool version_first{true};
    bool include_properties{true};
    bool include_audio{true};
};

void append_id(Bytes& bytes, const std::array<char, 4>& id) {
    for (const auto character : id) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

void append_id(Bytes& bytes, std::string_view id) {
    for (const auto character : id) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

void append_u16_be(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32_be(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64_be(Bytes& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_chunk(Bytes& bytes, std::string_view id, const Bytes& payload) {
    append_id(bytes, id);
    append_u64_be(bytes, static_cast<std::uint64_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) {
        bytes.push_back(std::byte{0});
    }
}

Bytes make_properties(const FixtureOptions& options) {
    Bytes properties;
    append_id(properties, "SND ");

    Bytes sample_rate;
    append_u32_be(sample_rate, options.sample_rate);
    append_chunk(properties, "FS  ", sample_rate);

    Bytes channels;
    append_u16_be(channels, static_cast<std::uint16_t>(options.channels.size()));
    for (const auto& channel : options.channels) {
        append_id(channels, channel);
    }
    append_chunk(properties, "CHNL", channels);

    Bytes compression;
    append_id(compression, options.compression);
    constexpr std::string_view name = "not compressed";
    compression.push_back(static_cast<std::byte>(name.size()));
    for (const auto character : name) {
        compression.push_back(static_cast<std::byte>(character));
    }
    append_chunk(properties, "CMPR", compression);

    if (options.loudspeaker_configuration) {
        Bytes loudspeaker_configuration;
        append_u16_be(loudspeaker_configuration, *options.loudspeaker_configuration);
        append_chunk(properties, "LSCO", loudspeaker_configuration);
    }

    if (options.unknown_property_chunk) {
        append_chunk(properties, "ZZZZ", Bytes{std::byte{0x55}});
    }
    return properties;
}

Bytes make_fixture(const FixtureOptions& options = {}) {
    Bytes form;
    append_id(form, "DSD ");

    Bytes version;
    append_u32_be(version, 0x01050000U);
    if (options.version_first) {
        append_chunk(form, "FVER", version);
    }
    if (options.unknown_top_chunk) {
        append_chunk(form, "JUNK", Bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}});
    }
    if (options.include_properties) {
        append_chunk(form, "PROP", make_properties(options));
    }
    if (!options.version_first) {
        append_chunk(form, "FVER", version);
    }
    if (options.include_audio) {
        append_chunk(form, options.compression == dsdiff::ChannelId{'D', 'S', 'T', ' '}
                               ? "DST "
                               : "DSD ",
                     options.audio);
    }

    Bytes file;
    append_id(file, "FRM8");
    append_u64_be(file, static_cast<std::uint64_t>(form.size()));
    file.insert(file.end(), form.begin(), form.end());
    return file;
}

bool check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        return false;
    }
    return true;
}

bool expect_error(Bytes bytes, dsdiff::ErrorCode code, std::string_view label) {
    const auto parsed = dsdiff::parse(bytes);
    return check(!parsed && parsed.error().code == code, label);
}

bool test_valid_file_and_channel_order() {
    const auto bytes = make_fixture();
    const auto parsed = dsdiff::parse(bytes);
    if (!check(parsed.has_value(), "valid uncompressed DSDIFF parses")) {
        return false;
    }
    if (!check(parsed->sample_rate_hz == 2'822'400U, "sample rate parsed")) {
        return false;
    }
    if (!check(parsed->channel_count() == 2U, "channel count parsed")) {
        return false;
    }
    if (!check(parsed->sample_count_per_channel == 16U, "sample count excludes chunk padding")) {
        return false;
    }
    if (!check(parsed->bit_order() == ps5_sacd::formats::DsdBitOrder::most_significant_bit_first,
               "DSDIFF bit order is MSB first")) {
        return false;
    }

    std::array<std::byte, 2> left{};
    const auto left_read = parsed->read_channel_bytes(0U, 0U, left);
    if (!check(left_read && *left_read == 2U, "channel zero reads")) {
        return false;
    }
    if (!check(left[0] == std::byte{0x11} && left[1] == std::byte{0x12},
               "channel zero is deinterleaved")) {
        return false;
    }

    std::array<std::byte, 2> right{};
    const auto right_read = parsed->read_channel_bytes(1U, 0U, right);
    return check(right_read && *right_read == 2U, "channel one reads")
        && check(right[0] == std::byte{0x91} && right[1] == std::byte{0x92},
                 "channel one is deinterleaved");
}

bool test_unknown_chunks_and_odd_audio_pad() {
    FixtureOptions options;
    options.unknown_top_chunk = true;
    options.unknown_property_chunk = true;
    options.channels = {{'M', 'O', 'N', 'O'}};
    options.audio = {std::byte{0x81}, std::byte{0x42}, std::byte{0x24}};
    const auto bytes = make_fixture(options);
    const auto parsed = dsdiff::parse(bytes);
    if (!check(parsed.has_value(), "bounded unknown chunks are skipped")) {
        return false;
    }
    if (!check(parsed->audio_data.size() == 3U, "odd DSD chunk pad is excluded from audio")) {
        return false;
    }
    std::array<std::byte, 4> channel_bytes{};
    const auto read = parsed->read_channel_bytes(0U, 1U, channel_bytes);
    return check(read && *read == 2U, "last logical channel bytes read")
        && check(channel_bytes[0] == std::byte{0x42} && channel_bytes[1] == std::byte{0x24},
                 "pad byte is not returned as audio");
}

bool test_dst_is_reported_as_unsupported() {
    FixtureOptions options;
    options.compression = {'D', 'S', 'T', ' '};
    return expect_error(make_fixture(options),
                        dsdiff::ErrorCode::unsupported_compression,
                        "DST data is not reported as decoded DSD");
}

bool test_header_and_extent_errors() {
    auto bytes = make_fixture();
    bytes[0] = std::byte{'X'};
    if (!expect_error(std::move(bytes), dsdiff::ErrorCode::invalid_identifier,
                      "invalid FRM8 identifier rejected")) {
        return false;
    }

    bytes = make_fixture();
    bytes.pop_back();
    if (!expect_error(std::move(bytes), dsdiff::ErrorCode::invalid_file_size,
                      "short form rejected")) {
        return false;
    }

    bytes = make_fixture();
    bytes.resize(10U);
    return expect_error(std::move(bytes), dsdiff::ErrorCode::truncated_input,
                        "truncated FRM8 header rejected");
}

bool test_required_order_and_fields() {
    FixtureOptions options;
    options.version_first = false;
    if (!expect_error(make_fixture(options), dsdiff::ErrorCode::invalid_chunk_order,
                      "FVER must be the first local chunk")) {
        return false;
    }

    options = {};
    options.include_properties = false;
    if (!expect_error(make_fixture(options), dsdiff::ErrorCode::invalid_chunk_order,
                      "audio before PROP rejected")) {
        return false;
    }

    options = {};
    options.include_audio = false;
    return expect_error(make_fixture(options), dsdiff::ErrorCode::missing_required_chunk,
                        "missing DSD data rejected");
}

bool test_rate_layout_and_channel_bounds() {
    FixtureOptions options;
    options.sample_rate = 123'456U;
    if (!expect_error(make_fixture(options), dsdiff::ErrorCode::invalid_sample_rate,
                      "unsupported DSD rate rejected")) {
        return false;
    }

    options = {};
    options.audio = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    if (!expect_error(make_fixture(options), dsdiff::ErrorCode::invalid_audio_size,
                      "partial interleaved channel frame rejected")) {
        return false;
    }

    const auto bytes = make_fixture();
    const auto parsed = dsdiff::parse(bytes);
    if (!check(parsed.has_value(), "fixture parses before channel bounds checks")) {
        return false;
    }
    std::array<std::byte, 1> one{};
    const auto invalid_channel = parsed->read_channel_bytes(2U, 0U, one);
    if (!check(!invalid_channel && invalid_channel.error().code == dsdiff::ErrorCode::invalid_channel_index,
               "invalid channel index rejected")) {
        return false;
    }
    const auto invalid_offset = parsed->read_channel_bytes(0U, 3U, one);
    return check(!invalid_offset
                     && invalid_offset.error().code == dsdiff::ErrorCode::channel_offset_out_of_range,
                 "out-of-range channel offset rejected");
}

bool test_optional_loudspeaker_configuration() {
    FixtureOptions options;
    options.loudspeaker_configuration = 4U;
    const auto bytes = make_fixture(options);
    const auto parsed = dsdiff::parse(bytes);
    if (!check(parsed.has_value(), "optional LSCO chunk parses")) {
        return false;
    }
    return check(parsed->loudspeaker_configuration == 4U,
                 "LSCO value is retained for speaker-aware WAV output");
}

} // namespace

int main() {
    const bool passed = test_valid_file_and_channel_order()
        && test_unknown_chunks_and_odd_audio_pad()
        && test_dst_is_reported_as_unsupported()
        && test_header_and_extent_errors()
        && test_required_order_and_fields()
        && test_rate_layout_and_channel_bounds()
        && test_optional_loudspeaker_configuration();
    if (!passed) {
        return 1;
    }
    std::cout << "All DSDIFF parser tests passed.\n";
    return 0;
}
