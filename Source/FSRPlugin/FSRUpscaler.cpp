#include <FSRPlugin/FSRPluginPCH.h>

#include <RendererCore/Upscaling/Upscaler.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <FSRPlugin/FSRNative.h>
#include <FSRPlugin/FSRRuntime.h>
#include <FSRPlugin/FSRUpscalerContext.h>
#include <ffx_api.h>
#include <ffx_upscale.h>

/// AMD FidelityFX Super Resolution through the FidelityFX API. The loader picks the best upscaler the driver offers
/// (FSR 4 on supported Radeon GPUs, FSR 3.1 everywhere else), on D3D12 and Vulkan.
class plUpscalerBackendFSR : public plUpscalerBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plUpscalerBackendFSR, plUpscalerBackend);

public:
  virtual plStringView GetName() const override { return "FSR"; }
  virtual plStringView GetDisplayName() const override { return "AMD FSR"; }

  // Preferred on AMD GPUs, where the driver can run FSR 4. Elsewhere vendor upscalers come first; FSR still beats TSR.
  virtual plInt32 GetAutoPriority() const override { return m_uiVendorId == 0x1002 ? 30 : 10; }

  virtual plUpscalerCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual plUniquePtr<plUpscalerContext> CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc) override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  bool LoadRuntime(plGALNativeApi::Enum api);

  plFSRRuntime m_Runtime;
  plUInt32 m_uiVendorId = 0;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUpscalerBackendFSR, 1, plRTTIDefaultAllocator<plUpscalerBackendFSR>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  FfxApiResource GetResource(plGALDevice& ref_device, plGALNativeApi::Enum api, plGALTextureHandle hTexture, uint32_t uiState)
  {
    FfxApiResource resource = {};
    resource.state = uiState;

    plGALNativeTexture texture;
    if (hTexture.IsInvalidated() || ref_device.GetNativeTexture(hTexture, texture).Failed())
      return resource;

    return api == plGALNativeApi::D3D12 ? plFSRGetResourceDX12(texture, uiState) : plFSRGetResourceVK(texture, uiState);
  }
} // namespace

plFSRUpscalerContext::plFSRUpscalerContext(plGALDevice& ref_device, const plFSRFunctions& functions, const plUpscalerContextDesc& desc)
  : m_Device(ref_device)
  , m_Functions(functions)
{
  m_Desc = desc;
}

plFSRUpscalerContext::~plFSRUpscalerContext()
{
  if (m_Context != nullptr)
    m_Functions.m_DestroyContext(&m_Context, nullptr);
}

bool plFSRUpscalerContext::Create()
{
  const plGALNativeDevice native = m_Device.GetNativeDevice();
  m_Api = native.m_Api;

  ffxCreateContextDescUpscale createDesc = {};
  createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  createDesc.flags = FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
  if (m_Desc.m_bHDR)
    createDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
  if (m_Desc.m_bDepthInverted)
    createDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  if (m_Desc.m_bDepthInfinite)
    createDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
  createDesc.maxRenderSize = {m_Desc.m_uiMaxRenderWidth, m_Desc.m_uiMaxRenderHeight};
  createDesc.maxUpscaleSize = {m_Desc.m_uiDisplayWidth, m_Desc.m_uiDisplayHeight};
  createDesc.fpMessage = &plFSRMessage;

  plFSRBackendDesc backend;
  if (m_Api == plGALNativeApi::D3D12)
    plFSRMakeBackendDescDX12(backend, native);
  else if (m_Api == plGALNativeApi::Vulkan)
    plFSRMakeBackendDescVK(backend, native);
  else
    return false;
  createDesc.header.pNext = backend.m_pHeader;

  const ffxReturnCode_t result = m_Functions.m_CreateContext(&m_Context, &createDesc.header, nullptr);
  if (result != FFX_API_RETURN_OK)
  {
    plLog::Error("FSR: creating the upscaler context failed (error {}).", result);
    m_Context = nullptr;
    return false;
  }
  return true;
}

plResult plFSRUpscalerContext::Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params)
{
  void* pCommandList = m_Device.BeginNativeCommands(ref_encoder);
  if (pCommandList == nullptr)
    return PL_FAILURE;

  const plResult result = Dispatch(pCommandList, params, nullptr);
  m_Device.EndNativeCommands(ref_encoder);
  return result;
}

plResult plFSRUpscalerContext::Dispatch(void* pCommandList, const plUpscalerExecuteParams& params, const FfxApiResource* pColor)
{
  ffxDispatchDescUpscale dispatch = {};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = pCommandList;
  dispatch.color = pColor != nullptr ? *pColor : GetResource(m_Device, m_Api, params.m_hColor, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.depth = GetResource(m_Device, m_Api, params.m_hDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.motionVectors = GetResource(m_Device, m_Api, params.m_hVelocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.reactive = GetResource(m_Device, m_Api, params.m_hReactiveMask, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.transparencyAndComposition = GetResource(m_Device, m_Api, params.m_hTransparencyCompositionMask, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.output = GetResource(m_Device, m_Api, params.m_hOutput, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

  dispatch.jitterOffset = {params.m_vJitterPixels.x, params.m_vJitterPixels.y};
  // The engine's velocity is current minus previous in UV units; FSR wants the pixel offset to the previous position.
  dispatch.motionVectorScale = {-static_cast<float>(params.m_uiRenderWidth), -static_cast<float>(params.m_uiRenderHeight)};
  dispatch.renderSize = {params.m_uiRenderWidth, params.m_uiRenderHeight};
  dispatch.upscaleSize = {params.m_uiDisplayWidth, params.m_uiDisplayHeight};
  dispatch.enableSharpening = params.m_fSharpness > 0.0f;
  dispatch.sharpness = params.m_fSharpness;
  dispatch.frameTimeDelta = params.m_fDeltaTimeMs;
  dispatch.preExposure = plMath::Max(params.m_fPreExposure, 0.0001f);
  dispatch.reset = params.m_bReset;
  dispatch.cameraNear = params.m_fCameraNear;
  dispatch.cameraFar = params.m_fCameraFar;
  dispatch.cameraFovAngleVertical = params.m_fCameraFovVertical;
  dispatch.viewSpaceToMetersFactor = 1.0f;

  return m_Functions.m_Dispatch(&m_Context, &dispatch.header) == FFX_API_RETURN_OK ? PL_SUCCESS : PL_FAILURE;
}

bool plUpscalerBackendFSR::LoadRuntime(plGALNativeApi::Enum api)
{
  static const char* const s_EffectsDX12[] = {"amd_fidelityfx_upscaler_dx12.dll"};
  static const char* const s_EffectsVK[] = {"amd_fidelityfx_upscaler_vk.dll"};

  if (api == plGALNativeApi::D3D12)
    return m_Runtime.Load(api, s_EffectsDX12);
  return m_Runtime.Load(api, s_EffectsVK);
}

plUpscalerCapabilities plUpscalerBackendFSR::QueryCapabilities(plGALDevice& ref_device)
{
  plUpscalerCapabilities caps;

  const plGALNativeDevice native = ref_device.GetNativeDevice();
  m_uiVendorId = native.m_uiVendorId;
  if (native.m_Api != plGALNativeApi::D3D12 && native.m_Api != plGALNativeApi::Vulkan)
    return caps;

  caps.m_bSupported = LoadRuntime(native.m_Api);
  caps.m_bSupportsTextureArrays = false;
  caps.m_bUsesReactiveMask = true;
  caps.m_bUsesTransparencyCompositionMask = true;
  caps.m_bSupportsDynamicResolution = true;
  caps.m_fMinRenderScale = 1.0f / 3.0f;
  return caps;
}

plUniquePtr<plUpscalerContext> plUpscalerBackendFSR::CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
{
  if (!LoadRuntime(ref_device.GetNativeDevice().m_Api))
    return nullptr;

  plUniquePtr<plFSRUpscalerContext> pContext = PL_DEFAULT_NEW(plFSRUpscalerContext, ref_device, m_Runtime.GetFunctions(), desc);
  if (!pContext->Create())
    return nullptr;
  return pContext;
}

void plUpscalerBackendFSR::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  m_Runtime.Unload();
}

PL_STATICLINK_FILE(FSRPlugin, FSRPlugin_FSRUpscaler);
