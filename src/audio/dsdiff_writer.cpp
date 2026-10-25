#include "ps5_sacd/audio/dsdiff_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <string_view>

namespace ps5_sacd::audio {
namespace {

constexpr std::string_view kCompressionName = "not compressed";

void write_ascii(std::ostream& output, std::string_view text) {
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void write_u16_be(std::ostream& output, std::uint16_t value) {
    const std::array<char, 2> bytes{
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>(value & 0xFFU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32_be(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>((value >> 24U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>(value & 0xFFU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u64_be(std::ostream& output, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned int>((bytes.size() - index - 1U) * 8U);
        bytes[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::streamoff current_offset(std::ostream& output) {
    const auto position = output.tellp();
    if (position == std::ostream::pos_type(-1)) {
        return -1;
    }
    return static_cast<std::streamoff>(position);
}

[[nodiscard]] bool write_chunk_header(
    std::ostream& output,
    std::string_view id,
    std::uint64_t payload_size) {
    write_ascii(output, id);
    write_u64_be(output, payload_size);
    return static_cast<bool>(output);
}

} // namespace

DsdiffWriter::DsdiffWriter(
    std::ostream& output,
    std::uint16_t channel_count,
    std::streamoff form_size_offset,
    std::streamoff data_size_offset,
    std::streamoff data_start_offset) noexcept
    : output_(&output),
      channel_count_(channel_count),
      form_size_offset_(form_size_offset),
      data_size_offset_(data_size_offset),
      data_start_offset_(data_start_offset) {}

std::expected<DsdiffWriter, DsdiffWriterError> DsdiffWriter::create(
    std::ostream& output,
    std::uint32_t sample_rate_hz,
    std::span<const formats::dsdiff::ChannelId> channel_ids,
    std::optional<std::uint16_t> loudspeaker_configuration) {
    if (channel_ids.empty() || channel_ids.size() > 255U || current_offset(output) != 0) {
        return std::unexpected(DsdiffWriterError{
            channel_ids.empty() || channel_ids.size() > 255U
                ? DsdiffWriterErrorCode::invalid_channel_layout
                : DsdiffWriterErrorCode::stream_error});
    }

    const auto channel_count = static_cast<std::uint16_t>(channel_ids.size());
    write_ascii(output, "FRM8");
    const auto form_size_offset = current_offset(output);
    write_u64_be(output, 0U);
    write_ascii(output, "DSD ");

    if (!write_chunk_header(output, "FVER", 4U)) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    write_u32_be(output, 0x01050000U);

    write_ascii(output, "PROP");
    const auto property_size_offset = current_offset(output);
    write_u64_be(output, 0U);
    const auto property_payload_offset = current_offset(output);
    write_ascii(output, "SND ");

    if (!write_chunk_header(output, "FS  ", 4U)) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    write_u32_be(output, sample_rate_hz);

    const auto channel_payload_size = 2U + channel_ids.size() * 4U;
    if (!write_chunk_header(output, "CHNL", channel_payload_size)) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    write_u16_be(output, channel_count);
    for (const auto& channel_id : channel_ids) {
        for (const auto character : channel_id) {
            output.put(character);
        }
    }

    constexpr auto compression_payload_size = 4U + 1U + kCompressionName.size();
    if (!write_chunk_header(output, "CMPR", compression_payload_size)) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    write_ascii(output, "DSD ");
    output.put(static_cast<char>(kCompressionName.size()));
    write_ascii(output, kCompressionName);
    if ((compression_payload_size & 1U) != 0U) {
        output.put('\0');
    }

    if (loudspeaker_configuration) {
        if (!write_chunk_header(output, "LSCO", 2U)) {
            return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
        }
        write_u16_be(output, *loudspeaker_configuration);
    }

    const auto property_end_offset = current_offset(output);
    if (!output || property_payload_offset < 0 || property_end_offset < property_payload_offset) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    const auto property_size = static_cast<std::uint64_t>(
        property_end_offset - property_payload_offset);
    const auto restore_position = current_offset(output);
    output.seekp(property_size_offset, std::ios::beg);
    write_u64_be(output, property_size);
    output.seekp(restore_position, std::ios::beg);

    if (!output) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    write_ascii(output, "DSD ");
    const auto data_size_offset = current_offset(output);
    write_u64_be(output, 0U);
    const auto data_start_offset = current_offset(output);
    if (!output || form_size_offset < 0 || data_size_offset < 0 || data_start_offset < 0) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }

    return DsdiffWriter(
        output,
        channel_count,
        form_size_offset,
        data_size_offset,
        data_start_offset);
}

std::expected<void, DsdiffWriterError> DsdiffWriter::write_audio(
    std::span<const std::byte> interleaved_dsd) {
    if (finished_ || output_ == nullptr) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::already_finalized});
    }
    if (interleaved_dsd.size() % channel_count_ != 0U) {
        return std::unexpected(DsdiffWriterError{
            DsdiffWriterErrorCode::invalid_audio_block_size});
    }
    if (interleaved_dsd.empty()) {
        return {};
    }
    if (interleaved_dsd.size()
        > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::size_overflow});
    }
    if (interleaved_dsd.size()
        > std::numeric_limits<std::uint64_t>::max() - audio_byte_count_) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::size_overflow});
    }

    output_->write(
        reinterpret_cast<const char*>(interleaved_dsd.data()),
        static_cast<std::streamsize>(interleaved_dsd.size()));
    if (!*output_) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    audio_byte_count_ += static_cast<std::uint64_t>(interleaved_dsd.size());
    return {};
}

std::expected<void, DsdiffWriterError> DsdiffWriter::finish() {
    if (finished_ || output_ == nullptr) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::already_finalized});
    }
    if (audio_byte_count_ == 0U) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::empty_audio});
    }

    const auto audio_end_offset = current_offset(*output_);
    if (audio_end_offset < data_start_offset_) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    if (audio_byte_count_ > std::numeric_limits<std::uint64_t>::max()
            - static_cast<std::uint64_t>(data_start_offset_)) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::size_overflow});
    }
    const auto expected_end = static_cast<std::uint64_t>(data_start_offset_)
        + audio_byte_count_;
    if (static_cast<std::uint64_t>(audio_end_offset) != expected_end) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }

    if ((audio_byte_count_ & 1U) != 0U) {
        output_->put('\0');
    }
    const auto final_end_offset = current_offset(*output_);
    if (final_end_offset < 12) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    const auto form_size = static_cast<std::uint64_t>(final_end_offset - 12);

    if (!patch_u64(data_size_offset_, audio_byte_count_)
        || !patch_u64(form_size_offset_, form_size)) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }

    output_->seekp(final_end_offset, std::ios::beg);
    if (!*output_) {
        return std::unexpected(DsdiffWriterError{DsdiffWriterErrorCode::stream_error});
    }
    finished_ = true;
    return {};
}

bool DsdiffWriter::patch_u64(std::streamoff offset, std::uint64_t value) {
    const auto restore_position = current_offset(*output_);
    if (restore_position < 0) {
        return false;
    }
    output_->seekp(offset, std::ios::beg);
    write_u64_be(*output_, value);
    output_->seekp(restore_position, std::ios::beg);
    return static_cast<bool>(*output_);
}

const char* message(DsdiffWriterErrorCode code) noexcept {
    switch (code) {
    case DsdiffWriterErrorCode::invalid_channel_layout:
        return "DSDIFF requires between 1 and 255 channel identifiers";
    case DsdiffWriterErrorCode::invalid_audio_block_size:
        return "interleaved DSD block is not aligned to the channel count";
    case DsdiffWriterErrorCode::empty_audio:
        return "cannot finalize a DSDIFF file without audio data";
    case DsdiffWriterErrorCode::stream_error:
        return "DSDIFF output stream failed or does not support seeking";
    case DsdiffWriterErrorCode::size_overflow:
        return "DSDIFF chunk size exceeds supported limits";
    case DsdiffWriterErrorCode::already_finalized:
        return "DSDIFF writer has already been finalized";
    }
    return "unknown DSDIFF writer error";
}

} // namespace ps5_sacd::audio
