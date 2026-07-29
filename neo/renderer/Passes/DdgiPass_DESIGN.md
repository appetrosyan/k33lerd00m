# DDGI for RBDOOM-3-BFG — Implementation Plan

Dynamic Diffuse Global Illumination (Majercik/McGuire 2019, RTXGI-style) on the
NVRHI backend. Ray-traced irradiance probes replacing/augmenting the baked light
grid (`RenderWorld_lightgrid.cpp`).

## Design constraints
- Backend: NVRHI (Vulkan + DX12). RT via inline **ray queries in a compute pass**
  (reuses the `SsaoPass` idiom; no RT-pipeline / shader-table needed).
- Diffuse GI only. Specular stays on existing SSR path.
- Gated behind `r_useDDGI`; baked light grid remains the default fallback.
- Probe data reuses the existing octahedral irradiance atlas layout
  (`LIGHTGRID_IRRADIANCE_SIZE`) so `ambient_lightgrid_IBL.ps.hlsl` can sample it
  unchanged (M4). Plus a parallel distance atlas (mean, mean^2) for Chebyshev.

## Verified integration points (from recon)
- Light-grid probe metadata: `RenderWorld_local.h:63-110` (`lightGridPoint_t`,
  `LightGrid`), per-area in `portalArea_s`.
- Atlas sampling shader: `shaders/builtin/lighting/ambient_lightgrid_IBL.ps.hlsl`
  (trilinear 8-corner octahedral, tex fetch ~line 333).
- Grid params wired at draw time: `RenderBackend.cpp:1190-1273`
  (gated by `r_useLightGrid` at :1190; swap in `r_useDDGI` at M4).
- NVRHI compute-pass template: `Passes/SsaoPass.cpp` (FindShader/GetShader,
  BindingLayoutDesc, createComputePipeline, createBindingSet, dispatch).
- `nvrhi::rt` API: `extern/nvrhi/include/nvrhi/nvrhi.h` — AccelStructDesc :1725,
  IAccelStruct :1748, `BindingSetItem::RayTracingAccelStruct` :2194,
  build{Bottom,Top}LevelAccelStruct :2945/:2948, createAccelStruct :3087.
  Feature gate: `Feature::RayQuery`, `Feature::RayTracingAccelStruct` (:2808-2812).
- Geometry for BLAS: shared vertex cache; unpack idiom at
  `RenderBackend_NVRHI.cpp:320-381`. **Gap:** static vtx/idx buffers lack
  `desc.isAccelStructBuildInput` (`BufferObject_NVRHI.cpp:198`) — must set it on
  the static cache (branch already handled at :116) or copy into build buffers.
- Pass alloc/free: `RenderBackend_NVRHI.cpp:2009-2048 / 2349-2364`; member on
  `idRenderBackend` (`RenderBackend.h:383`).
- cvar idiom: `RenderSystem_init.cpp:291` (`r_useLightGrid`).

## Milestones (each compiles + is independently verifiable)

### M0 — Scaffold  ✅ DONE
- `r_useDDGI` (RenderSystem_init) + pass-local `r_ddgi*` cvars.
- `Passes/DdgiPass.{h,cpp}` + `DdgiPass_cb.h`; member wired on idRenderBackend,
  lazily allocated, logs RT support on enable. `Render()` early-outs.
- No behavior change. Builds clean.

### M1 — Acceleration structures (shared RT prerequisite)
- Set `isAccelStructBuildInput` on the static vertex/index cache.
- BLAS per static idRenderModel surface; TLAS per frame from visible entities.
- Skinned md5: BLAS refit each frame.
- Verify: debug ray-query compute writing hit distance to a texture.

### M2 — Probe update pass
- Compute + inline ray query: N rays/probe, per-frame rotated dirs.
- Shade hit: direct light + sample PREVIOUS irradiance (multi-bounce). Miss: sky.
- Write raw radiance to a per-probe ray-radiance buffer.

### M3 — Probe integration / blend
- Integrate rays -> octahedral irradiance + distance(mean,mean^2) atlases.
- Border-texel copy; temporal blend (r_ddgiHysteresis).

### M4 — Shading integration
- Sample 8 probes in the ambient shader; cosine * Chebyshev(distance) *
  trilinear weighting; swap in for light-grid ambient when `r_useDDGI`.

### M5 — Leak control + robustness
- Probe relocation (offset out of geometry); classification (skip solid);
  cascaded volumes around camera.

### M6 — Tuning + debug viz
- Probe debug spheres; tune hysteresis, normal/view bias, ray count.
