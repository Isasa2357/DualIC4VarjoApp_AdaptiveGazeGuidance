#pragma once

#include <IC4Ext/IC4Ext.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace DualIC4Varjo {

struct ImGuiStereoPreviewConfig {
    int windowWidth = 1600;
    int windowHeight = 800;
    bool vsync = true;
    std::string windowTitle = "Dual IC4 Stereo Preview";
};

class ImGuiStereoPreview {
public:
    ImGuiStereoPreview(
        std::shared_ptr<D3D12CoreLib::D3D12Core> core,
        std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> inputQueue,
        ImGuiStereoPreviewConfig config = {});
    ~ImGuiStereoPreview();

    ImGuiStereoPreview(const ImGuiStereoPreview&) = delete;
    ImGuiStereoPreview& operator=(const ImGuiStereoPreview&) = delete;

    bool start();
    void requestStop() noexcept;
    void join() noexcept;
    void stopAndJoin() noexcept;

    bool running() const noexcept;
    std::string lastError() const;
    void rethrowWorkerExceptionIfAny() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace DualIC4Varjo
