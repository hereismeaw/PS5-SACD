#include "ps5_sacd/formats/sacd_iso/audio_frames.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ps5_sacd::formats::sacd_iso {
namespace {

constexpr std::size_t kSectorSize = 2048U;
constexpr std::size_t kSectorHeaderSize = 1U;
constexpr std::size_t kPacketInfoSize = 2U;
constexpr std::size_t kPlainDsdFrameInfoSize = 3U;
constexpr std::size_t kMaximumPacketCount = 7U;
constexpr std::uint8_t kAudioPacketType = 2U;
constexpr std::uint8_t kSupplementaryPacketType = 3U;
constexpr std::uint8_t kPaddingPacketType = 7U;

struct PacketInfo {
    bool frame_start{};
    std::uint8_t data_type{};
    std::size_t payload_size{};
    std::size_t payload_offset{};
};

struct SectorFrameInfo {
    FrameTimecode timecode{};
    std::size_t header_offset{};
};

[[nodiscard]] std::uint8_t byte_at(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] bool valid_packet_type(std::uint8_t type) noexcept {
    return type == kAudioPacketType
        || type == kSupplementaryPacketType
        || type == kPaddingPacketType;
}

} // namespace

AudioFrameAssembler::AudioFrameAssembler(std::uint8_t channel_count) noexcept
    : channel_count_(channel_count) {
    if (channel_count_ > 0U && channel_count_ <= maximum_audio_channels) {
        frame_size_bytes_ = static_cast<std::size_t>(channel_count_)
            * dsd64_frame_bytes_per_channel;
    }
}

std::expected<void, AudioFrameError> AudioFrameAssembler::emit_frame(
    AudioFrameSink sink,
    void* context,
    std::size_t error_offset) {
    if (frame_bytes_used_ != frame_size_bytes_) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::incomplete_frame,
            error_offset});
    }
    if (sink == nullptr || !sink(
            std::span<const std::byte>(frame_).first(frame_bytes_used_),
            current_timecode_,
            context)) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::frame_sink_failed,
            error_offset});
    }

    ++emitted_frame_count_;
    frame_active_ = false;
    frame_bytes_used_ = 0U;
    return {};
}

std::expected<void, AudioFrameError> AudioFrameAssembler::process_sector(
    std::span<const std::byte> sector,
    AudioFrameSink sink,
    void* context) {
    if (channel_count_ == 0U || channel_count_ > maximum_audio_channels) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::invalid_channel_count,
            0U});
    }
    if (sector.size() != kSectorSize) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::invalid_sector_size,
            sector.size()});
    }

    const auto header = byte_at(sector, 0U);
    if ((header & 0x01U) != 0U) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::unsupported_dst_frame,
            0U});
    }
    if ((header & 0x02U) != 0U) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::reserved_header_bit_set,
            0U});
    }

    const auto packet_count = static_cast<std::size_t>(header >> 5U);
    const auto frame_info_count = static_cast<std::size_t>((header >> 2U) & 0x07U);
    if (packet_count == 0U || packet_count > kMaximumPacketCount) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::missing_packets,
            0U});
    }

    const auto packet_info_offset = kSectorHeaderSize;
    const auto frame_info_offset = packet_info_offset + packet_count * kPacketInfoSize;
    const auto packet_payload_offset = frame_info_offset
        + frame_info_count * kPlainDsdFrameInfoSize;
    if (packet_payload_offset > sector.size()) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::truncated_packet_headers,
            packet_info_offset});
    }

    std::array<PacketInfo, kMaximumPacketCount> packets{};
    std::size_t audio_frame_start_count = 0U;
    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        const auto info_offset = packet_info_offset + packet_index * kPacketInfoSize;
        const auto first = byte_at(sector, info_offset);
        const auto type = static_cast<std::uint8_t>((first >> 3U) & 0x07U);
        const bool frame_start = (first & 0x80U) != 0U;
        if (!valid_packet_type(type)) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::invalid_packet_type,
                info_offset});
        }
        if (frame_start && type != kAudioPacketType) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::invalid_frame_start,
                info_offset});
        }
        if (frame_start) {
            ++audio_frame_start_count;
        }

        const auto payload_size = static_cast<std::size_t>(
            (static_cast<std::uint16_t>(first & 0x07U) << 8U)
            | byte_at(sector, info_offset + 1U));
        packets[packet_index] = PacketInfo{
            frame_start,
            type,
            payload_size,
            packet_payload_offset,
        };
    }

    if (audio_frame_start_count != frame_info_count) {
        return std::unexpected(AudioFrameError{
            AudioFrameErrorCode::truncated_packet_headers,
            frame_info_offset});
    }

    std::array<SectorFrameInfo, kMaximumPacketCount> frame_infos{};
    for (std::size_t frame_index = 0; frame_index < frame_info_count; ++frame_index) {
        const auto info_offset = frame_info_offset + frame_index * kPlainDsdFrameInfoSize;
        const FrameTimecode timecode{
            byte_at(sector, info_offset),
            byte_at(sector, info_offset + 1U),
            byte_at(sector, info_offset + 2U),
        };
        if (timecode.seconds >= 60U || timecode.frames >= 75U) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::invalid_timecode,
                info_offset});
        }
        frame_infos[frame_index] = SectorFrameInfo{timecode, info_offset};
    }

    std::size_t payload_cursor = packet_payload_offset;
    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        auto& packet = packets[packet_index];
        packet.payload_offset = payload_cursor;
        if (packet.payload_size > sector.size() - payload_cursor) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::packet_out_of_bounds,
                packet.payload_offset});
        }
        payload_cursor += packet.payload_size;
    }

    std::size_t frame_info_index = 0U;
    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        const auto& packet = packets[packet_index];
        if (packet.data_type != kAudioPacketType) {
            continue;
        }
        if (packet.payload_size == 0U) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::packet_out_of_bounds,
                packet.payload_offset});
        }

        if (packet.frame_start) {
            if (frame_active_) {
                auto emitted = emit_frame(sink, context, packet.payload_offset);
                if (!emitted) {
                    return emitted;
                }
            }

            current_timecode_ = frame_infos[frame_info_index].timecode;
            ++frame_info_index;
            frame_active_ = true;
            frame_bytes_used_ = 0U;
        }

        if (!frame_active_) {
            continue;
        }

        if (packet.payload_size > frame_size_bytes_ - frame_bytes_used_) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::frame_size_exceeded,
                packet.payload_offset});
        }
        for (std::size_t byte_index = 0; byte_index < packet.payload_size; ++byte_index) {
            frame_[frame_bytes_used_ + byte_index] = sector[packet.payload_offset + byte_index];
        }
        frame_bytes_used_ += packet.payload_size;
    }

    return {};
}

std::expected<void, AudioFrameError> AudioFrameAssembler::finish(
    AudioFrameSink sink,
    void* context) {
    if (!frame_active_) {
        if (emitted_frame_count_ == 0U) {
            return std::unexpected(AudioFrameError{
                AudioFrameErrorCode::missing_audio_frames,
                0U});
        }
        return {};
    }
    return emit_frame(sink, context, 0U);
}

const char* message(AudioFrameErrorCode code) noexcept {
    switch (code) {
    case AudioFrameErrorCode::invalid_channel_count:
        return "audio frame assembler requires 1 through 6 channels";
    case AudioFrameErrorCode::invalid_sector_size:
        return "audio sector must be exactly 2048 bytes";
    case AudioFrameErrorCode::unsupported_dst_frame:
        return "DST-compressed audio frames are not supported";
    case AudioFrameErrorCode::reserved_header_bit_set:
        return "audio sector header has a reserved bit set";
    case AudioFrameErrorCode::missing_packets:
        return "audio sector contains no packet records";
    case AudioFrameErrorCode::truncated_packet_headers:
        return "audio sector packet or frame information is inconsistent";
    case AudioFrameErrorCode::invalid_timecode:
        return "audio frame timecode is invalid";
    case AudioFrameErrorCode::invalid_packet_type:
        return "audio sector contains an unknown packet type";
    case AudioFrameErrorCode::invalid_frame_start:
        return "frame start flag is attached to a non-audio packet";
    case AudioFrameErrorCode::packet_out_of_bounds:
        return "audio packet payload exceeds its sector";
    case AudioFrameErrorCode::frame_size_exceeded:
        return "uncompressed DSD frame exceeds its fixed frame size";
    case AudioFrameErrorCode::incomplete_frame:
        return "uncompressed DSD frame ended before all channel bytes arrived";
    case AudioFrameErrorCode::missing_audio_frames:
        return "track contains no complete uncompressed DSD frames";
    case AudioFrameErrorCode::frame_sink_failed:
        return "could not write a completed DSD frame";
    }
    return "unknown SACD audio-frame error";
}

} // namespace ps5_sacd::formats::sacd_iso
