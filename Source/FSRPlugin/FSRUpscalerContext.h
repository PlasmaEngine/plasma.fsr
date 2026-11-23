#pragma once

#include <FSRPlugin/FSRRuntime.h>
#include <RendererCore/Upscaling/Upscaler.h>

/// One FSR upscaler instance for one view.
class plFSRUpscalerContext : public plUpscalerContext
{
public:
  plFSRUpscalerContext(plGALDevice& ref_device, const plFSRFunctions& functions, const plUpscalerContextDesc& desc);
  ~plFSRUpscalerContext();

  bool Create();

  virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params) override;

  /// Records the upscale into a native command list the caller already opened (plGALDevice::BeginNativeCommands).
  /// pColor replaces params.m_hColor when set.
  plResult Dispatch(void* pCommandList, const plUpscalerExecuteParams& params, const FfxApiResource* pColor);

private:
  plGALDevice& m_Device;
  plFSRFunctions m_Functions;
  plGALNativeApi::Enum m_Api = plGALNativeApi::None;
  ffxContext m_Context = nullptr;
};
