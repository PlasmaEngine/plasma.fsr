using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class FSRPluginTarget : TargetRules
{
    public FSRPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plFSRPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("FSRPluginModule");
    }
}
