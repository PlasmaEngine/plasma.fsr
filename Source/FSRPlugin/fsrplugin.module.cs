using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class FSRPluginModule : ModuleRules
{
    public FSRPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/FSRPlugin", "FSRPluginPCH.h",
            "BUILDSYSTEM_BUILDING_FSRPLUGIN_LIB");

        PlasmaPackageFidelityFX.ConfigureConsumer(this, context);

        // The FidelityFX D3D12 and Vulkan headers conflict with each other; each stays in its own translation unit.
        UnityBuildExclusions.Add("FSRNativeDX12.cpp");
        UnityBuildExclusions.Add("FSRNativeVK.cpp");
        UnityBuildExclusions.Add("FSRFrameGenerationDX12.cpp");
        UnityBuildExclusions.Add("FSRAntiLag2DX12.cpp");
        UnityBuildExclusions.Add("FSRRayRegenerationDX12.cpp");
    }
}
