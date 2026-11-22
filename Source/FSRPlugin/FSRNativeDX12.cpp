#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRNative.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>

#include <d3d12.h>
#include <dx12/ffx_api_dx12.h>

static_assert(sizeof(ffxCreateBackendDX12Desc) <= sizeof(plFSRBackendDesc::m_Storage));

void plFSRMakeBackendDescDX12(plFSRBackendDesc& out_desc, const plGALNativeDevice& device)
{
  ffxCreateBackendDX12Desc* pDesc = new (out_desc.m_Storage) ffxCreateBackendDX12Desc();
  pDesc->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  pDesc->device = static_cast<ID3D12Device*>(device.m_pDevice);
  out_desc.m_pHeader = &pDesc->header;
}

FfxApiResource plFSRGetResourceDX12(const plGALNativeTexture& texture, uint32_t uiState)
{
  // Reads size, format and usage from the resource itself.
  return ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(texture.m_pResource), uiState, 0);
}
