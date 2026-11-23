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

plUInt32 plFSRGetShaderReadFormatDX12(plUInt32 uiFormat)
{
  switch (static_cast<DXGI_FORMAT>(uiFormat))
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
      return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
      return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      return uiFormat;
  }
}
