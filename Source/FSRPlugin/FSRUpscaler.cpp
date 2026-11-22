#include <FSRPlugin/FSRPluginPCH.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Strings/StringConversion.h>
#include <RendererCore/Upscaling/Upscaler.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <FSRPlugin/FSRNative.h>
#include <ffx_api.h>
#include <ffx_upscale.h>

/// AMD FidelityFX Super Resolution through the FidelityFX API. The loader picks the best upscaler the driver offers
/// (FSR 4 on supported Radeon GPUs, FSR 3.1 everywhere else), on D3D12 and Vulkan.
class plUpscalerBackendFSR : public plUpscalerBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plUpscalerBackendFSR, plUpscalerBackend);

public:
  struct Functions
  {
    PfnFfxCreateContext m_CreateContext = nullptr;
    PfnFfxDestroyContext m_DestroyContext = nullptr;
    PfnFfxDispatch m_Dispatch = nullptr;
    PfnFfxQuery m_Query = nullptr;
  };

  virtual plStringView GetName() const override { return "FSR"; }
  virtual plStringView GetDisplayName() const override { return "AMD FSR"; }

  // Preferred on AMD GPUs, where the driver can run FSR 4. Elsewhere vendor upscalers come first; FSR still beats TSR.
  virtual plInt32 GetAutoPriority() const override { return m_uiVendorId == 0x1002 ? 30 : 10; }

  virtual plUpscalerCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual plUniquePtr<plUpscalerContext> CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc) override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  bool LoadRuntime(plGALNativeApi::Enum api);

  HMODULE m_hLoader = nullptr;
  HMODULE m_hUpscaler = nullptr;
  Functions m_Functions;
  plUInt32 m_uiVendorId = 0;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUpscalerBackendFSR, 1, plRTTIDefaultAllocator<plUpscalerBackendFSR>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  void FfxMessage(uint32_t type, const wchar_t* szMessage)
  {
    plStringUtf8 sMessage(szMessage);
    if (type == FFX_API_MESSAGE_TYPE_ERROR)
      plLog::Error("FSR: {}", sMessage.GetData());
    else
      plLog::Warning("FSR: {}", sMessage.GetData());
  }

  /// The directory this plugin was loaded from; the FidelityFX binaries are installed next to it.
  plStringBuilder GetPluginDirectory()
  {
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetPluginDirectory), &hSelf);

    wchar_t szPath[MAX_PATH] = {};
    GetModuleFileNameW(hSelf, szPath, MAX_PATH);

    plStringBuilder sPath = plStringUtf8(szPath).GetData();
    sPath.PathParentDirectory();
    return sPath;
  }

  HMODULE LoadFromPluginDirectory(plStringView sFile)
  {
    plStringBuilder sPath = GetPluginDirectory();
    sPath.AppendPath(sFile);
    sPath.MakePathSeparatorsNative();
    return LoadLibraryExW(plStringWChar(sPath), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  }

  FfxApiResource GetResource(plGALDevice& ref_device, plGALNativeApi::Enum api, plGALTextureHandle hTexture, uint32_t uiState)
  {
    FfxApiResource resource = {};
    resource.state = uiState;

    plGALNativeTexture texture;
    if (hTexture.IsInvalidated() || ref_device.GetNativeTexture(hTexture, texture).Failed())
      return resource;

    return api == plGALNativeApi::D3D12 ? plFSRGetResourceDX12(texture, uiState) : plFSRGetResourceVK(texture, uiState);
  }

  class plFSRUpscalerContext : public plUpscalerContext
  {
  public:
    plFSRUpscalerContext(plGALDevice& ref_device, const plUpscalerBackendFSR::Functions& functions, const plUpscalerContextDesc& desc)
      : m_Device(ref_device)
      , m_Functions(functions)
    {
      m_Desc = desc;
    }

    ~plFSRUpscalerContext()
    {
      if (m_Context != nullptr)
        m_Functions.m_DestroyContext(&m_Context, nullptr);
    }

    bool Create()
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
      createDesc.fpMessage = &FfxMessage;

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

    virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params) override
    {
      void* pCommandList = m_Device.BeginNativeCommands(ref_encoder);
      if (pCommandList == nullptr)
        return PL_FAILURE;

      ffxDispatchDescUpscale dispatch = {};
      dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
      dispatch.commandList = pCommandList;
      dispatch.color = GetResource(m_Device, m_Api, params.m_hColor, FFX_API_RESOURCE_STATE_COMPUTE_READ);
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

      const ffxReturnCode_t result = m_Functions.m_Dispatch(&m_Context, &dispatch.header);
      m_Device.EndNativeCommands(ref_encoder);
      return result == FFX_API_RETURN_OK ? PL_SUCCESS : PL_FAILURE;
    }

  private:
    plGALDevice& m_Device;
    plUpscalerBackendFSR::Functions m_Functions;
    plGALNativeApi::Enum m_Api = plGALNativeApi::None;
    ffxContext m_Context = nullptr;
  };
} // namespace

bool plUpscalerBackendFSR::LoadRuntime(plGALNativeApi::Enum api)
{
  if (m_hLoader != nullptr)
    return true;

  const bool bD3D12 = api == plGALNativeApi::D3D12;

  // The loader finds the upscaler by name, which resolves to the copy already in the process.
  m_hUpscaler = LoadFromPluginDirectory(bD3D12 ? "amd_fidelityfx_upscaler_dx12.dll" : "amd_fidelityfx_upscaler_vk.dll");
  m_hLoader = LoadFromPluginDirectory(bD3D12 ? "amd_fidelityfx_loader_dx12.dll" : "amd_fidelityfx_loader_vk.dll");
  if (m_hLoader == nullptr)
  {
    plLog::Warning("FSR: the FidelityFX runtime could not be loaded (error {}).", static_cast<plUInt32>(GetLastError()));
    return false;
  }

  m_Functions.m_CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m_hLoader, "ffxCreateContext"));
  m_Functions.m_DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m_hLoader, "ffxDestroyContext"));
  m_Functions.m_Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m_hLoader, "ffxDispatch"));
  m_Functions.m_Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m_hLoader, "ffxQuery"));
  return m_Functions.m_CreateContext != nullptr && m_Functions.m_DestroyContext != nullptr && m_Functions.m_Dispatch != nullptr;
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

  plUniquePtr<plFSRUpscalerContext> pContext = PL_DEFAULT_NEW(plFSRUpscalerContext, ref_device, m_Functions, desc);
  if (!pContext->Create())
    return nullptr;
  return pContext;
}

void plUpscalerBackendFSR::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  if (m_hLoader != nullptr)
    FreeLibrary(m_hLoader);
  if (m_hUpscaler != nullptr)
    FreeLibrary(m_hUpscaler);
  m_hLoader = nullptr;
  m_hUpscaler = nullptr;
  m_Functions = {};
}

PL_STATICLINK_FILE(FSRPlugin, FSRPlugin_FSRUpscaler);
