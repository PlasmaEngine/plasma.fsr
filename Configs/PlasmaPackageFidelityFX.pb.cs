// #pl-version 1
//
// The FidelityFX API (FSR upscaler, frame generation, Ray Regeneration denoiser, Anti-Lag 2) this package builds
// against and ships. ThirdParty/FidelityFX-SDK is a git submodule
// of AMD's SDK repository, pinned to a release; its signed binaries are copied next to the plugin, which loads them at
// run time.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageFidelityFX
{
    private static readonly string[] s_RuntimeFiles =
    {
        "amd_fidelityfx_loader_dx12.dll",
        "amd_fidelityfx_upscaler_dx12.dll",
        "amd_fidelityfx_loader_vk.dll",
        "amd_fidelityfx_upscaler_vk.dll",
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_denoiser_dx12.dll",
    };

    public static void ConfigureConsumer(ModuleRules rules, BuildContext context)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("The FSR package currently ships Windows binaries only.");
        }

        var sdkRoot = Path.Combine(context.ProjectRoot, "ThirdParty", "FidelityFX-SDK", "Kits", "FidelityFX");
        if (!Directory.Exists(Path.Combine(sdkRoot, "api")))
        {
            throw new DirectoryNotFoundException("ThirdParty/FidelityFX-SDK is empty. Run 'git submodule update --init' in the package directory.");
        }

        // The SDK's own layout: its headers include each other by relative path.
        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "api", "include"));
        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "upscalers", "include"));
        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "framegeneration", "include"));
        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "denoisers", "include"));

        // Anti-Lag 2 is a header-only interface to the Radeon driver.
        rules.PrivateIncludePaths.Add(Path.Combine(sdkRoot, "framegeneration", "fsr3", "dx12", "antilag2"));

        // The Vulkan types of ffx_api_vk.h come from the engine's Vulkan headers.
        rules.PrivateIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty", "Vulkan-Headers", "include"));

        var binaryDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Directory.CreateDirectory(binaryDirectory);
        foreach (var file in s_RuntimeFiles)
        {
            CopyIfDifferent(Path.Combine(sdkRoot, "signedbin", file), Path.Combine(binaryDirectory, file));
        }
        CopyIfDifferent(Path.Combine(sdkRoot, "docs", "license.md"), Path.Combine(binaryDirectory, "FidelityFX-LICENSE.txt"));
    }

    private static void CopyIfDifferent(string source, string target)
    {
        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"Missing FidelityFX file '{source}'.", source);
        }

        if (File.Exists(target))
        {
            var from = new FileInfo(source);
            var to = new FileInfo(target);
            if (from.Length == to.Length && from.LastWriteTimeUtc <= to.LastWriteTimeUtc)
            {
                return;
            }
        }

        File.Copy(source, target, true);
    }
}
