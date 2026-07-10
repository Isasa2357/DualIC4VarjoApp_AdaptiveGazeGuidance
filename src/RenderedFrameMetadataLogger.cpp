#include "RenderedFrameMetadataLogger.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <iomanip>
#include <sstream>
#include <utility>

namespace DualIC4Varjo {
namespace {

void WriteCamera(std::ostream& out, const CameraFrameMetadataRow& camera)
{
    out << ',' << camera.frameNumber
        << ',' << camera.deviceTimestampNs
        << ',' << camera.hostReceivedUnixUs
        << ',' << camera.width
        << ',' << camera.height;
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

void RenderedFrameMetadataLogger::writeRecord(const std::string& record)
{
    if (record.empty()) return;
    output_.write(record.data(), static_cast<std::streamsize>(record.size()));
    output_.write("\r\n", 2);
}

void RenderedFrameMetadataLogger::writeHeader()
{
    writeRecord(
        "render_row_index,render_submit_unix_us,render_submit_local_iso8601,new_frame_from_queue,submit_ok,"
        "plane_moved,plane_resized,plane_placement_mode,plane_x_m,plane_y_m,plane_z_m,plane_width_m,plane_height_m,"
        "sync_group_id,sync_emitted_unix_us,sync_timestamp_source,sync_timestamp_diff_us,host_received_diff_us,display_slot_index,"
        "left_frame_number,left_device_timestamp_ns,left_host_received_unix_us,left_width,left_height,"
        "right_frame_number,right_device_timestamp_ns,right_host_received_unix_us,right_width,right_height,"
        "sync_input_frames,sync_emitted_sets,sync_dropped_frames,sync_push_failures,"
        "synced_queue_dropped_oldest,synced_queue_dropped_by_pop_latest,"
        "left_camera_read_frames,left_camera_read_timeouts,left_camera_read_errors,"
        "right_camera_read_frames,right_camera_read_timeouts,right_camera_read_errors");
}

void RenderedFrameMetadataLogger::writeRow(const RenderedFrameMetadataRow& row)
{
    std::ostringstream line;
    line << row.renderRowIndex
         << ',' << row.renderSubmitUnixUs
         << ',' << row.renderSubmitLocalIso8601
         << ',' << (row.newFrameFromQueue ? 1 : 0)
         << ',' << (row.submitOk ? 1 : 0)
         << ',' << (row.planeMoved ? 1 : 0)
         << ',' << (row.planeResized ? 1 : 0)
         << ',' << row.planePlacementMode
         << ',' << std::fixed << std::setprecision(4) << row.planeX
         << ',' << std::fixed << std::setprecision(4) << row.planeY
         << ',' << std::fixed << std::setprecision(4) << row.planeZ
         << ',' << std::fixed << std::setprecision(4) << row.planeWidth
         << ',' << std::fixed << std::setprecision(4) << row.planeHeight
         << ',' << row.syncGroupId
         << ',' << row.syncEmittedUnixUs
         << ',' << row.syncTimestampSource
         << ',' << std::fixed << std::setprecision(3)
         << (static_cast<double>(row.syncTimestampDiffNs) / 1000.0)
         << ',' << row.hostReceivedDiffUs
         << ',' << row.displaySlotIndex;

    WriteCamera(line, row.left);
    WriteCamera(line, row.right);

    line << ',' << row.syncStats.inputFrames
         << ',' << row.syncStats.emittedSets
         << ',' << row.syncStats.droppedFrames
         << ',' << row.syncStats.pushFailures
         << ',' << row.syncedQueueStats.droppedOldest
         << ',' << row.syncedQueueStats.droppedByPopLatest
         << ',' << row.leftCameraStats.readFrames
         << ',' << row.leftCameraStats.readTimeouts
         << ',' << row.leftCameraStats.readErrors
         << ',' << row.rightCameraStats.readFrames
         << ',' << row.rightCameraStats.readTimeouts
         << ',' << row.rightCameraStats.readErrors;

    writeRecord(line.str());
}

void RenderedFrameMetadataLogger::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = std::move(message);
}

} // namespace DualIC4Varjo
