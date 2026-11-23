#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRRuntime.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Strings/StringConversion.h>

namespace
{
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
} // namespace

void plFSRMessage(uint32_t type, const wchar_t* szMessage)
{
  plStringUtf8 sMessage(szMessage);
  if (type == FFX_API_MESSAGE_TYPE_ERROR)
    plLog::Error("FSR: {}", sMessage.GetData());
  else
    plLog::Warning("FSR: {}", sMessage.GetData());
}

bool plFSRRuntime::Load(plGALNativeApi::Enum api, plArrayPtr<const char* const> effects)
{
  if (m_pLoader != nullptr)
    return true;

  if (api != plGALNativeApi::D3D12 && api != plGALNativeApi::Vulkan)
    return false;

  for (const char* szEffect : effects)
  {
    HMODULE hEffect = LoadFromPluginDirectory(szEffect);
    if (hEffect == nullptr)
    {
      plLog::Warning("FSR: '{}' could not be loaded (error {}).", szEffect, static_cast<plUInt32>(GetLastError()));
      continue;
    }

    m_Effects.PushBack(hEffect);
  }

  HMODULE hLoader = LoadFromPluginDirectory(api == plGALNativeApi::D3D12 ? "amd_fidelityfx_loader_dx12.dll" : "amd_fidelityfx_loader_vk.dll");
  if (hLoader == nullptr)
  {
    plLog::Warning("FSR: the FidelityFX runtime could not be loaded (error {}).", static_cast<plUInt32>(GetLastError()));
    Unload();
    return false;
  }

  m_pLoader = hLoader;
  m_Functions.m_CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(hLoader, "ffxCreateContext"));
  m_Functions.m_DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(hLoader, "ffxDestroyContext"));
  m_Functions.m_Configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(hLoader, "ffxConfigure"));
  m_Functions.m_Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(hLoader, "ffxDispatch"));
  m_Functions.m_Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(hLoader, "ffxQuery"));

  if (m_Functions.m_CreateContext == nullptr || m_Functions.m_DestroyContext == nullptr || m_Functions.m_Configure == nullptr ||
      m_Functions.m_Dispatch == nullptr || m_Functions.m_Query == nullptr)
  {
    plLog::Warning("FSR: the FidelityFX loader is missing entry points.");
    Unload();
    return false;
  }

  return true;
}

void plFSRRuntime::Unload()
{
  if (m_pLoader != nullptr)
    FreeLibrary(static_cast<HMODULE>(m_pLoader));

  for (void* pEffect : m_Effects)
    FreeLibrary(static_cast<HMODULE>(pEffect));

  m_pLoader = nullptr;
  m_Effects.Clear();
  m_Functions = {};
}

PL_STATICLINK_FILE(FSRPlugin, FSRPlugin_FSRRuntime);
