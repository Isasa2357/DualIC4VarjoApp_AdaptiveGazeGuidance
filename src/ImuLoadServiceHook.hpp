#pragma once

// Include this before defining the markRendered macro in the IMU entry point.
// The include guard prevents the macro from rewriting the class declaration.
#include "StereoDisplayTextureRing.hpp"

#include <VarjoToolkit/Core/VarjoFrameInfo.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <Varjo.h>

namespace DualIC4Varjo {

class ImuLoadServiceHook {
public:
    static void configure(int argc, char** argv);

    static void submit(
        const VarjoFrameInfoSnapshot& snapshot,
        const std::shared_ptr<varjo_Session>& session) noexcept;

    static void stop() noexcept;

    static std::filesystem::path outputPath();
    static std::string lastError();
    static std::uint64_t receivedCount() noexcept;
    static std::uint64_t processedCount() noexcept;
    static std::uint64_t writtenCount() noexcept;
    static std::uint64_t droppedCount() noexcept;
};

} // namespace DualIC4Varjo
