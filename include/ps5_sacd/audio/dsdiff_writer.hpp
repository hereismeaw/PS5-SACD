#pragma once

#include "ps5_sacd/formats/dsdiff/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ostream>
#include <span>

namespace ps5_sacd::audio {

enum class DsdiffWriterErrorCode {
    invalid_channel_layout,
    invalid_audio_block_size,
    empty_audio,
    stream_error,
    size_overflow,
    already_finalized,
};

struct DsdiffWriterError {
    DsdiffWriterErrorCode code;
};

[[nodiscard]] const char* message(DsdiffWriterErrorCode code) noexcept;

// Writes interleaved, most-significant-bit-first DSD bytes to a DSDIFF 1.5
// file. The output stream must support seeking so chunk sizes can be finalized.
class DsdiffWriter {
public:
    DsdiffWriter(const DsdiffWriter&) = delete;
    DsdiffWriter& operator=(const DsdiffWriter&) = delete;
    DsdiffWriter(DsdiffWriter&&) noexcept = default;
    DsdiffWriter& operator=(DsdiffWriter&&) noexcept = default;

    [[nodiscard]] static std::expected<DsdiffWriter, DsdiffWriterError> create(
        std::ostream& output,
        std::uint32_t sample_rate_hz,
        std::span<const formats::dsdiff::ChannelId> channel_ids,
        std::optional<std::uint16_t> loudspeaker_configuration = std::nullopt);

    [[nodiscard]] std::expected<void, DsdiffWriterError> write_audio(
        std::span<const std::byte> interleaved_dsd);

    [[nodiscard]] std::expected<void, DsdiffWriterError> finish();

    [[nodiscard]] std::uint64_t audio_byte_count() const noexcept {
        return audio_byte_count_;
    }

private:
    DsdiffWriter(
        std::ostream& output,
        std::uint16_t channel_count,
        std::streamoff form_size_offset,
        std::streamoff data_size_offset,
        std::streamoff data_start_offset) noexcept;

    [[nodiscard]] bool patch_u64(std::streamoff offset, std::uint64_t value);

    std::ostream* output_{};
    std::uint16_t channel_count_{};
    std::streamoff form_size_offset_{};
    std::streamoff data_size_offset_{};
    std::streamoff data_start_offset_{};
    std::uint64_t audio_byte_count_{};
    bool finished_{};
};

} // namespace ps5_sacd::audio
