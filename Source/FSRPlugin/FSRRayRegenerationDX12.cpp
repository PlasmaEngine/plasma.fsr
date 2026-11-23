#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRNative.h>
#include <FSRPlugin/FSRRuntime.h>
#include <FSRPlugin/FSRUpscalerContext.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <RendererCore/Upscaling/RayReconstruction.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_denoiser.h>

/// AMD FSR Ray Regeneration on D3D12, paired with the FSR upscaler.
///
/// Ray Regeneration is a denoiser only: it neither upscales nor resolves jitter. One Execute therefore records three
/// steps on the same command list:
/// 1. A compute pass turns the engine's guides into the denoiser's inputs: linear depth, motion vectors with the linear
///    depth delta, octahedral normals with roughness, and the noisy colour with a hit distance in alpha.
/// 2. The denoiser runs on the noisy lit colour as one indirect diffuse signal. The colour is already modulated by the
///    surface albedo, so the albedo guide is white.
/// 3. The FSR upscaler upscales the denoised colour to the output.
///
/// The denoiser needs an AMD Radeon RX 9000 series GPU; IsSupported asks the runtime whether it can create a context.
class plRayReconstructionBackendFSR : public plRayReconstructionBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plRayReconstructionBackendFSR, plRayReconstructionBackend);

public:
  virtual plStringView GetName() const override { return "FSRRayRegeneration"; }
  virtual plStringView GetUpscalerName() const override { return "FSR"; }

  virtual bool IsSupported(plGALDevice& ref_device) override;
  virtual plUniquePtr<plRayReconstructionContext> CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc) override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  bool LoadRuntime();

  plFSRRuntime m_Runtime;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plRayReconstructionBackendFSR, 1, plRTTIDefaultAllocator<plRayReconstructionBackendFSR>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  // Converts the engine's guides into the denoiser inputs. Matrices are the engine's column-vector matrices.
  constexpr const char* s_szConvertShader = R"(
cbuffer Constants : register(b0)
{
  float4x4 ClipToView;
  float4x4 ViewToClip;
  float4x4 ClipToPrevClip;
  uint2 RenderSize;
  float DefaultHitDistance;
  uint HasHitDistance;
};

Texture2D<float> Depth : register(t0);
Texture2D<float4> Velocity : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
Texture2D<float4> Color : register(t3);
Texture2D<float4> HitDistance : register(t4);

RWTexture2D<float> OutLinearDepth : register(u0);
RWTexture2D<float4> OutMotion : register(u1);
RWTexture2D<float4> OutNormals : register(u2);
RWTexture2D<float4> OutSignal : register(u3);
RWTexture2D<float4> OutAlbedo : register(u4);

float2 NormalToOctahedronUv(float3 N)
{
  N.xy /= abs(N.x) + abs(N.y) + abs(N.z);
  float2 k = float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
  if (N.z < 0.0)
    N.xy = (1.0 - abs(N.yx)) * k;
  return N.xy * 0.5 + 0.5;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= RenderSize.x || id.y >= RenderSize.y)
    return;

  float2 uv = (float2(id.xy) + 0.5) / float2(RenderSize);
  float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, Depth[id.xy], 1.0);

  float4 viewPos = mul(ClipToView, ndc);
  viewPos /= viewPos.w;
  float4 clipPos = mul(ViewToClip, viewPos);
  float4 prevClipPos = mul(ClipToPrevClip, clipPos);

  OutLinearDepth[id.xy] = clipPos.w;

  // The engine stores current minus previous UV; the denoiser wants previous minus current.
  OutMotion[id.xy] = float4(-Velocity[id.xy].xy, prevClipPos.w - clipPos.w, 0.0);

  float4 normalRoughness = NormalRoughness[id.xy];
  float normalLength = length(normalRoughness.xyz);
  float3 normal = normalLength > 0.0001 ? normalRoughness.xyz / normalLength : float3(0.0, 0.0, 1.0);
  OutNormals[id.xy] = float4(NormalToOctahedronUv(normal), saturate(normalRoughness.w), 0.0);

  float hitDistance = HasHitDistance != 0 ? HitDistance[id.xy].x : DefaultHitDistance;
  OutSignal[id.xy] = float4(Color[id.xy].rgb, hitDistance);
  OutAlbedo[id.xy] = 1.0;
}
)";

  struct ConvertConstants
  {
    float m_ClipToView[16];
    float m_ViewToClip[16];
    float m_ClipToPrevClip[16];
    plUInt32 m_uiRenderSize[2];
    float m_fDefaultHitDistance;
    plUInt32 m_uiHasHitDistance;
  };

  static_assert(sizeof(ConvertConstants) % 4 == 0);
  constexpr plUInt32 s_uiConstantCount = sizeof(ConvertConstants) / 4;

  constexpr plUInt32 s_uiInputCount = 5;
  constexpr plUInt32 s_uiOutputCount = 5;
  constexpr plUInt32 s_uiDescriptorsPerSet = s_uiInputCount + s_uiOutputCount;

  // Descriptor sets the conversion cycles through, so a set is not rewritten while a frame in flight still reads it.
  constexpr plUInt32 s_uiDescriptorSets = 8;

  enum Output
  {
    LinearDepth,
    Motion,
    Normals,
    Signal,
    Albedo,
  };

  constexpr DXGI_FORMAT s_OutputFormats[s_uiOutputCount] = {
    DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM,
  };

  constexpr const wchar_t* s_OutputNames[s_uiOutputCount] = {
    L"FSR Ray Regeneration Linear Depth",
    L"FSR Ray Regeneration Motion",
    L"FSR Ray Regeneration Normals",
    L"FSR Ray Regeneration Signal",
    L"FSR Ray Regeneration Albedo",
  };

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

  template <typename T>
  void SafeRelease(T*& ref_pObject)
  {
    if (ref_pObject != nullptr)
      ref_pObject->Release();
    ref_pObject = nullptr;
  }

  bool CreateDenoiserContext(const plFSRFunctions& functions, ID3D12Device* pDevice, plUInt32 uiMaxWidth, plUInt32 uiMaxHeight, ffxContext& out_context)
  {
    ffxCreateContextDescDenoiser createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    createDesc.version = FFX_DENOISER_VERSION;
    createDesc.maxRenderSize = {uiMaxWidth, uiMaxHeight};
    createDesc.signalFlags = FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE;
    createDesc.checkerboardSignalFlags = FFX_DENOISER_SIGNAL_NONE;
    createDesc.flags = 0;

    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = pDevice;
    createDesc.header.pNext = &backendDesc.header;

    out_context = nullptr;
    if (functions.m_CreateContext(&out_context, &createDesc.header, nullptr) != FFX_API_RETURN_OK)
    {
      out_context = nullptr;
      return false;
    }
    return true;
  }

  class plFSRRayRegenerationContext : public plRayReconstructionContext
  {
  public:
    plFSRRayRegenerationContext(plGALDevice& ref_device, const plFSRFunctions& functions, const plUpscalerContextDesc& desc)
      : m_Device(ref_device)
      , m_Functions(functions)
      , m_Upscaler(ref_device, functions, desc)
    {
      m_Desc = desc;
    }

    ~plFSRRayRegenerationContext()
    {
      if (m_Denoiser != nullptr)
        m_Functions.m_DestroyContext(&m_Denoiser, nullptr);

      for (ID3D12Resource*& pOutput : m_Outputs)
        SafeRelease(pOutput);
      SafeRelease(m_pDenoised);
      SafeRelease(m_pDescriptorHeap);
      SafeRelease(m_pPipeline);
      SafeRelease(m_pRootSignature);
    }

    bool Create()
    {
      m_pDevice = static_cast<ID3D12Device*>(m_Device.GetNativeDevice().m_pDevice);

      if (!m_Upscaler.Create())
        return false;

      if (!CreateDenoiserContext(m_Functions, m_pDevice, m_Desc.m_uiMaxRenderWidth, m_Desc.m_uiMaxRenderHeight, m_Denoiser))
      {
        plLog::Error("FSR: creating the Ray Regeneration context failed.");
        return false;
      }

      return CreatePipeline() && CreateTextures();
    }

    virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params, const plRayReconstructionParams& rr) override
    {
      ID3D12GraphicsCommandList* pCommandList = static_cast<ID3D12GraphicsCommandList*>(m_Device.BeginNativeCommands(ref_encoder));
      if (pCommandList == nullptr)
        return PL_FAILURE;

      plResult result = Convert(pCommandList, params, rr);
      if (result.Succeeded())
        result = Denoise(pCommandList, params, rr);
      if (result.Succeeded())
        result = Upscale(pCommandList, params);

      m_Device.EndNativeCommands(ref_encoder);

      m_vPrevCameraPosition = rr.m_vCameraPosition;
      m_bHasPrevCamera = true;
      return result;
    }

  private:
    bool CreatePipeline()
    {
      HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
      auto serializeRootSignature = hD3D12 != nullptr ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(hD3D12, "D3D12SerializeRootSignature")) : nullptr;

      HMODULE hCompiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      auto compile = hCompiler != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(hCompiler, "D3DCompile")) : nullptr;

      if (serializeRootSignature == nullptr || compile == nullptr)
      {
        plLog::Error("FSR: Ray Regeneration needs d3d12.dll and d3dcompiler_47.dll.");
        if (hCompiler != nullptr)
          FreeLibrary(hCompiler);
        return false;
      }

      D3D12_DESCRIPTOR_RANGE ranges[2] = {};
      ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors = s_uiInputCount;
      ranges[0].OffsetInDescriptorsFromTableStart = 0;
      ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      ranges[1].NumDescriptors = s_uiOutputCount;
      ranges[1].OffsetInDescriptorsFromTableStart = s_uiInputCount;

      D3D12_ROOT_PARAMETER parameters[2] = {};
      parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      parameters[0].Constants.Num32BitValues = s_uiConstantCount;
      parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[1].DescriptorTable.NumDescriptorRanges = 2;
      parameters[1].DescriptorTable.pDescriptorRanges = ranges;
      parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

      D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
      rootDesc.NumParameters = 2;
      rootDesc.pParameters = parameters;

      ID3DBlob* pRootBlob = nullptr;
      ID3DBlob* pShaderBlob = nullptr;
      ID3DBlob* pErrors = nullptr;
      bool bSuccess = false;

      if (FAILED(serializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pRootBlob, &pErrors)))
      {
        plLog::Error("FSR: serializing the Ray Regeneration root signature failed.");
      }
      else if (FAILED(compile(s_szConvertShader, plStringUtils::GetStringElementCount(s_szConvertShader), "FSRRayRegenerationConvert", nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pShaderBlob, &pErrors)))
      {
        plLog::Error("FSR: compiling the Ray Regeneration conversion shader failed: {}", pErrors != nullptr ? static_cast<const char*>(pErrors->GetBufferPointer()) : "");
      }
      else if (FAILED(m_pDevice->CreateRootSignature(0, pRootBlob->GetBufferPointer(), pRootBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature))))
      {
        plLog::Error("FSR: creating the Ray Regeneration root signature failed.");
      }
      else
      {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
        pipelineDesc.pRootSignature = m_pRootSignature;
        pipelineDesc.CS = {pShaderBlob->GetBufferPointer(), pShaderBlob->GetBufferSize()};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = s_uiDescriptorsPerSet * s_uiDescriptorSets;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(m_pDevice->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&m_pPipeline))))
          plLog::Error("FSR: creating the Ray Regeneration conversion pipeline failed.");
        else if (FAILED(m_pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pDescriptorHeap))))
          plLog::Error("FSR: creating the Ray Regeneration descriptor heap failed.");
        else
          bSuccess = true;
      }

      SafeRelease(pRootBlob);
      SafeRelease(pShaderBlob);
      SafeRelease(pErrors);
      FreeLibrary(hCompiler);

      if (bSuccess)
        m_uiDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      return bSuccess;
    }

    bool CreateTexture(DXGI_FORMAT format, const wchar_t* szName, ID3D12Resource*& out_pResource)
    {
      D3D12_HEAP_PROPERTIES heap = {};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;

      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = m_Desc.m_uiMaxRenderWidth;
      desc.Height = m_Desc.m_uiMaxRenderHeight;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

      if (FAILED(m_pDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out_pResource))))
      {
        plLog::Error("FSR: creating a Ray Regeneration texture failed.");
        return false;
      }

      out_pResource->SetName(szName);
      return true;
    }

    bool CreateTextures()
    {
      for (plUInt32 i = 0; i < s_uiOutputCount; ++i)
      {
        if (!CreateTexture(s_OutputFormats[i], s_OutputNames[i], m_Outputs[i]))
          return false;
      }

      return CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"FSR Ray Regeneration Denoised", m_pDenoised);
    }

    void WriteInputDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, plGALTextureHandle hTexture)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
      desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

      plGALNativeTexture texture;
      if (hTexture.IsInvalidated() || m_Device.GetNativeTexture(hTexture, texture).Failed() || texture.m_pResource == nullptr)
      {
        // Reads return zero.
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        m_pDevice->CreateShaderResourceView(nullptr, &desc, handle);
        return;
      }

      // The FSR upscaler does not take texture arrays, so every input is a plain 2D texture.
      desc.Format = static_cast<DXGI_FORMAT>(plFSRGetShaderReadFormatDX12(texture.m_uiNativeFormat));
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipLevels = 1;

      m_pDevice->CreateShaderResourceView(static_cast<ID3D12Resource*>(texture.m_pResource), &desc, handle);
    }

    plResult Convert(ID3D12GraphicsCommandList* pCommandList, const plUpscalerExecuteParams& params, const plRayReconstructionParams& rr)
    {
      const plUInt32 uiSet = m_uiFrameIndex % s_uiDescriptorSets;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_pDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
      cpu.ptr += static_cast<SIZE_T>(uiSet * s_uiDescriptorsPerSet) * m_uiDescriptorSize;
      gpu.ptr += static_cast<UINT64>(uiSet * s_uiDescriptorsPerSet) * m_uiDescriptorSize;

      const plGALTextureHandle inputs[s_uiInputCount] = {params.m_hDepth, params.m_hVelocity, rr.m_hNormalRoughness, params.m_hColor, rr.m_hSpecularHitDistance};
      for (plUInt32 i = 0; i < s_uiInputCount; ++i)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
        handle.ptr += static_cast<SIZE_T>(i) * m_uiDescriptorSize;
        WriteInputDescriptor(handle, inputs[i]);
      }

      for (plUInt32 i = 0; i < s_uiOutputCount; ++i)
      {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format = s_OutputFormats[i];
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
        handle.ptr += static_cast<SIZE_T>(s_uiInputCount + i) * m_uiDescriptorSize;
        m_pDevice->CreateUnorderedAccessView(m_Outputs[i], nullptr, &desc, handle);
      }

      ConvertConstants constants = {};
      plMemoryUtils::Copy(constants.m_ClipToView, rr.m_ClipToView.m_fElementsCM, 16);
      plMemoryUtils::Copy(constants.m_ViewToClip, rr.m_ViewToClip.m_fElementsCM, 16);
      plMemoryUtils::Copy(constants.m_ClipToPrevClip, rr.m_ClipToPrevClip.m_fElementsCM, 16);
      constants.m_uiRenderSize[0] = params.m_uiRenderWidth;
      constants.m_uiRenderSize[1] = params.m_uiRenderHeight;
      // Without a hit distance the denoiser assumes rays travelled about one unit.
      constants.m_fDefaultHitDistance = 1.0f;
      constants.m_uiHasHitDistance = rr.m_hSpecularHitDistance.IsInvalidated() ? 0u : 1u;

      ID3D12DescriptorHeap* heaps[] = {m_pDescriptorHeap};
      pCommandList->SetDescriptorHeaps(1, heaps);
      pCommandList->SetComputeRootSignature(m_pRootSignature);
      pCommandList->SetPipelineState(m_pPipeline);
      pCommandList->SetComputeRoot32BitConstants(0, s_uiConstantCount, &constants, 0);
      pCommandList->SetComputeRootDescriptorTable(1, gpu);
      pCommandList->Dispatch((params.m_uiRenderWidth + 7) / 8, (params.m_uiRenderHeight + 7) / 8, 1);

      // The denoiser reads the converted inputs.
      D3D12_RESOURCE_BARRIER barriers[s_uiOutputCount];
      for (plUInt32 i = 0; i < s_uiOutputCount; ++i)
        barriers[i] = MakeTransition(m_Outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      pCommandList->ResourceBarrier(s_uiOutputCount, barriers);

      return PL_SUCCESS;
    }

    plResult Denoise(ID3D12GraphicsCommandList* pCommandList, const plUpscalerExecuteParams& params, const plRayReconstructionParams& rr)
    {
      ffxDispatchDescDenoiser dispatch = {};
      dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
      dispatch.commandList = pCommandList;
      dispatch.linearDepth = ffxApiGetResourceDX12(m_Outputs[LinearDepth], FFX_API_RESOURCE_STATE_COMPUTE_READ);
      dispatch.motionVectors = ffxApiGetResourceDX12(m_Outputs[Motion], FFX_API_RESOURCE_STATE_COMPUTE_READ);
      dispatch.normals = ffxApiGetResourceDX12(m_Outputs[Normals], FFX_API_RESOURCE_STATE_COMPUTE_READ);
      dispatch.diffuseAlbedo = ffxApiGetResourceDX12(m_Outputs[Albedo], FFX_API_RESOURCE_STATE_COMPUTE_READ);
      dispatch.motionVectorScale = {1.0f, 1.0f, 1.0f};
      dispatch.jitterOffsets = {params.m_vJitterPixels.x, params.m_vJitterPixels.y};

      const plVec3 vDelta = m_bHasPrevCamera && !params.m_bReset ? m_vPrevCameraPosition - rr.m_vCameraPosition : plVec3::MakeZero();
      dispatch.cameraPositionDelta = {vDelta.x, vDelta.y, vDelta.z};

      // The engine's column-major storage of a column-vector matrix is the row-major storage of the row-vector matrix.
      static_assert(sizeof(FfxApiMatrix4x4) == sizeof(float) * 16);
      plMemoryUtils::RawByteCopy(&dispatch.view, rr.m_WorldToView.m_fElementsCM, sizeof(FfxApiMatrix4x4));
      plMemoryUtils::RawByteCopy(&dispatch.projection, rr.m_ViewToClip.m_fElementsCM, sizeof(FfxApiMatrix4x4));

      // The sky and everything beyond the far plane passes through.
      dispatch.linearDepthBounds = {0.0f, params.m_fCameraFar * 0.999f};
      dispatch.renderSize = {params.m_uiRenderWidth, params.m_uiRenderHeight};
      dispatch.frameIndex = m_uiFrameIndex++;
      dispatch.flags = FFX_DENOISER_DISPATCH_NON_ENCODED_ALBEDO;
      if (params.m_bReset)
        dispatch.flags |= FFX_DENOISER_DISPATCH_RESET;

      ffxDispatchDescDenoiserIndirectDiffuse signal = {};
      signal.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
      signal.signal.input = ffxApiGetResourceDX12(m_Outputs[Signal], FFX_API_RESOURCE_STATE_COMPUTE_READ);
      signal.signal.output = ffxApiGetResourceDX12(m_pDenoised, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
      dispatch.header.pNext = &signal.header;

      const ffxReturnCode_t result = m_Functions.m_Dispatch(&m_Denoiser, &dispatch.header);

      D3D12_RESOURCE_BARRIER barriers[s_uiOutputCount];
      for (plUInt32 i = 0; i < s_uiOutputCount; ++i)
        barriers[i] = MakeTransition(m_Outputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      pCommandList->ResourceBarrier(s_uiOutputCount, barriers);

      if (result != FFX_API_RETURN_OK)
      {
        plLog::Warning("FSR: the Ray Regeneration dispatch failed (error {}).", result);
        return PL_FAILURE;
      }
      return PL_SUCCESS;
    }

    plResult Upscale(ID3D12GraphicsCommandList* pCommandList, const plUpscalerExecuteParams& params)
    {
      D3D12_RESOURCE_BARRIER barrier = MakeTransition(m_pDenoised, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      pCommandList->ResourceBarrier(1, &barrier);

      const FfxApiResource color = ffxApiGetResourceDX12(m_pDenoised, FFX_API_RESOURCE_STATE_COMPUTE_READ);
      const plResult result = m_Upscaler.Dispatch(pCommandList, params, &color);

      plMath::Swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      pCommandList->ResourceBarrier(1, &barrier);
      return result;
    }

    plGALDevice& m_Device;
    plFSRFunctions m_Functions;
    plFSRUpscalerContext m_Upscaler;
    ID3D12Device* m_pDevice = nullptr;
    ffxContext m_Denoiser = nullptr;

    ID3D12RootSignature* m_pRootSignature = nullptr;
    ID3D12PipelineState* m_pPipeline = nullptr;
    ID3D12DescriptorHeap* m_pDescriptorHeap = nullptr;
    plUInt32 m_uiDescriptorSize = 0;

    ID3D12Resource* m_Outputs[s_uiOutputCount] = {};
    ID3D12Resource* m_pDenoised = nullptr;

    plUInt32 m_uiFrameIndex = 0;
    plVec3 m_vPrevCameraPosition = plVec3::MakeZero();
    bool m_bHasPrevCamera = false;
  };
} // namespace

bool plRayReconstructionBackendFSR::LoadRuntime()
{
  static const char* const s_Effects[] = {"amd_fidelityfx_denoiser_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"};
  return m_Runtime.Load(plGALNativeApi::D3D12, s_Effects);
}

bool plRayReconstructionBackendFSR::IsSupported(plGALDevice& ref_device)
{
  const plGALNativeDevice native = ref_device.GetNativeDevice();
  if (native.m_Api != plGALNativeApi::D3D12 || !LoadRuntime())
    return false;

  const plFSRFunctions& functions = m_Runtime.GetFunctions();

  uint64_t uiVersionCount = 0;
  ffxQueryDescGetVersions query = {};
  query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
  query.device = native.m_pDevice;
  query.outputCount = &uiVersionCount;
  if (functions.m_Query(nullptr, &query.header) != FFX_API_RETURN_OK || uiVersionCount == 0)
    return false;

  // The runtime only creates a context on hardware that runs the denoiser.
  ffxContext context = nullptr;
  if (!CreateDenoiserContext(functions, static_cast<ID3D12Device*>(native.m_pDevice), 64, 64, context))
    return false;

  functions.m_DestroyContext(&context, nullptr);
  return true;
}

plUniquePtr<plRayReconstructionContext> plRayReconstructionBackendFSR::CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
{
  if (ref_device.GetNativeDevice().m_Api != plGALNativeApi::D3D12 || !LoadRuntime())
    return nullptr;

  plUniquePtr<plFSRRayRegenerationContext> pContext = PL_DEFAULT_NEW(plFSRRayRegenerationContext, ref_device, m_Runtime.GetFunctions(), desc);
  if (!pContext->Create())
    return nullptr;
  return pContext;
}

void plRayReconstructionBackendFSR::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  m_Runtime.Unload();
}

PL_STATICLINK_FILE(FSRPlugin, FSRPlugin_FSRRayRegenerationDX12);
