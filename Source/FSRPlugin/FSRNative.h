#pragma once

#include <RendererFoundation/Device/Device.h>
#include <ffx_api.h>
#include <ffx_api_types.h>

// The FidelityFX D3D12 and Vulkan headers declare the same callback types with different parameters, so neither may be
// included next to the other. Everything that needs one of them lives in its own source file behind these functions.

/// A backend description for ffxCreateContext, chained into the effect's description through m_pHeader.
struct plFSRBackendDesc
{
  alignas(16) plUInt8 m_Storage[128] = {};
  ffxCreateContextDescHeader* m_pHeader = nullptr;
};

void plFSRMakeBackendDescDX12(plFSRBackendDesc& out_desc, const plGALNativeDevice& device);
void plFSRMakeBackendDescVK(plFSRBackendDesc& out_desc, const plGALNativeDevice& device);

FfxApiResource plFSRGetResourceDX12(const plGALNativeTexture& texture, uint32_t uiState);
FfxApiResource plFSRGetResourceVK(const plGALNativeTexture& texture, uint32_t uiState);

/// The typed DXGI_FORMAT a shader reads a texture of the given (possibly typeless or depth) DXGI_FORMAT as.
plUInt32 plFSRGetShaderReadFormatDX12(plUInt32 uiFormat);
