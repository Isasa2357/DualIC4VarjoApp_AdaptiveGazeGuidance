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

class StereoDisplayTextureRing {
public:
    struct UploadResult {
        std::shared_ptr<VarjoXR::Backends::D3D12::D3D12Texture> left;
        std::shared_ptr<VarjoXR::Backends::D3D12::D3D12Texture> right;
        std::size_t slotIndex = 0;
    };

    StereoDisplayTextureRing(
        std::shared_ptr<D3D12CoreLib::D3D12Core> core,
        VarjoXR::Backends::D3D12::D3D12Backend& backend,
        std::size_t slotCount);
    ~StereoDisplayTextureRing();

    StereoDisplayTextureRing(const StereoDisplayTextureRing&) = delete;
    StereoDisplayTextureRing& operator=(const StereoDisplayTextureRing&) = delete;

    UploadResult upload(const IC4Ext::D3D12CameraFrame& left, const IC4Ext::D3D12CameraFrame& right);
    void markRendered(std::size_t slotIndex);
    void waitIdle();

private:
    struct FormatKey {
        UINT leftWidth = 0;
        UINT leftHeight = 0;
        DXGI_FORMAT leftFormat = DXGI_FORMAT_UNKNOWN;
        UINT rightWidth = 0;
        UINT rightHeight = 0;
        DXGI_FORMAT rightFormat = DXGI_FORMAT_UNKNOWN;

        bool operator==(const FormatKey& other) const noexcept;
        bool operator!=(const FormatKey& other) const noexcept { return !(*this == other); }
    };

    struct Slot {
        D3D12CoreLib::D3D12CommandContext commandContext;
        D3D12CoreLib::D3D12Resource leftResource;
        D3D12CoreLib::D3D12Resource rightResource;
        std::shared_ptr<VarjoXR::Backends::D3D12::D3D12Texture> leftTexture;
        std::shared_ptr<VarjoXR::Backends::D3D12::D3D12Texture> rightTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> leftSourceKeepAlive;
        Microsoft::WRL::ComPtr<ID3D12Resource> rightSourceKeepAlive;
        std::uint64_t lastRenderFence = 0;
    };

    static FormatKey makeFormatKey(const IC4Ext::D3D12CameraFrame& left, const IC4Ext::D3D12CameraFrame& right);
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
