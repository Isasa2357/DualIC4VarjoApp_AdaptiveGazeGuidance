#pragma once

#include <IC4Ext/IC4Ext.hpp>
#include <VarjoXR/Backends/D3D12/D3D12Backend.hpp>

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>

namespace DualIC4Varjo {

class DisplayTextureRing {
public:
    struct UploadResult {
        std::shared_ptr<VarjoXR::Backends::D3D12::D3D12Texture> texture;
        std::size_t slotIndex = 0;
    };

    DisplayTextureRing(
        std::shared_ptr<D3D12CoreLib::D3D12Core> core,
        VarjoXR::Backends::D3D12::D3D12Backend& backend,
        std::size_t slotCount);
    ~DisplayTextureRing();

    DisplayTextureRing(const DisplayTextureRing&) = delete;
    DisplayTextureRing& operator=(const DisplayTextureRing&) = delete;

    UploadResult upload(const IC4Ext::D3D12CameraFrame& frame);
    void markRendered(std::size_t slotIndex);
    void waitIdle();

private:
    struct FormatKey {
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

        bool operator==(const FormatKey& other) const noexcept;
        bool operator!=(const FormatKey& other) const noexcept
        {
            return !(*this == other);
        }
    };

    struct Slot {
        D3D12CoreLib::D3D12CommandContext commandContext;
        D3D12CoreLib::D3D12Resource resource;
        std::shared_ptr<VarjoXR::Backends::D3D12::D3D12Texture> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> sourceKeepAlive;
        std::uint64_t lastRenderFence = 0;
    };

    static FormatKey makeFormatKey(const IC4Ext::D3D12CameraFrame& frame);
    void rebuild(const FormatKey& format);
    void waitSlot(Slot& slot);

    std::shared_ptr<D3D12CoreLib::D3D12Core> core_;
    VarjoXR::Backends::D3D12::D3D12Backend& backend_;
    std::size_t requestedSlotCount_ = 0;
    std::vector<Slot> slots_;
    FormatKey format_{};
    bool initialized_ = false;
    std::size_t nextSlot_ = 0;
};

} // namespace DualIC4Varjo
