#pragma once

#include <IC4Ext/IC4Ext.hpp>

#include <cstddef>
#include <utility>

namespace IC4Ext {

// Capacity-1 synchronized queues were sufficient for latest-frame preview and
// discard. Raw recording needs burst tolerance, so only capacity-1 queues are
// expanded to 16 pairs. Other explicitly configured capacities are preserved.
class RecordingD3D12SyncedFrameQueue final
    : public D3D12SyncedFrameQueue {
public:
    RecordingD3D12SyncedFrameQueue()
        : D3D12SyncedFrameQueue()
    {
    }

    explicit RecordingD3D12SyncedFrameQueue(std::size_t maxSize)
        : D3D12SyncedFrameQueue(maxSize == 1 ? 16 : maxSize)
    {
    }

    explicit RecordingD3D12SyncedFrameQueue(
        ThreadKit::Queues::QueueOptions options)
        : D3D12SyncedFrameQueue(adjust(std::move(options)))
    {
    }

private:
    static ThreadKit::Queues::QueueOptions adjust(
        ThreadKit::Queues::QueueOptions options)
    {
        if (options.maxSize == 1) options.maxSize = 16;
        return options;
    }
};

} // namespace IC4Ext
