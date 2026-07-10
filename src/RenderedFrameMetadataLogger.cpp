#include "RenderedFrameMetadataLogger.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <iomanip>
#include <sstream>
#include <utility>

namespace DualIC4Varjo {
namespace {

void WriteChunk(std::ostream& out, const IC4Ext::FrameChunkMetadata& chunk)
{
    out << ',' << (chunk.hasBlockId ? 1 : 0) << ',';
    if (chunk.hasBlockId) out << chunk.blockId;
    out << ',' << (chunk.hasExposureTime ? 1 : 0) << ',';
    if (chunk.hasExposureTime) out << std::fixed << std::setprecision(6) << chunk.exposureTimeUs;
    out << ',' << (chunk.hasGain ? 1 : 0) << ',';
    if (chunk.hasGain) out << std::fixed << std::setprecision(6) << chunk.gain;
    out << ',' << (chunk.hasIMX174FrameId ? 1 : 0) << ',';
    if (chunk.hasIMX174FrameId) out << chunk.imx174FrameId;
    out << ',' << (chunk.hasIMX174FrameSet ? 1 : 0) << ',';
    if (chunk.hasIMX174FrameSet) out << chunk.imx174FrameSet;
    out << ',' << (chunk.hasMultiFrameSetId ? 1 : 0) << ',';
    if (chunk.hasMultiFrameSetId) out << chunk.multiFrameSetId;
    out << ',' << (chunk.hasMultiFrameSetFrameId ? 1 : 0) << ',';
    if (chunk.hasMultiFrameSetFrameId) out << chunk.multiFrameSetFrameId;
}

void WriteCamera(std::ostream& out, const CameraFrameMetadataRow& camera)
{
    out << ',' << camera.frameNumber
        << ',' << camera.deviceTimestampNs
        << ',' << camera.hostReceivedSteadyUs
        << ',' << camera.hostReceivedUnixUs
        << ',' << camera.hostReceivedLocalIso8601
        << ',' << camera.width
        << ',' << camera.height
        << ',' << camera.dxgiFormat;
    WriteChunk(out, camera.chunk);
}

} // namespace

RenderedFrameMetadataLogger::~RenderedFrameMetadataLogger()
{
    stop();
}

std::string RenderedFrameMetadataLogger::lastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

bool RenderedFrameMetadataLogger::start(const std::filesystem::path& path)
{
    if (started_) return true;
    path_ = path;
    try {
        if (!path_.parent_path().empty()) {
            std::filesystem::create_directories(path_.parent_path());
        }
        output_.open(path_, std::ios::binary | std::ios::trunc);
        if (!output_) {
            setError("Failed to open metadata CSV: " + path_.string());
            return false;
        }
        output_.write("\xEF\xBB\xBF", 3);
        writeHeader();
        output_.flush();
        started_ = true;
        worker_ = std::thread(&RenderedFrameMetadataLogger::workerLoop, this);
        return true;
    } catch (const std::exception& e) {
        setError(e.what());
        return false;
    }
}

bool RenderedFrameMetadataLogger::enqueue(RenderedFrameMetadataRow row)
{
    if (!started_) return false;
    const auto result = queue_.push(std::move(row));
    if (!ThreadKit::Queues::isPushSucceeded(result)) {
        setError(std::string("Metadata queue push failed: ") + ThreadKit::Queues::toString(result));
        return false;
    }
    return true;
}

void RenderedFrameMetadataLogger::stop()
{
    if (!started_) return;
    queue_.close();
    if (worker_.joinable()) worker_.join();
    output_.flush();
    output_.close();
    started_ = false;
}

void RenderedFrameMetadataLogger::workerLoop()
{
    try {
        while (auto row = queue_.waitPop()) {
            writeRow(*row);
            output_.flush();
        }
    } catch (const std::exception& e) {
        setError(e.what());
    }
}

void RenderedFrameMetadataLogger::writeHeader()
{
    output_ <<
        "render_row_index,render_submit_unix_us,render_submit_local_iso8601,render_submit_steady_us,new_frame_from_queue,submit_ok,"
        "sync_group_id,sync_emitted_steady_us,sync_emitted_unix_us,sync_timestamp_source,sync_timestamp_diff_ns,sync_timestamp_diff_ms,"
        "host_received_diff_us,host_received_diff_ms,display_slot_index,"
        "left_frame_number,left_device_timestamp_ns,left_host_received_steady_us,left_host_received_unix_us,left_host_received_local_iso8601,left_width,left_height,left_dxgi_format,"
        "left_has_block_id,left_block_id,left_has_exposure_time,left_exposure_time_us,left_has_gain,left_gain,left_has_imx174_frame_id,left_imx174_frame_id,left_has_imx174_frame_set,left_imx174_frame_set,left_has_multiframe_set_id,left_multiframe_set_id,left_has_multiframe_set_frame_id,left_multiframe_set_frame_id,"
        "right_frame_number,right_device_timestamp_ns,right_host_received_steady_us,right_host_received_unix_us,right_host_received_local_iso8601,right_width,right_height,right_dxgi_format,"
        "right_has_block_id,right_block_id,right_has_exposure_time,right_exposure_time_us,right_has_gain,right_gain,right_has_imx174_frame_id,right_imx174_frame_id,right_has_imx174_frame_set,right_imx174_frame_set,right_has_multiframe_set_id,right_multiframe_set_id,right_has_multiframe_set_frame_id,right_multiframe_set_frame_id,"
        "sync_input_frames,sync_emitted_sets,sync_ignored_frames,sync_dropped_frames,sync_push_failures,"
        "synced_queue_pushed,synced_queue_popped,synced_queue_dropped_oldest,synced_queue_dropped_by_pop_latest,synced_queue_current_size,synced_queue_max_observed_size,"
        "left_camera_read_frames,left_camera_read_timeouts,left_camera_read_errors,left_camera_pushed_frames,left_camera_push_failures,"
        "right_camera_read_frames,right_camera_read_timeouts,right_camera_read_errors,right_camera_pushed_frames,right_camera_push_failures\n";
}

void RenderedFrameMetadataLogger::writeRow(const RenderedFrameMetadataRow& row)
{
    output_ << row.renderRowIndex
        << ',' << row.renderSubmitUnixUs
        << ',' << row.renderSubmitLocalIso8601
        << ',' << row.renderSubmitSteadyUs
        << ',' << (row.newFrameFromQueue ? 1 : 0)
        << ',' << (row.submitOk ? 1 : 0)
        << ',' << row.syncGroupId
        << ',' << row.syncEmittedSteadyUs
        << ',' << row.syncEmittedUnixUs
        << ',' << row.syncTimestampSource
        << ',' << row.syncTimestampDiffNs
        << ',' << std::fixed << std::setprecision(6) << row.syncTimestampDiffMs
        << ',' << row.hostReceivedDiffUs
        << ',' << std::fixed << std::setprecision(6) << row.hostReceivedDiffMs
        << ',' << row.displaySlotIndex;

    WriteCamera(output_, row.left);
    WriteCamera(output_, row.right);

    output_ << ',' << row.syncStats.inputFrames
        << ',' << row.syncStats.emittedSets
        << ',' << row.syncStats.ignoredFrames
        << ',' << row.syncStats.droppedFrames
        << ',' << row.syncStats.pushFailures
        << ',' << row.syncedQueueStats.pushed
        << ',' << row.syncedQueueStats.popped
        << ',' << row.syncedQueueStats.droppedOldest
        << ',' << row.syncedQueueStats.droppedByPopLatest
        << ',' << row.syncedQueueStats.currentSize
        << ',' << row.syncedQueueStats.maxObservedSize
        << ',' << row.leftCameraStats.readFrames
        << ',' << row.leftCameraStats.readTimeouts
        << ',' << row.leftCameraStats.readErrors
        << ',' << row.leftCameraStats.pushedFrames
        << ',' << row.leftCameraStats.pushFailures
        << ',' << row.rightCameraStats.readFrames
        << ',' << row.rightCameraStats.readTimeouts
        << ',' << row.rightCameraStats.readErrors
        << ',' << row.rightCameraStats.pushedFrames
        << ',' << row.rightCameraStats.pushFailures
        << '\n';
}

void RenderedFrameMetadataLogger::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = std::move(message);
}

} // namespace DualIC4Varjo
