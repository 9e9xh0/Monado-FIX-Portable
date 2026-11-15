# ADR 0013 - Interface resolution independent of the 3D render scale

[XCDE] T-UI-SCALE, 2026-11-15. Implemented in the Vulkan backend. Opt-in: `xcde.ini` `ui_scale = 1`
(default 0 keeps the previous single-scale behaviour).

## Context

Until now one factor (`OutScale.sx/sy`, chosen by `xcde.ini` `resolution`) sized every scalable NVN render target,
including the presented window texture. At 1080p with the internal resolution at 1x (720p) the HUD, menus and text
were therefore drawn at 720p and upscaled with FSR 3/DLSS to the window; at 4K with 50 % they were drawn at 1080p.
The interface resolution can instead stay separate from the 3D render scale: the HUD is composed at the output
resolution, while the 3D scene is upscaled from its own render size.

## Decision

`runtime/nvn/output_scale.c` now returns two scales:

- `ui_sx/ui_sy`: the game's frame placed in the window (the old "Match display" size). The presented window texture and
  the interface use it, whatever `resolution` is chosen.
- `sx/sy`: the 3D render scale from `resolution` (1x..8x, 1.5x, 75/50/25 %, Dynamic, or the FSR 3/DLSS quality ratio).

The Vulkan backend keeps the window texture in two images (`TexBackend.split` holds the 3D-sized one). The first pass
into the window texture after the frame's scene draws is the scene composite; it is detected with the existing scene
camera tracking (`tmp_draw`) and the `tmp_hud_started` flag, and it renders into the split image. Everything else -
early full-frame writes, the title screen's HUD-in-the-same-pass case, and the HUD passes - uses the interface image.
The split image is also kept when the two scales are equal (e.g. `resolution = 1.5` on a 720p game frame at a 1080p
window) as long as a temporal upscaler is active: at the same size FSR 3 / DLSS still reconstructs the scene, and the
HUD must stay out of its input or it shakes with the camera.
At the end of the scene pass the split image is upscaled into the interface image **before the HUD passes**: the
upscaler replaces the game's stretch draw at the start of the HUD pass, and the HUD is drawn on top at output resolution.
With FSR 3/DLSS `tmp_upscale` runs at the scene transition, `window_image_to_tex`
copies/encodes its output into the interface image, and `tmp_hud_snapshot` captures that pre-HUD image for frame
generation; the HUD passes then blend over the real upscaled scene. There is no HUD delta for the split path. Without a
temporal upscaler the same transition uses FSR 1 or a bilinear blit.

This ordering is slower than the asynchronous alternative that drew the HUD over a bilinear background and recomposed
it at present: 41-43 fps against 55-56 fps on the GTX 1050 Ti at 1080p output with 720p 3D, and 30-32 fps against
~42 fps at 1080p 3D with a 1080p interface (FSR 3 at 1:1). The asynchronous HUD mixed the difference between the FSR
output and the bilinear background into translucent / anti-aliased HUD pixels. Camera-pan captures showed the text-core
temporal standard deviation dropping from ~6 to ~0.07 at 720p 3D and from ~6.9 to ~0.035 at 1080p 3D, so the slower
ordering is the default.

HUD passes still bind the scene depth. An interface-sized copy (`vk_ui_depth`) is made once per frame with a nearest
depth blit (stencil stays zero); if the format cannot be blitted it is cleared to far. The scene depth values are
resolution independent.

Limits and fallbacks:

- The 3D scene is still rendered through the ordinary scalable 16:9 targets (plus the T182 portraits, which now follow
  the interface scale). Fixed-size targets (shadow maps, small bloom tails, probes) are unchanged.
- If the scene camera is not found, no split pass is detected; the game draws the scene at the interface scale, exactly
  as before this change.
- If the scene pass depth does not match the split colour size, the pass is treated as interface content instead.
- Metal keeps one scale for now; `renderer_set_ui_scale` there is a no-op and the presented texture is still named.
- At an aspect other than 16:9 the interface is still the centred 16:9 box (the V9 wide-box logic); the scene keeps the
  widened shape.

## Why not patch XCDE's frame size

Patching the game's own frame size was considered. XCDE's NVN window textures are 1280x720, but its engine has no
frame-size table that can be raised safely, and its memory layout has fixed-size assumptions. The backend split needs
no game addresses or version-specific patches and keeps the HUD/menus at the window resolution at any window size.

## Verification

Measurements below are with `ui_scale = 1` (or `LARECOMP_UI_SPLIT=1`).

- 1080p window, 1x 3D, 1080p interface and FSR 3: HUD text/minimap sharp at 1920x1080; `tmp` logs
  `HUD snapshot 1920x1080` (the pre-HUD upscaled scene, for frame generation). A clean run measured 41.5-42.3 fps;
  the old single-scale path at the same scene was ~55-56 fps. Screenshot crops show the sharper interface.
- Camera pan (right-button mouse look, 90 screen captures): HUD text-core temporal standard deviation ~0.07 and
  centre-of-mass std 0.15 px, against ~6 and 3.3 px in the asynchronous HUD-delta path.
- 1080p 3D and 1080p interface (`resolution = 1.5` on the 720p game frame), FSR 3 native: the split is active and
  `tmp` logs `1920x1080 -> 1920x1080`; the fixed HUD text mask measured 0.035 (old asynchronous path: 6.9). A clean
  run measured 30-32 fps against ~42 fps for the old path.
- Title screen (`Press Any Button`, scene and HUD in the same pass): no artefacts, 60 fps.
- 75 % (`1440x810` 3D), Match + FSR 3 Quality (`1280x720` 3D), 2x (`2560x1440` 3D) and Dynamic all reach gameplay;
  the pre-HUD snapshot is always the interface size (1920x1080).
- `xcde.ini` `ui_scale = 1` enables the split; the default (`0`) is the old single-scale path. For tests,
  `LARECOMP_UI_SPLIT=1` forces it on and `LARECOMP_NO_UI_SPLIT=1` forces it off. `XCDE_UI_NO_DEPTH_COPY=1` and
  `XCDE_UI_NO_UPSCALE=1` are diagnostics for the two parts.
