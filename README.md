# plasma.fsr — AMD FSR upscaler, frame generation, Ray Regeneration and Anti-Lag 2

Adds AMD FidelityFX backends to four engine frameworks:

| Feature | Engine framework | Setting | Platform | Hardware |
|---|---|---|---|---|
| **AMD FSR** upscaler | `plUpscalerBackend` (RendererCore/Upscaling) | `Rendering.Upscaler = FSR` | D3D12, Vulkan | Any GPU; FSR 4 on supported Radeon GPUs, FSR 3.1 elsewhere |
| **AMD FSR Frame Generation** | `plFrameGenerationBackend` (RendererCore/FrameGeneration) | `Rendering.FrameGeneration = FSR` | D3D12 | Any GPU |
| **AMD FSR Ray Regeneration** | `plRayReconstructionBackend` (RendererCore/Upscaling), paired with the FSR upscaler | `Rendering.Upscaler.RayReconstruction` | D3D12 | Radeon RX 9000 series |
| **AMD Anti-Lag 2** | `plLowLatencyBackend` (RendererCore/FrameGeneration) | `Rendering.LowLatency.Backend = AntiLag2` | D3D12 | Radeon GPUs |

- Upscaler mode: `Rendering.Upscaler.Quality` (0 native AA … 4 ultra performance, 5 custom). Dynamic resolution: `Rendering.DynamicResolution.Enable`.
- Frame generation presents one generated frame per rendered frame (`Rendering.FrameGeneration.Multiplier` 2). It replaces the game window's swap chain with the FidelityFX frame generation swap chain, so switching it on or off re-creates the swap chain. The UI is taken from the difference between the back buffer and the HUD-less scene image. It runs in game windows only, never in the editor.
- Ray Regeneration is a denoiser; it runs together with the FSR upscale. The noisy lit colour is denoised as one signal (it is already modulated by the albedo), then upscaled by FSR.
- Anti-Lag 2 is used by frame generation when it is the active low-latency backend, but frame generation does not require it.
- Several upscaler and frame generation packages can be installed at the same time; the settings choose between them at run time. Without any, the engine's built-in TSR is available.

Ships AMD's signed FidelityFX runtime (loader, upscaler, frame generation and denoiser binaries from the SDK submodule ThirdParty/FidelityFX-SDK) under the AMD FidelityFX licence.

Clone with `git clone --recurse-submodules` (or run `git submodule update --init`): the vendor SDK is the submodule under `ThirdParty/`, pinned to a release. Build against an engine SDK: set `PLASMA_SDK_ROOT` to the engine root, then `plasmabuild build FSRPlugin` in this directory.
