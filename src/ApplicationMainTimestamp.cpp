#define main DualIC4VarjoBaseMain
#include "ApplicationMainPlaneControl.cpp"
#undef main

#include "TimestampLoadService.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    DualIC4Varjo::TimestampLoadService timestampService(argc, argv);
    if (!timestampService.start()) {
        std::cerr
            << "Timestamp service failed to start: "
            << timestampService.lastError()
            << '\n';
        return 1;
    }

    int result = 1;
    try {
        result = DualIC4VarjoBaseMain(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr
            << "Unhandled application exception: "
            << exception.what()
            << '\n';
        result = 1;
    } catch (...) {
        std::cerr << "Unhandled unknown application exception.\n";
        result = 1;
    }

    timestampService.stop();
    std::cout
        << "[TIMESTAMP] samples="
        << timestampService.sampleCount()
        << " failed="
        << timestampService.failedSampleCount()
        << " CSV="
        << timestampService.outputPath().string()
        << '\n';
    return result;
}
