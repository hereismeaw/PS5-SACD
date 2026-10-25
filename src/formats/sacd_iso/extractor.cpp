#include "ps5_sacd/formats/sacd_iso/extractor.hpp"

#include "ps5_sacd/audio/dsdiff_writer.hpp"
#include "ps5_sacd/formats/sacd_iso/audio_frames.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>

namespace ps5_sacd::formats::sacd_iso {
namespace {

constexpr std::uint32_t kSacdDsd64RateHz = 2'822'400U;

struct WriterContext {
    audio::DsdiffWriter* writer{};
    std::uint64_t frame_count{};
    bool write_failed{};
};

bool write_frame(
    std::span<const std::byte> frame,
    FrameTimecode,
    void* context) {
    auto& output = *static_cast<WriterContext*>(context);
    const auto written = output.writer->write_audio(frame);
    if (!written) {
        output.write_failed = true;
        return false;
    }
    ++output.frame_count;
    return true;
}

[[nodiscard]] ExtractionError writer_error(std::uint32_t lsn = 0U) noexcept {
    return ExtractionError{ExtractionErrorCode::output_error, lsn};
}

} // namespace

std::string_view message(ExtractionErrorCode code) noexcept {
    switch (code) {
    case ExtractionErrorCode::invalid_image:
        return "SACD image is invalid";
    case ExtractionErrorCode::area_not_found:
        return "requested audio area is not present";
    case ExtractionErrorCode::invalid_track_number:
        return "track number is outside the selected area";
    case ExtractionErrorCode::unsupported_sample_frequency:
        return "only DSD64 SACD sample-frequency code 4 is supported for extraction";
    case ExtractionErrorCode::unsupported_frame_format:
        return "only fixed plain-DSD frame formats 2 and 3 are supported";
    case ExtractionErrorCode::dst_not_supported:
        return "DST decoding is not implemented";
    case ExtractionErrorCode::unsupported_channel_layout:
        return "only standard 2, 5, or 6 channel SACD layouts are supported";
    case ExtractionErrorCode::sector_frame_error:
        return "audio sector could not be reassembled into a complete DSD frame";
    case ExtractionErrorCode::output_error:
        return "could not write a valid DSDIFF output";
    }
    return "unknown SACD extraction error";
}

std::expected<ExtractionInfo, ExtractionError> extract_track_to_dsdiff(
    std::span<const std::byte> image,
    AreaKind area_kind,
    std::uint32_t track_number,
    std::ostream& output) {
    const auto image_metadata = parse(image);
    if (!image_metadata) {
        return std::unexpected(ExtractionError{
            ExtractionErrorCode::invalid_image,
            0U,
            image_metadata.error().byte_offset});
    }

    const AudioArea* area = nullptr;
    for (std::size_t index = 0; index < image_metadata->area_count; ++index) {
        if (image_metadata->areas[index].kind == area_kind) {
            area = &image_metadata->areas[index];
            break;
        }
    }
    if (area == nullptr) {
        return std::unexpected(ExtractionError{ExtractionErrorCode::area_not_found});
    }
    if (track_number == 0U || track_number > area->track_count) {
        return std::unexpected(ExtractionError{ExtractionErrorCode::invalid_track_number});
    }
    if (area->frame_format_code == 0U) {
        return std::unexpected(ExtractionError{ExtractionErrorCode::dst_not_supported});
    }
    if (area->frame_format_code != 2U && area->frame_format_code != 3U) {
        return std::unexpected(ExtractionError{ExtractionErrorCode::unsupported_frame_format});
    }
    if (area->sample_frequency_code != 4U) {
        return std::unexpected(ExtractionError{
            ExtractionErrorCode::unsupported_sample_frequency});
    }
    std::array<::ps5_sacd::formats::dsdiff::ChannelId, 6> channel_ids{};
    std::uint16_t channel_count = 0U;
    std::uint16_t loudspeaker_configuration = 0U;
    if (area->channel_count == 2U) {
        channel_ids[0] = {'S', 'L', 'F', 'T'};
        channel_ids[1] = {'S', 'R', 'G', 'T'};
        channel_count = 2U;
    } else if (area->channel_count == 5U) {
        channel_ids[0] = {'M', 'L', 'F', 'T'};
        channel_ids[1] = {'M', 'R', 'G', 'T'};
        channel_ids[2] = {'C', ' ', ' ', ' '};
        channel_ids[3] = {'L', 'S', ' ', ' '};
        channel_ids[4] = {'R', 'S', ' ', ' '};
        channel_count = 5U;
        loudspeaker_configuration = 3U;
    } else if (area->channel_count == 6U) {
        channel_ids[0] = {'M', 'L', 'F', 'T'};
        channel_ids[1] = {'M', 'R', 'G', 'T'};
        channel_ids[2] = {'C', ' ', ' ', ' '};
        channel_ids[3] = {'L', 'F', 'E', ' '};
        channel_ids[4] = {'L', 'S', ' ', ' '};
        channel_ids[5] = {'R', 'S', ' ', ' '};
        channel_count = 6U;
        loudspeaker_configuration = 4U;
    } else {
        return std::unexpected(ExtractionError{
            ExtractionErrorCode::unsupported_channel_layout});
    }

    auto writer_result = audio::DsdiffWriter::create(
        output,
        kSacdDsd64RateHz,
        std::span<const ::ps5_sacd::formats::dsdiff::ChannelId>(channel_ids)
            .first(channel_count),
        loudspeaker_configuration);
    if (!writer_result) {
        return std::unexpected(writer_error());
    }
    auto writer = std::move(*writer_result);
    WriterContext writer_context{&writer};
    AudioFrameAssembler frame_assembler(area->channel_count);
    const auto& extent = area->tracks[track_number - 1U];
    const auto end_lsn_exclusive = extent.end_lsn_exclusive();
    for (std::uint64_t current_lsn = extent.start_lsn;
         current_lsn < end_lsn_exclusive;
         ++current_lsn) {
        const auto byte_offset = static_cast<std::size_t>(current_lsn)
            * logical_sector_size;
        const auto sector = image.subspan(byte_offset, logical_sector_size);
        const auto processed = frame_assembler.process_sector(
            sector,
            write_frame,
            &writer_context);
        if (!processed) {
            if (writer_context.write_failed) {
                return std::unexpected(writer_error(static_cast<std::uint32_t>(current_lsn)));
            }
            return std::unexpected(ExtractionError{
                ExtractionErrorCode::sector_frame_error,
                static_cast<std::uint32_t>(current_lsn),
                processed.error().byte_offset,
                processed.error().code});
        }
    }

    const auto flushed = frame_assembler.finish(write_frame, &writer_context);
    if (!flushed) {
        if (writer_context.write_failed) {
            return std::unexpected(writer_error(extent.start_lsn));
        }
        return std::unexpected(ExtractionError{
            ExtractionErrorCode::sector_frame_error,
            extent.start_lsn,
            flushed.error().byte_offset,
            flushed.error().code});
    }

    const auto finalized = writer.finish();
    if (!finalized) {
        return std::unexpected(writer_error());
    }

    return ExtractionInfo{
        track_number,
        extent.start_lsn,
        extent.length_lsn,
        writer_context.frame_count,
        writer.audio_byte_count(),
    };
}

} // namespace ps5_sacd::formats::sacd_iso
