#pragma once

#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace DualIC4Varjo {

struct RawStereoNvencRecorderConfig {
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> inputQueue;
    std::filesystem::path outputDirectory;
    std::string baseFilename;
    double frameRate = 160.0;
    IC4Ext::FrameSyncTimestampSource timestampSource =
        IC4Ext::FrameSyncTimestampSource::HostReceived;
    std::uint32_t constantQp = 18;
    std::size_t maximumPendingGpuPairs = 32;

    // Keep shutdown short for Varjo. The app writes raw .h264 only; MP4 remux is
    // performed offline by the user after the application has exited.
    bool remuxToMp4 = false;
};

class RawStereoNvencRecorder final {
public:
    explicit RawStereoNvencRecorder(RawStereoNvencRecorderConfig config);
    ~RawStereoNvencRecorder();

    RawStereoNvencRecorder(const RawStereoNvencRecorder&) = delete;
    RawStereoNvencRecorder& operator=(const RawStereoNvencRecorder&) = delete;

    bool start();
    void requestStop() noexcept;
    void stopAndJoin() noexcept;

    bool running() const noexcept;
    void rethrowWorkerExceptionIfAny() const;
    std::string lastError() const;

    std::uint64_t receivedPairCount() const noexcept;
    std::uint64_t writtenPairCount() const noexcept;
    std::uint64_t failedPairCount() const noexcept;

    std::filesystem::path leftVideoPath() const;
    std::filesystem::path rightVideoPath() const;
    std::filesystem::path leftMetadataPath() const;
    std::filesystem::path rightMetadataPath() const;
    std::filesystem::path pairMetadataPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace DualIC4Varjo
