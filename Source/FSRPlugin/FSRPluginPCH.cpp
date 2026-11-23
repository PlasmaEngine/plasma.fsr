#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRPluginDLL.h>
#include <Foundation/Configuration/Plugin.h>

PL_STATICLINK_LIBRARY(FSRPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(FSRPlugin_FSRAntiLag2DX12);
  PL_STATICLINK_REFERENCE(FSRPlugin_FSRFrameGenerationDX12);
  PL_STATICLINK_REFERENCE(FSRPlugin_FSRRayRegenerationDX12);
  PL_STATICLINK_REFERENCE(FSRPlugin_FSRRuntime);
  PL_STATICLINK_REFERENCE(FSRPlugin_FSRUpscaler);
}
