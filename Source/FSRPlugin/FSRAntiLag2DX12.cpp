#include <FSRPlugin/FSRPluginPCH.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <RendererCore/FrameGeneration/LowLatency.h>
#include <RendererFoundation/Device/Device.h>

#include <d3d12.h>
#include <ffx_antilag2_dx12.h>

/// AMD Anti-Lag 2 on D3D12. The Radeon driver delays the start of a frame so the CPU does not run ahead of the GPU.
/// Only AMD GPUs whose driver offers the interface support it.
class plLowLatencyBackendAntiLag2 : public plLowLatencyBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plLowLatencyBackendAntiLag2, plLowLatencyBackend);

public:
  ~plLowLatencyBackendAntiLag2();

  virtual plStringView GetName() const override { return "AntiLag2"; }
  virtual plStringView GetDisplayName() const override { return "AMD Anti-Lag 2"; }
  virtual plInt32 GetAutoPriority() const override { return 10; }

  virtual plLowLatencyCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual void SetMode(plGALDevice& ref_device, plLowLatencyMode::Enum mode, float fFrameLimitFps) override;
  virtual void Sleep(plUInt64 uiFrameId) override;
  virtual void SetMarker(plLatencyMarker::Enum marker, plUInt64 uiFrameId) override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  AMD::AntiLag2DX12::Context m_Context = {};
  bool m_bQueried = false;
  bool m_bInitialized = false;
  bool m_bEnabled = false;
  unsigned int m_uiMaxFps = 0;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plLowLatencyBackendAntiLag2, 1, plRTTIDefaultAllocator<plLowLatencyBackendAntiLag2>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plLowLatencyBackendAntiLag2::~plLowLatencyBackendAntiLag2()
{
  if (m_bInitialized)
    AMD::AntiLag2DX12::DeInitialize(&m_Context);
}

plLowLatencyCapabilities plLowLatencyBackendAntiLag2::QueryCapabilities(plGALDevice& ref_device)
{
  plLowLatencyCapabilities caps;
  caps.m_bSupportsBoost = false;
  caps.m_bReportsLatency = false;

  if (!m_bQueried)
  {
    m_bQueried = true;

    const plGALNativeDevice native = ref_device.GetNativeDevice();
    if (native.m_Api == plGALNativeApi::D3D12 && native.m_uiVendorId == 0x1002)
    {
      // Succeeds only when the AMD driver exposes the Anti-Lag 2 interface for this device.
      m_bInitialized = AMD::AntiLag2DX12::Initialize(&m_Context, static_cast<ID3D12Device*>(native.m_pDevice)) == S_OK;
    }
  }

  caps.m_bSupported = m_bInitialized;
  return caps;
}

void plLowLatencyBackendAntiLag2::SetMode(plGALDevice& ref_device, plLowLatencyMode::Enum mode, float fFrameLimitFps)
{
  PL_IGNORE_UNUSED(ref_device);

  // Anti-Lag 2 has no boost mode; Boost runs as On.
  m_bEnabled = mode != plLowLatencyMode::Off;
  m_uiMaxFps = fFrameLimitFps > 0.0f ? static_cast<unsigned int>(fFrameLimitFps + 0.5f) : 0u;

  // Applies the state right away; Sleep keeps it current.
  if (m_bInitialized && !m_bEnabled)
    AMD::AntiLag2DX12::Update(&m_Context, false, 0);
}

void plLowLatencyBackendAntiLag2::Sleep(plUInt64 uiFrameId)
{
  PL_IGNORE_UNUSED(uiFrameId);

  if (!m_bInitialized || !m_bEnabled)
    return;

  // Inserts the driver's delay before the input of the frame is sampled.
  AMD::AntiLag2DX12::Update(&m_Context, true, m_uiMaxFps);
}

void plLowLatencyBackendAntiLag2::SetMarker(plLatencyMarker::Enum marker, plUInt64 uiFrameId)
{
  PL_IGNORE_UNUSED(uiFrameId);

  if (!m_bInitialized || !m_bEnabled)
    return;

  // With frame generation the driver needs to know where a rendered frame ends and which presents are rendered ones.
  // The engine presents rendered frames only; the frame generation swap chain presents the generated ones itself.
  switch (marker)
  {
    case plLatencyMarker::RenderSubmitEnd:
      AMD::AntiLag2DX12::MarkEndOfFrameRendering(&m_Context);
      break;

    case plLatencyMarker::PresentStart:
      AMD::AntiLag2DX12::SetFrameGenFrameType(&m_Context, false);
      break;

    default:
      break;
  }
}

void plLowLatencyBackendAntiLag2::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  if (m_bInitialized)
    AMD::AntiLag2DX12::DeInitialize(&m_Context);

  m_Context = {};
  m_bInitialized = false;
  m_bQueried = false;
  m_bEnabled = false;
}

PL_STATICLINK_FILE(FSRPlugin, FSRPlugin_FSRAntiLag2DX12);
