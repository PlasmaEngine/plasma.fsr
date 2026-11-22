#include <FSRPlugin/FSRPluginPCH.h>

#include <FSRPlugin/FSRPluginDLL.h>
#include <Foundation/Configuration/Plugin.h>

PL_STATICLINK_LIBRARY(FSRPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(FSRPlugin_FSRUpscaler);
}
