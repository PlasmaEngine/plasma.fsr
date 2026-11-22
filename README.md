# plasma.fsr — AMD FSR upscaler

Adds **AMD FSR** as a backend of the engine's temporal upscaler framework (`plUpscalerBackend`, RendererCore/Upscaling).

- Select it with `Rendering.Upscaler = FSR` or in the game settings (Graphics → Upscaler). `Auto` picks the best supported backend.
- Mode: `Rendering.Upscaler.Quality` (0 native AA … 4 ultra performance, 5 custom). Dynamic resolution: `Rendering.DynamicResolution.Enable`.
- Several upscaler packages can be installed at the same time; the setting chooses between them at run time. Without any, the engine's built-in TSR is available.

Platform: D3D12 and Vulkan (Windows). Ships AMD's signed FidelityFX runtime (amd_fidelityfx_loader/upscaler for dx12 and vk, from the SDK submodule ThirdParty/FidelityFX-SDK) under the AMD FidelityFX licence. The driver picks FSR 4 on supported Radeon GPUs and FSR 3.1 elsewhere.

Clone with `git clone --recurse-submodules` (or run `git submodule update --init`): the vendor SDK is the submodule under `ThirdParty/`, pinned to a release. Build against an engine SDK: set `PLASMA_SDK_ROOT` to the engine root, then `plasmabuild build FSRPlugin` in this directory.
