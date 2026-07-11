#pragma once

#include "RawStereoNvencRecorder.hpp"
#include "RenderedFrameMetadataLogger.hpp"
#include "StereoDisplayTextureRing.hpp"

#include <IC4Ext/IC4Ext.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace DualIC4Varjo {

class RawStereoRecordingManager final {
public:
    static RawStereoRecordingManager& instance()
    {
        static RawStereoRecordingManager value;
        return value;
    }

    void setCore(std::shared_ptr<D3D12CoreLib::D3D12Core> core)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        core_ = std::move(core);
    }

    void registerSync(
        const void* owner,
        std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> output)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeSyncs_.erase(
            std::remove_if(
                activeSyncs_.begin(),
                activeSyncs_.end(),
                [&](const SyncBinding& item) { return item.owner == owner; }),
            activeSyncs_.end());
        activeSyncs_.push_back({owner, std::move(output)});
        updateAdditionalSyncLocked();
    }

    void unregisterSync(const void* owner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeSyncs_.erase(
            std::remove_if(
                activeSyncs_.begin(),
                activeSyncs_.end(),
                [&](const SyncBinding& item) { return item.owner == owner; }),
            activeSyncs_.end());
        updateAdditionalSyncLocked();
    }

    bool start(const std::filesystem::path& renderedMetadataPath)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_) return true;
        lastError_.clear();
        if (!core_) {
            lastError_ = "raw recorder could not obtain D3D12Core";
            return false;
        }
        if (!additionalSyncOutput_) {
            lastError_ =
                "raw recorder could not find the post-calibration third SyncThread";
            return false;
        }

        std::string base = renderedMetadataPath.stem().string();
        constexpr const char* suffix = "_rendered_frames";
        const std::size_t suffixLength = std::char_traits<char>::length(suffix);
        if (base.size() >= suffixLength &&
            base.compare(base.size() - suffixLength, suffixLength, suffix) == 0) {
            base.resize(base.size() - suffixLength);
        }
        if (base.empty()) base = "recording";

        additionalSyncOutput_->clear();
        RawStereoNvencRecorderConfig config;
        config.core = core_;
        config.inputQueue = additionalSyncOutput_;
        config.outputDirectory = renderedMetadataPath.parent_path();
        config.baseFilename = base;
        config.frameRate = commandLineFrameRate();
        config.timestampSource = commandLineTimestampSource();
        config.constantQp = 18;
        config.maximumPendingGpuPairs = 32;
        config.remuxToMp4 = true;

        recorder_ = std::make_unique<RawStereoNvencRecorder>(std::move(config));
        if (!recorder_->start()) {
            lastError_ = recorder_->lastError();
            recorder_.reset();
            return false;
        }
        return true;
    }

    void stop() noexcept
    {
        std::unique_ptr<RawStereoNvencRecorder> recorder;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recorder = std::move(recorder_);
        }
        if (!recorder) return;
        recorder->stopAndJoin();
        try {
            recorder->rethrowWorkerExceptionIfAny();
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = exception.what();
            std::cerr << "[RAW_NVENC] error: " << lastError_ << '\n';
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "unknown raw recorder shutdown failure";
            std::cerr << "[RAW_NVENC] error: " << lastError_ << '\n';
        }
        std::cout
            << "[RAW_NVENC] receivedPairs=" << recorder->receivedPairCount()
            << " writtenPairs=" << recorder->writtenPairCount()
            << " failedPairs=" << recorder->failedPairCount() << '\n'
            << "[RAW_NVENC] leftVideo=" << recorder->leftVideoPath().string() << '\n'
            << "[RAW_NVENC] rightVideo=" << recorder->rightVideoPath().string() << '\n'
            << "[RAW_NVENC] leftMetadata="
            << recorder->leftMetadataPath().string() << '\n'
            << "[RAW_NVENC] rightMetadata="
            << recorder->rightMetadataPath().string() << '\n';
    }

    void rethrowWorkerExceptionIfAny() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_) recorder_->rethrowWorkerExceptionIfAny();
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_ && !recorder_->lastError().empty()) {
            return recorder_->lastError();
        }
        return lastError_;
    }

private:
    struct SyncBinding {
        const void* owner = nullptr;
        std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> output;
    };

    static std::vector<std::wstring> commandLineArguments()
    {
        int count = 0;
        LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!values) return {};
        std::vector<std::wstring> result;
        result.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            result.emplace_back(values[index] ? values[index] : L"");
        }
        LocalFree(values);
        return result;
    }

    static double commandLineFrameRate()
    {
        const auto args = commandLineArguments();
        for (std::size_t index = 0; index + 1 < args.size(); ++index) {
            if (args[index] != L"--fps") continue;
            try {
                std::size_t consumed = 0;
                const double value = std::stod(args[index + 1], &consumed);
                if (consumed == args[index + 1].size() &&
                    std::isfinite(value) && value > 0.0) {
                    return value;
                }
            } catch (...) {
            }
        }
        return 160.0;
    }

    static IC4Ext::FrameSyncTimestampSource commandLineTimestampSource()
    {
        const auto args = commandLineArguments();
        for (std::size_t index = 0; index + 1 < args.size(); ++index) {
            if (args[index] != L"--sync-timestamp") continue;
            if (args[index + 1] == L"device") {
                return IC4Ext::FrameSyncTimestampSource::Device;
            }
            if (args[index + 1] == L"auto") {
                return IC4Ext::FrameSyncTimestampSource::Auto;
            }
            return IC4Ext::FrameSyncTimestampSource::HostReceived;
        }
        return IC4Ext::FrameSyncTimestampSource::HostReceived;
    }

    void updateAdditionalSyncLocked()
    {
        // The first two active SyncThreads are always Varjo and ImGui. During
        // calibration the third is temporary and is removed before recording
        // starts. At RenderedFrameMetadataLogger::start(), the active third
        // thread is therefore the post-calibration raw-recording pipeline.
        additionalSyncOutput_ = activeSyncs_.size() > 2
            ? activeSyncs_.back().output
            : nullptr;
    }

    mutable std::mutex mutex_;
    std::shared_ptr<D3D12CoreLib::D3D12Core> core_;
    std::vector<SyncBinding> activeSyncs_;
    std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> additionalSyncOutput_;
    std::unique_ptr<RawStereoNvencRecorder> recorder_;
    std::string lastError_;
};

class RecordingStereoDisplayTextureRing final {
public:
    using UploadResult = StereoDisplayTextureRing::UploadResult;

    RecordingStereoDisplayTextureRing(
        std::shared_ptr<D3D12CoreLib::D3D12Core> core,
        VarjoXR::Backends::D3D12::D3D12Backend& backend,
        std::size_t slotCount)
        : inner_(core, backend, slotCount)
    {
        RawStereoRecordingManager::instance().setCore(std::move(core));
    }

    UploadResult upload(
        const IC4Ext::D3D12CameraFrame& left,
        const IC4Ext::D3D12CameraFrame& right)
    {
        return inner_.upload(left, right);
    }

    void markRendered(std::size_t slotIndex) { inner_.markRendered(slotIndex); }
    void waitIdle() { inner_.waitIdle(); }

private:
    StereoDisplayTextureRing inner_;
};

class RecordingRenderedFrameMetadataLogger final {
public:
    bool start(const std::filesystem::path& path)
    {
        if (!inner_.start(path)) return false;
        if (!RawStereoRecordingManager::instance().start(path)) {
            integrationError_ = RawStereoRecordingManager::instance().lastError();
            inner_.stop();
            return false;
        }
        return true;
    }

    bool enqueue(RenderedFrameMetadataRow row)
    {
        RawStereoRecordingManager::instance().rethrowWorkerExceptionIfAny();
        return inner_.enqueue(std::move(row));
    }

    void stop()
    {
        RawStereoRecordingManager::instance().stop();
        inner_.stop();
    }

    std::string lastError() const
    {
        if (!integrationError_.empty()) return integrationError_;
        const std::string recorderError =
            RawStereoRecordingManager::instance().lastError();
        return recorderError.empty() ? inner_.lastError() : recorderError;
    }

private:
    RenderedFrameMetadataLogger inner_;
    std::string integrationError_;
};

} // namespace DualIC4Varjo

namespace IC4Ext {

class RecordingD3D12FrameSyncThread final {
public:
    RecordingD3D12FrameSyncThread(
        std::shared_ptr<D3D12IndexedFrameQueue> inputQueue,
        std::shared_ptr<D3D12SyncedFrameQueue> outputQueue,
        FrameSyncOptions options = {})
        : outputQueue_(outputQueue)
        , inner_(
              std::move(inputQueue),
              std::move(outputQueue),
              std::move(options))
    {
    }

    ~RecordingD3D12FrameSyncThread() { stopAndJoin(); }

    RecordingD3D12FrameSyncThread(
        const RecordingD3D12FrameSyncThread&) = delete;
    RecordingD3D12FrameSyncThread& operator=(
        const RecordingD3D12FrameSyncThread&) = delete;

    bool start()
    {
        if (started_) return true;
        if (!inner_.start()) return false;
        started_ = true;
        DualIC4Varjo::RawStereoRecordingManager::instance().registerSync(
            this,
            outputQueue_);
        return true;
    }

    void requestStop() { inner_.requestStop(); }

    void join()
    {
        inner_.join();
        unregister();
    }

    void stopAndJoin()
    {
        inner_.stopAndJoin();
        unregister();
    }

    FrameSyncStats stats() const { return inner_.stats(); }
    const ErrorInfo& lastError() const noexcept { return inner_.lastError(); }

private:
    void unregister()
    {
        if (!started_) return;
        DualIC4Varjo::RawStereoRecordingManager::instance().unregisterSync(this);
        started_ = false;
    }

    std::shared_ptr<D3D12SyncedFrameQueue> outputQueue_;
    D3D12FrameSyncThread inner_;
    bool started_ = false;
};

} // namespace IC4Ext
