#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRNative.h>

// Only the types are needed: no Vulkan function is called from here.
#define VK_NO_PROTOTYPES
#include <vk/ffx_api_vk.h>

static_assert(sizeof(ffxCreateBackendVKDesc) <= sizeof(plFSRBackendDesc::m_Storage));

void plFSRMakeBackendDescVK(plFSRBackendDesc& out_desc, const plGALNativeDevice& device)
{
  ffxCreateBackendVKDesc* pDesc = new (out_desc.m_Storage) ffxCreateBackendVKDesc();
  pDesc->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  pDesc->deviceContext.vkDevice = static_cast<VkDevice>(device.m_pDevice);
  pDesc->deviceContext.vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.m_pAdapter);
  pDesc->deviceContext.vkDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(device.m_pfnGetDeviceProcAddr);
  out_desc.m_pHeader = &pDesc->header;
}

FfxApiResource plFSRGetResourceVK(const plGALNativeTexture& texture, uint32_t uiState)
{
  FfxApiResource resource = {};
  resource.resource = texture.m_pResource;
  resource.state = uiState;
  resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  resource.description.format = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(texture.m_uiNativeFormat));
  resource.description.width = texture.m_uiWidth;
  resource.description.height = texture.m_uiHeight;
  resource.description.depth = 1;
  resource.description.mipCount = texture.m_uiMipLevels;
  resource.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
  resource.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
  if (texture.m_bUnorderedAccess)
    resource.description.usage |= FFX_API_RESOURCE_USAGE_UAV;
  if (texture.m_bDepth)
    resource.description.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
  return resource;
}
