#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ps5_sacd::formats::sacd_iso {

inline constexpr std::size_t dsd64_frame_bytes_per_channel = 4704;
inline constexpr std::size_t maximum_audio_channels = 6;

enum class AudioFrameErrorCode {
    invalid_channel_count,
    invalid_sector_size,
    unsupported_dst_frame,
    reserved_header_bit_set,
    missing_packets,
    truncated_packet_headers,
    invalid_timecode,
    invalid_packet_type,
    invalid_frame_start,
    packet_out_of_bounds,
    frame_size_exceeded,
    incomplete_frame,
    missing_audio_frames,
    frame_sink_failed,
};

struct AudioFrameError {
    AudioFrameErrorCode code;
    std::size_t byte_offset;
};

struct FrameTimecode {
    std::uint8_t minutes{};
    std::uint8_t seconds{};
    std::uint8_t frames{};
};

using AudioFrameSink = bool (*)(
    std::span<const std::byte> frame,
    FrameTimecode timecode,
    void* context);

// Reassembles fixed-size, uncompressed DSD frames from 2048-byte SACD audio
// sectors. DST-marked sectors are rejected before packet payload is consumed.
class AudioFrameAssembler {
public:
    explicit AudioFrameAssembler(std::uint8_t channel_count) noexcept;

    [[nodiscard]] std::expected<void, AudioFrameError> process_sector(
        std::span<const std::byte> sector,
        AudioFrameSink sink,
        void* context);

    [[nodiscard]] std::expected<void, AudioFrameError> finish(
        AudioFrameSink sink,
        void* context);

    [[nodiscard]] std::uint8_t channel_count() const noexcept {
        return channel_count_;
    }

    [[nodiscard]] std::size_t frame_size_bytes() const noexcept {
        return frame_size_bytes_;
    }

private:
    [[nodiscard]] std::expected<void, AudioFrameError> emit_frame(
        AudioFrameSink sink,
        void* context,
        std::size_t error_offset);

    std::array<std::byte, maximum_audio_channels * dsd64_frame_bytes_per_channel> frame_{};
    std::size_t frame_size_bytes_{};
    std::size_t frame_bytes_used_{};
    std::uint8_t channel_count_{};
    std::size_t emitted_frame_count_{};
    FrameTimecode current_timecode_{};
    bool frame_active_{};
};

[[nodiscard]] const char* message(AudioFrameErrorCode code) noexcept;

} // namespace ps5_sacd::formats::sacd_iso
