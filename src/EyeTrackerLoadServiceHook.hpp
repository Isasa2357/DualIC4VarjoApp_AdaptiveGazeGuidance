#pragma once

// Include this before defining the markRendered macro in the integration entry
// point. The include guard prevents the macro from rewriting declarations.
#include "StereoDisplayTextureRing.hpp"

#include <VarjoToolkit/Core/VarjoFrameInfo.hpp>
#include <VarjoToolkit/Services/EyeTracking/VarjoEyeTrackingService.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Varjo.h>

namespace DualIC4Varjo {

class EyeTrackerLoadServiceHook {
public:
    static void configure(int argc, char** argv);

    static void submit(
        const VarjoFrameInfoSnapshot& snapshot,
        const std::shared_ptr<varjo_Session>& session) noexcept;

    // Called by the application integration layer. EyeTrackerService remains
    // responsible only for acquisition and its own CSV; projection is external.
    static std::vector<VarjoEyeTrackingData> requestData();

    static void stop() noexcept;

    static std::filesystem::path outputPath();
    static std::string lastError();
    static std::uint64_t receivedSampleCount() noexcept;
    static std::uint64_t processedSampleCount() noexcept;
    static std::uint64_t writtenSampleCount() noexcept;
    static std::uint64_t droppedSampleCount() noexcept;
    static std::uint64_t submittedFrameInfoCount() noexcept;
    static std::uint64_t droppedFrameInfoCount() noexcept;
};

} // namespace DualIC4Varjo
