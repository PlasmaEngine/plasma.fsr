#pragma once

#include <Foundation/Containers/HybridArray.h>
#include <RendererFoundation/Device/Device.h>
#include <ffx_api.h>
#include <ffx_api_types.h>

/// The FidelityFX API entry points of the loader.
struct plFSRFunctions
{
  PfnFfxCreateContext m_CreateContext = nullptr;
  PfnFfxDestroyContext m_DestroyContext = nullptr;
  PfnFfxConfigure m_Configure = nullptr;
  PfnFfxDispatch m_Dispatch = nullptr;
  PfnFfxQuery m_Query = nullptr;
};

/// Loads the FidelityFX loader of one graphics API and the effect binaries it serves from the plugin's directory.
///
/// The loader finds an effect binary by name, which resolves to the copy already in the process, so every effect a
/// backend uses is loaded before the loader. Each backend owns one instance; the OS reference counts the modules.
class plFSRRuntime
{
public:
  plFSRRuntime() = default;
  ~plFSRRuntime() { Unload(); }

  plFSRRuntime(const plFSRRuntime&) = delete;
  plFSRRuntime& operator=(const plFSRRuntime&) = delete;

  /// Loads the loader of the API and the given effect binaries (file names next to the plugin). Returns whether the
  /// loader and its entry points are available; an effect that fails to load is only logged.
  bool Load(plGALNativeApi::Enum api, plArrayPtr<const char* const> effects);
  void Unload();

  bool IsLoaded() const { return m_pLoader != nullptr; }
  const plFSRFunctions& GetFunctions() const { return m_Functions; }

private:
  void* m_pLoader = nullptr;
  plHybridArray<void*, 4> m_Effects;
  plFSRFunctions m_Functions;
};

/// The message callback every FidelityFX context of the plugin logs through.
void plFSRMessage(uint32_t type, const wchar_t* szMessage);
