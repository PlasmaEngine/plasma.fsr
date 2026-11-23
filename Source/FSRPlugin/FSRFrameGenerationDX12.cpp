#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRNative.h>
#include <FSRPlugin/FSRRuntime.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/UniquePtr.h>
#include <RendererCore/FrameGeneration/FrameGeneration.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Device/NativeHooks.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>

class plFrameGenerationBackendFSR;

/// Creates the FidelityFX frame generation swap chain for the window while the FSR backend owns it.
class plFSRFrameGenerationHook : public plGALNativeHook
{
public:
  explicit plFSRFrameGenerationHook(plFrameGenerationBackendFSR& ref_backend)
    : m_Backend(ref_backend)
  {
  }

  virtual plStringView GetName() const override { return "FSR Frame Generation"; }
  virtual void* CreateSwapChain(const plGALNativeSwapChainCreateInfo& info) override;
  virtual void OnSwapChainDestroying(plGALSwapChain* pSwapChain, void* pNativeSwapChain) override;

private:
  plFrameGenerationBackendFSR& m_Backend;
};

/// AMD FSR Frame Generation on D3D12: one interpolated frame between two rendered ones, presented by the FidelityFX
/// frame generation swap chain that replaces the window's swap chain.
///
/// ExecuteSceneInputs records the FidelityFX prepare pass (depth, motion vectors, camera). ExecuteHudless copies the
/// HUD-less image into a texture the backend owns, because the swap chain reads it when it generates the frame at
/// present, and hands it to the swap chain configuration. The UI is extracted from the difference between the back
/// buffer and the HUD-less image.
class plFrameGenerationBackendFSR : public plFrameGenerationBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plFrameGenerationBackendFSR, plFrameGenerationBackend);

public:
  plFrameGenerationBackendFSR();
  ~plFrameGenerationBackendFSR();

  virtual plStringView GetName() const override { return "FSR"; }
  virtual plStringView GetDisplayName() const override { return "AMD FSR Frame Generation"; }
  virtual plInt32 GetAutoPriority() const override { return m_uiVendorId == 0x1002 ? 30 : 10; }

  virtual plFrameGenerationCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual void Configure(plGALDevice& ref_device, bool bActive, const plFrameGenerationSettings& settings) override;
  virtual void ExecuteSceneInputs(plGALCommandEncoder& ref_encoder, const plFrameGenerationSceneParams& params) override;
  virtual void ExecuteHudless(plGALCommandEncoder& ref_encoder, const plFrameGenerationHudlessParams& params) override;
  virtual plFrameGenerationStats GetStats() const override;
  virtual void Shutdown(plGALDevice& ref_device) override;

  void* CreateSwapChain(const plGALNativeSwapChainCreateInfo& info);
  void OnSwapChainDestroying(void* pNativeSwapChain);

private:
  static constexpr plUInt32 s_uiHudlessCopies = 3;

  /// Frames after which nothing in flight uses a retired context or copy any more.
  static constexpr plUInt64 s_uiRetireFrames = 4;

  /// A frame generation context. The swap chain's frame generation callback gets the one the frame was configured with,
  /// because the present thread generates frames after the render thread has moved on, possibly to a new context.
  struct Context
  {
    plFrameGenerationBackendFSR* m_pBackend = nullptr;
    ffxContext m_Context = nullptr;
    plUInt64 m_uiPreparedFrameId = plMath::MaxValue<plUInt64>(); ///< The last frame whose prepare pass this context recorded.
  };

  struct RetiredResource
  {
    ID3D12Resource* m_pResource = nullptr;
    plUInt64 m_uiFrameId = 0;
  };

  struct RetiredContext
  {
    Context* m_pContext = nullptr;
    plUInt64 m_uiFrameId = 0;
  };

  static ffxReturnCode_t DispatchFrameGeneration(ffxDispatchDescFrameGeneration* pParams, void* pUserContext);

  bool LoadRuntime();
  bool EnsureFrameGenerationContext(plUInt32 uiWidth, plUInt32 uiHeight, DXGI_FORMAT hudlessFormat, plUInt64 uiFrameId);
  void RetireFrameGenerationContext(plUInt64 uiFrameId);
  void DestroyContext(Context* pContext);
  void DestroyContexts();
  void ReleaseRetiredResources(plUInt64 uiFrameId, bool bAll);
  void ConfigureDisabled();

  mutable plMutex m_Mutex;
  plFSRRuntime m_Runtime;
  plUInt32 m_uiVendorId = 0;

  ID3D12Device* m_pDevice = nullptr;
  IDXGISwapChain4* m_pSwapChain = nullptr; ///< Owned by the engine's swap chain; created through m_SwapChainContext.
  ffxContext m_SwapChainContext = nullptr;
  DXGI_FORMAT m_BackBufferFormat = DXGI_FORMAT_UNKNOWN;

  Context* m_pFrameGenerationContext = nullptr;
  plUInt32 m_uiContextWidth = 0;
  plUInt32 m_uiContextHeight = 0;
  DXGI_FORMAT m_ContextHudlessFormat = DXGI_FORMAT_UNKNOWN;
  bool m_bContextDepthInverted = false;
  ID3D12Resource* m_HudlessCopies[s_uiHudlessCopies] = {};
  plDynamicArray<RetiredResource> m_RetiredResources;
  plDynamicArray<RetiredContext> m_RetiredContexts;

  bool m_bEnabled = false;
  bool m_bDepthInverted = false;
  plUInt64 m_uiLastFrameId = 0;

  plUniquePtr<plFSRFrameGenerationHook> m_pHook;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plFrameGenerationBackendFSR, 1, plRTTIDefaultAllocator<plFrameGenerationBackendFSR>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void* plFSRFrameGenerationHook::CreateSwapChain(const plGALNativeSwapChainCreateInfo& info)
{
  return m_Backend.CreateSwapChain(info);
}

void plFSRFrameGenerationHook::OnSwapChainDestroying(plGALSwapChain* pSwapChain, void* pNativeSwapChain)
{
  PL_IGNORE_UNUSED(pSwapChain);
  m_Backend.OnSwapChainDestroying(pNativeSwapChain);
}

/// The interface Streamline's D3D12 and DXGI proxies answer with the object they wrap.
struct DECLSPEC_UUID("ADEC44E2-61F0-45C3-AD9F-1B37379284FF") plStreamlineRetrieveBaseInterface : IUnknown
{
};

namespace
{
  /// Returns the object a Streamline proxy wraps, with a reference, or nullptr when pObject is no proxy.
  ///
  /// The FidelityFX swap chain creates its present queue on the game queue's device and hands that queue to DXGI. A
  /// queue created through Streamline's device proxy returns the proxy device, so DXGI would get a proxy queue it cannot
  /// use and the driver faults in the first present.
  template <typename T>
  T* RetrieveStreamlineBase(T* pObject)
  {
    if (pObject == nullptr)
      return nullptr;

    void* pBase = nullptr;
    if (FAILED(pObject->QueryInterface(__uuidof(plStreamlineRetrieveBaseInterface), &pBase)) || pBase == nullptr)
      return nullptr;

    return static_cast<T*>(pBase);
  }

  D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* pResource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
  {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = pResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
  }
} // namespace

plFrameGenerationBackendFSR::plFrameGenerationBackendFSR()
{
  // Backends are created when the plugin loads, before the game window's swap chain exists.
  m_pHook = PL_DEFAULT_NEW(plFSRFrameGenerationHook, *this);
}

plFrameGenerationBackendFSR::~plFrameGenerationBackendFSR()
{
  m_pHook.Clear();

  PL_LOCK(m_Mutex);
  DestroyContexts();
}

bool plFrameGenerationBackendFSR::LoadRuntime()
{
  static const char* const s_Effects[] = {"amd_fidelityfx_framegeneration_dx12.dll"};
  return m_Runtime.Load(plGALNativeApi::D3D12, s_Effects);
}

plFrameGenerationCapabilities plFrameGenerationBackendFSR::QueryCapabilities(plGALDevice& ref_device)
{
  plFrameGenerationCapabilities caps;
  caps.m_uiMaxGeneratedFrames = 1;
  caps.m_bSupportsDynamic = false;
  caps.m_bReplacesSwapChain = true;
  caps.m_bUsesHudless = true;
  caps.m_bNoScRGB = false;
  caps.m_szRequiredLowLatencyBackend = "";

  const plGALNativeDevice native = ref_device.GetNativeDevice();
  m_uiVendorId = native.m_uiVendorId;

  // Vulkan frame generation needs the Vulkan swap chain hooks, which the engine does not offer yet.
  if (native.m_Api != plGALNativeApi::D3D12)
    return caps;

  PL_LOCK(m_Mutex);

  if (!LoadRuntime())
    return caps;

  uint64_t uiVersionCount = 0;
  ffxQueryDescGetVersions query = {};
  query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  query.device = native.m_pDevice;
  query.outputCount = &uiVersionCount;

  caps.m_bSupported = m_Runtime.GetFunctions().m_Query(nullptr, &query.header) == FFX_API_RETURN_OK && uiVersionCount > 0;
  return caps;
}

void plFrameGenerationBackendFSR::Configure(plGALDevice& ref_device, bool bActive, const plFrameGenerationSettings& settings)
{
  PL_IGNORE_UNUSED(ref_device);

  PL_LOCK(m_Mutex);

  m_bEnabled = bActive && settings.m_bEnabled;

  // Turning frame generation off re-creates the swap chain, which destroys the contexts. Until then the swap chain only
  // presents the rendered frames.
  if (!m_bEnabled)
    ConfigureDisabled();
}

void plFrameGenerationBackendFSR::ConfigureDisabled()
{
  if (m_pFrameGenerationContext == nullptr || m_pSwapChain == nullptr)
    return;

  ffxConfigureDescFrameGeneration config = {};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.swapChain = m_pSwapChain;
  config.frameGenerationEnabled = false;
  config.frameID = m_uiLastFrameId;
  m_Runtime.GetFunctions().m_Configure(&m_pFrameGenerationContext->m_Context, &config.header);
}

void* plFrameGenerationBackendFSR::CreateSwapChain(const plGALNativeSwapChainCreateInfo& info)
{
  if (info.m_Api != plGALNativeApi::D3D12 || !plFrameGenerationRegistry::ShouldOwnSwapChain(this, info))
    return nullptr;

  PL_LOCK(m_Mutex);

  // Only one window presents generated frames.
  if (m_SwapChainContext != nullptr)
    return nullptr;

  if (!LoadRuntime())
    return nullptr;

  DXGI_SWAP_CHAIN_DESC1 desc = *static_cast<const DXGI_SWAP_CHAIN_DESC1*>(info.m_pDesc);

  // When Streamline (the DLSS plugin) created the engine's queues, the swap chain is created on the queue and factory
  // Streamline wraps. The engine keeps submitting through the proxy, which forwards to the same queue.
  ID3D12CommandQueue* pGameQueue = static_cast<ID3D12CommandQueue*>(info.m_pQueue);
  IDXGIFactory* pFactory = static_cast<IDXGIFactory*>(info.m_pFactory);
  ID3D12CommandQueue* pBaseQueue = RetrieveStreamlineBase(pGameQueue);
  IDXGIFactory* pBaseFactory = RetrieveStreamlineBase(pFactory);
  if (pBaseQueue != nullptr)
  {
    plLog::Info("FSR: the graphics queue is a Streamline proxy; the frame generation swap chain uses the queue it wraps.");
    pGameQueue = pBaseQueue;
  }
  if (pBaseFactory != nullptr)
    pFactory = pBaseFactory;

  IDXGISwapChain4* pSwapChain = nullptr;
  ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 createDesc = {};
  createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
  createDesc.swapchain = &pSwapChain;
  createDesc.hwnd = static_cast<HWND>(info.m_pWindowHandle);
  createDesc.desc = &desc;
  createDesc.fullscreenDesc = nullptr;
  createDesc.dxgiFactory = pFactory;
  createDesc.gameQueue = pGameQueue;

  ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc = {};
  versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
  versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
  createDesc.header.pNext = &versionDesc.header;

  const ffxReturnCode_t result = m_Runtime.GetFunctions().m_CreateContext(&m_SwapChainContext, &createDesc.header, nullptr);

  // The proxies keep the objects they wrap alive for as long as the engine uses them.
  if (pBaseQueue != nullptr)
    pBaseQueue->Release();
  if (pBaseFactory != nullptr)
    pBaseFactory->Release();

  if (result != FFX_API_RETURN_OK || pSwapChain == nullptr)
  {
    plLog::Error("FSR: creating the frame generation swap chain failed (error {}).", result);
    m_SwapChainContext = nullptr;
    return nullptr;
  }

  m_pSwapChain = pSwapChain;
  m_pDevice = static_cast<ID3D12Device*>(info.m_pDevice);
  m_BackBufferFormat = desc.Format;

  // The reference the context returned the swap chain with is the one the engine takes over.
  return static_cast<IDXGISwapChain1*>(pSwapChain);
}

void plFrameGenerationBackendFSR::OnSwapChainDestroying(void* pNativeSwapChain)
{
  PL_LOCK(m_Mutex);

  if (m_SwapChainContext == nullptr || pNativeSwapChain != m_pSwapChain)
    return;

  DestroyContexts();
}

void plFrameGenerationBackendFSR::DestroyContexts()
{
  if (m_SwapChainContext != nullptr)
  {
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait = {};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    m_Runtime.GetFunctions().m_Dispatch(&m_SwapChainContext, &wait.header);
  }

  ConfigureDisabled();

  // The engine waits for the GPU before it destroys the swap chain or the device, so no command list uses the contexts.
  DestroyContext(m_pFrameGenerationContext);
  m_pFrameGenerationContext = nullptr;

  for (const RetiredContext& retired : m_RetiredContexts)
    DestroyContext(retired.m_pContext);
  m_RetiredContexts.Clear();

  for (ID3D12Resource*& pCopy : m_HudlessCopies)
  {
    if (pCopy != nullptr)
      pCopy->Release();
    pCopy = nullptr;
  }
  ReleaseRetiredResources(0, true);

  // The engine releases its reference to the swap chain after this; the swap chain stays valid until then.
  if (m_SwapChainContext != nullptr)
    m_Runtime.GetFunctions().m_DestroyContext(&m_SwapChainContext, nullptr);
  m_SwapChainContext = nullptr;
  m_pSwapChain = nullptr;
  m_uiContextWidth = 0;
  m_uiContextHeight = 0;
  m_ContextHudlessFormat = DXGI_FORMAT_UNKNOWN;
}

void plFrameGenerationBackendFSR::ReleaseRetiredResources(plUInt64 uiFrameId, bool bAll)
{
  for (plUInt32 i = m_RetiredResources.GetCount(); i-- > 0;)
  {
    // Frames in flight may still read a retired copy.
    if (bAll || m_RetiredResources[i].m_uiFrameId + s_uiRetireFrames <= uiFrameId)
    {
      m_RetiredResources[i].m_pResource->Release();
      m_RetiredResources.RemoveAtAndSwap(i);
    }
  }

  bool bWaitedForPresents = false;
  for (plUInt32 i = m_RetiredContexts.GetCount(); i-- > 0;)
  {
    // Command lists in flight may still use a retired context's resources, and the present thread may still generate a
    // frame with it.
    if (!bAll && m_RetiredContexts[i].m_uiFrameId + s_uiRetireFrames > uiFrameId)
      continue;

    if (!bWaitedForPresents && m_SwapChainContext != nullptr)
    {
      ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait = {};
      wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
      m_Runtime.GetFunctions().m_Dispatch(&m_SwapChainContext, &wait.header);
      bWaitedForPresents = true;
    }

    DestroyContext(m_RetiredContexts[i].m_pContext);
    m_RetiredContexts.RemoveAtAndSwap(i);
  }
}

void plFrameGenerationBackendFSR::DestroyContext(Context* pContext)
{
  if (pContext == nullptr)
    return;

  if (pContext->m_Context != nullptr)
    m_Runtime.GetFunctions().m_DestroyContext(&pContext->m_Context, nullptr);
  PL_DEFAULT_DELETE(pContext);
}

void plFrameGenerationBackendFSR::RetireFrameGenerationContext(plUInt64 uiFrameId)
{
  if (m_pFrameGenerationContext != nullptr)
  {
    // The swap chain stops calling the retired context; the command lists of this frame and the frames in flight may
    // still use its resources, so it is destroyed a few frames later.
    ConfigureDisabled();
    m_RetiredContexts.PushBack({m_pFrameGenerationContext, uiFrameId});
    m_pFrameGenerationContext = nullptr;
  }

  for (ID3D12Resource*& pCopy : m_HudlessCopies)
  {
    if (pCopy != nullptr)
      m_RetiredResources.PushBack({pCopy, uiFrameId});
    pCopy = nullptr;
  }
}

bool plFrameGenerationBackendFSR::EnsureFrameGenerationContext(plUInt32 uiWidth, plUInt32 uiHeight, DXGI_FORMAT hudlessFormat, plUInt64 uiFrameId)
{
  if (m_pFrameGenerationContext != nullptr && m_uiContextWidth == uiWidth && m_uiContextHeight == uiHeight &&
      m_ContextHudlessFormat == hudlessFormat && m_bContextDepthInverted == m_bDepthInverted)
    return true;

  RetireFrameGenerationContext(uiFrameId);

  ffxCreateContextDescFrameGeneration createDesc = {};
  createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  createDesc.flags = 0;
  if (m_bDepthInverted)
    createDesc.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
  if (m_BackBufferFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
    createDesc.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
  createDesc.displaySize = {uiWidth, uiHeight};
  createDesc.maxRenderSize = {uiWidth, uiHeight};
  createDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(m_BackBufferFormat);

  ffxCreateContextDescFrameGenerationVersion versionDesc = {};
  versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  versionDesc.version = FFX_FRAMEGENERATION_VERSION;

  ffxCreateContextDescFrameGenerationHudless hudlessDesc = {};
  hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
  hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(hudlessFormat);

  ffxCreateBackendDX12Desc backendDesc = {};
  backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backendDesc.device = m_pDevice;

  createDesc.header.pNext = &versionDesc.header;
  versionDesc.header.pNext = &hudlessDesc.header;
  hudlessDesc.header.pNext = &backendDesc.header;

  Context* pContext = PL_DEFAULT_NEW(Context);
  pContext->m_pBackend = this;

  const ffxReturnCode_t result = m_Runtime.GetFunctions().m_CreateContext(&pContext->m_Context, &createDesc.header, nullptr);
  if (result != FFX_API_RETURN_OK)
  {
    plLog::Error("FSR: creating the frame generation context failed (error {}).", result);
    pContext->m_Context = nullptr;
    DestroyContext(pContext);
    return false;
  }

  m_pFrameGenerationContext = pContext;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC textureDesc = {};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = uiWidth;
  textureDesc.Height = uiHeight;
  textureDesc.DepthOrArraySize = 1;
  textureDesc.MipLevels = 1;
  textureDesc.Format = hudlessFormat;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  for (ID3D12Resource*& pCopy : m_HudlessCopies)
  {
    if (FAILED(m_pDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&pCopy))))
    {
      plLog::Error("FSR: creating the HUD-less copies for frame generation failed.");
      RetireFrameGenerationContext(uiFrameId);
      return false;
    }

    pCopy->SetName(L"FSR Frame Generation HUD-less");
  }

  plLog::Info("FSR: frame generation context created at frame {} ({}x{}, HUD-less format {}).", uiFrameId, uiWidth, uiHeight, static_cast<plUInt32>(hudlessFormat));

  m_uiContextWidth = uiWidth;
  m_uiContextHeight = uiHeight;
  m_ContextHudlessFormat = hudlessFormat;
  m_bContextDepthInverted = m_bDepthInverted;
  return true;
}

ffxReturnCode_t plFrameGenerationBackendFSR::DispatchFrameGeneration(ffxDispatchDescFrameGeneration* pParams, void* pUserContext)
{
  // Called on the swap chain's present thread with the context the frame was configured with.
  Context* pContext = static_cast<Context*>(pUserContext);
  return pContext->m_pBackend->m_Runtime.GetFunctions().m_Dispatch(&pContext->m_Context, &pParams->header);
}

void plFrameGenerationBackendFSR::ExecuteSceneInputs(plGALCommandEncoder& ref_encoder, const plFrameGenerationSceneParams& params)
{
  PL_LOCK(m_Mutex);

  if (!m_bEnabled || m_SwapChainContext == nullptr)
    return;

  m_bDepthInverted = params.m_bDepthInverted;

  // The context is created by the first HUD-less image, which tells its format. A context created for other inputs is
  // replaced there as well; until then this frame is not prepared and generation restarts.
  if (m_pFrameGenerationContext == nullptr || m_bContextDepthInverted != m_bDepthInverted || m_uiContextWidth != params.m_uiDisplayWidth ||
      m_uiContextHeight != params.m_uiDisplayHeight)
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  plGALNativeTexture depth;
  plGALNativeTexture velocity;
  if (pDevice->GetNativeTexture(params.m_hDepth, depth).Failed() || pDevice->GetNativeTexture(params.m_hVelocity, velocity).Failed())
    return;

  void* pCommandList = pDevice->BeginNativeCommands(ref_encoder);
  if (pCommandList == nullptr)
    return;

  ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  prepare.frameID = params.m_uiFrameId;
  prepare.flags = 0;
  prepare.commandList = pCommandList;
  prepare.renderSize = {params.m_uiInputWidth, params.m_uiInputHeight};
  prepare.jitterOffset = {params.m_vJitterPixels.x, params.m_vJitterPixels.y};
  // The engine's velocity is current minus previous in UV units; FSR wants the pixel offset to the previous position.
  prepare.motionVectorScale = {-static_cast<float>(params.m_uiInputWidth), -static_cast<float>(params.m_uiInputHeight)};
  prepare.frameTimeDelta = params.m_fDeltaTimeMs;
  prepare.reset = params.m_bReset;
  prepare.cameraNear = params.m_fCameraNear;
  prepare.cameraFar = params.m_fCameraFar;
  prepare.cameraFovAngleVertical = params.m_fCameraFovVertical;
  prepare.viewSpaceToMetersFactor = 1.0f;
  prepare.depth = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(depth.m_pResource), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  prepare.motionVectors = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(velocity.m_pResource), FFX_API_RESOURCE_STATE_COMPUTE_READ);

  const plVec3 vectors[4] = {params.m_vCameraPosition, params.m_vCameraUp, params.m_vCameraRight, params.m_vCameraForward};
  float* targets[4] = {prepare.cameraPosition, prepare.cameraUp, prepare.cameraRight, prepare.cameraForward};
  for (plUInt32 i = 0; i < 4; ++i)
  {
    targets[i][0] = vectors[i].x;
    targets[i][1] = vectors[i].y;
    targets[i][2] = vectors[i].z;
  }

  const ffxReturnCode_t result = m_Runtime.GetFunctions().m_Dispatch(&m_pFrameGenerationContext->m_Context, &prepare.header);
  pDevice->EndNativeCommands(ref_encoder);

  if (result != FFX_API_RETURN_OK)
    plLog::Warning("FSR: the frame generation prepare pass failed (error {}).", result);
  else
    m_pFrameGenerationContext->m_uiPreparedFrameId = params.m_uiFrameId;
}

void plFrameGenerationBackendFSR::ExecuteHudless(plGALCommandEncoder& ref_encoder, const plFrameGenerationHudlessParams& params)
{
  PL_LOCK(m_Mutex);

  if (!m_bEnabled || m_SwapChainContext == nullptr)
    return;

  ReleaseRetiredResources(params.m_uiFrameId, false);

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  plGALNativeTexture hudless;
  if (pDevice->GetNativeTexture(params.m_hHudless, hudless).Failed() || hudless.m_pResource == nullptr)
    return;

  const DXGI_FORMAT hudlessFormat = static_cast<DXGI_FORMAT>(plFSRGetShaderReadFormatDX12(hudless.m_uiNativeFormat));
  if (!EnsureFrameGenerationContext(params.m_uiWidth, params.m_uiHeight, hudlessFormat, params.m_uiFrameId))
    return;

  ID3D12GraphicsCommandList* pCommandList = static_cast<ID3D12GraphicsCommandList*>(pDevice->BeginNativeCommands(ref_encoder));
  if (pCommandList == nullptr)
    return;

  ID3D12Resource* pSource = static_cast<ID3D12Resource*>(hudless.m_pResource);
  ID3D12Resource* pCopy = m_HudlessCopies[params.m_uiFrameId % s_uiHudlessCopies];

  // The render graph leaves the HUD-less image in the compute read state.
  D3D12_RESOURCE_BARRIER barriers[2] = {
    MakeTransition(pSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
    MakeTransition(pCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
  };
  pCommandList->ResourceBarrier(2, barriers);

  D3D12_TEXTURE_COPY_LOCATION target = {};
  target.pResource = pCopy;
  target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = pSource;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

  const D3D12_BOX box = {0, 0, 0, params.m_uiWidth, params.m_uiHeight, 1};
  pCommandList->CopyTextureRegion(&target, 0, 0, 0, &source, &box);

  for (D3D12_RESOURCE_BARRIER& barrier : barriers)
    plMath::Swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  pCommandList->ResourceBarrier(2, barriers);

  pDevice->EndNativeCommands(ref_encoder);

  ffxConfigureDescFrameGeneration config = {};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.swapChain = m_pSwapChain;
  config.presentCallback = nullptr;
  config.presentCallbackUserContext = nullptr;
  config.frameGenerationCallback = &plFrameGenerationBackendFSR::DispatchFrameGeneration;
  config.frameGenerationCallbackUserContext = m_pFrameGenerationContext;
  // A context created by this HUD-less image has not prepared the frame (the prepare pass ran earlier with the context it
  // replaced, or not at all), so this frame is presented without a generated one.
  config.frameGenerationEnabled = m_pFrameGenerationContext->m_uiPreparedFrameId == params.m_uiFrameId;
  config.allowAsyncWorkloads = false;
  config.HUDLessColor = ffxApiGetResourceDX12(pCopy, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  config.flags = 0;
  config.onlyPresentGenerated = false;
  config.generationRect = {0, 0, static_cast<int32_t>(params.m_uiWidth), static_cast<int32_t>(params.m_uiHeight)};
  config.frameID = params.m_uiFrameId;

  const ffxReturnCode_t result = m_Runtime.GetFunctions().m_Configure(&m_pFrameGenerationContext->m_Context, &config.header);
  if (result != FFX_API_RETURN_OK)
    plLog::Warning("FSR: configuring frame generation failed (error {}).", result);

  m_uiLastFrameId = params.m_uiFrameId;
}

plFrameGenerationStats plFrameGenerationBackendFSR::GetStats() const
{
  PL_LOCK(m_Mutex);

  plFrameGenerationStats stats;
  stats.m_bActive = m_bEnabled && m_pFrameGenerationContext != nullptr;
  stats.m_uiPresentedPerFrame = stats.m_bActive ? 2 : 1;

  if (m_pFrameGenerationContext != nullptr)
  {
    FfxApiEffectMemoryUsage usage = {};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    query.gpuMemoryUsageFrameGeneration = &usage;

    ffxContext context = m_pFrameGenerationContext->m_Context;
    if (m_Runtime.GetFunctions().m_Query(&context, &query.header) == FFX_API_RETURN_OK)
      stats.m_uiVRAMBytes += usage.totalUsageInBytes;
  }

  if (m_SwapChainContext != nullptr)
  {
    FfxApiEffectMemoryUsage usage = {};
    ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12;
    query.gpuMemoryUsageFrameGenerationSwapchain = &usage;

    ffxContext context = m_SwapChainContext;
    if (m_Runtime.GetFunctions().m_Query(&context, &query.header) == FFX_API_RETURN_OK)
      stats.m_uiVRAMBytes += usage.totalUsageInBytes;
  }

  return stats;
}

void plFrameGenerationBackendFSR::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  PL_LOCK(m_Mutex);
  DestroyContexts();
  m_Runtime.Unload();
}

PL_STATICLINK_FILE(FSRPlugin, FSRPlugin_FSRFrameGenerationDX12);
