---
name: volumetric-lighting
description: "In-game sun shafts: post pass in iw4r_mapload.dll (hooked D3D9 Present), floatz depth, runtime camera scan, <map>.volumetric settings; harness-verified, awaiting in-game test"
metadata:
  type: project
---
Added 2026-11-12 (user chose a real post-process over baked light-shaft geometry or an editor-only view).
src/dll/volumetric.{h,cpp} inside iw4r_mapload.dll. Shaders: tools/shaders/post/volumetric.hlsl, compiled
by `python tools/shaders/build_post.py` -> src/dll/postshaders.h (vs*/ps* entries, *_3_0).
No game addresses, both builds are D3D9 (no Ex):
- hooks the device vtable (Present 17, Reset 16, SetRenderTarget 37) via dummy HW+SW devices + an IDirect3D9::CreateDevice
  vtable hook (each vertex-processing mode/flag set can be its own vtable class)
- depth: first full-screen D3DFMT_R32F render target the game sets = floatz (logged "found the floatz depth target";
  every distinct RT format/size is logged once). Sky test: z <= 0 or z >= skydepth (UNKNOWN in-game: may need skymode 0)
- camera: scans the exe image + private RW memory for GfxViewParms (V orthonormal, P perspective with P[11]=1,
  VP = V*P, aspect = backbuffer), prefers image hits; log lists candidates. Rescans when it stops validating.
- passes: mask (half res) -> radial blur 32 taps x2 -> additive composite; drawn at Present, so it's over the HUD too.
Settings: compiler writes <map>.volumetric next to the .ff (light::volumetricConfig from worldspawn volumetric_* keys;
editor F5 writes it in the deploy folder); DLL activates on the redirected .ff open, re-reads the file live (~1 s),
turns off on another map's mp_*.ff / *_load.ff. debug 1 mask, 2 depth view.
Test harness: build/Release/iw4r_voltest.exe build/Release/iw4r_mapload.dll (fake game; PASS).
Next: user's first in-game run -> read %LOCALAPPDATA%\iw4-radiant\mapload.log ([volumetric] lines).
Related: [[devmap-loader-dll]], [[lighting]].

**First in-game check (2026-11-13), three findings:**
1. Steam RELAUNCHES iw4sp.exe: the process the editor starts exits, and steam.exe starts a new one,
   so the injected DLL was never in the real game (0 "redirect" lines in mapload.log ever; maps only
   loaded because a copy sat in zone/english). Fix: compileAndRun's detached thread waits 20 s on the
   started process; if it exits, finds the new iw4sp/iw4mp (Toolhelp) and injects into it.
2. Win10 d3d9 gives each DEVICE a private heap copy of its vtable (game device vt ~0x204e2d918b0,
   102/119 entries equal the dummy's), so hooking a dummy device does nothing. Fix: record the dummy's
   real Present/Reset/SetRenderTarget, then findGameDevice() scans the exe's writable data for a
   pointer to an object whose vtable holds them (found at exe+0x410FD20 in SP), and hooks that.
   Re-scans whenever frames stop for ~2 s.
3. The game presents via IDirect3DSwapChain9::Present (slot 3), not the device's. Swap chain vtables
   are static in d3d9.dll (shared). hookSwapChains() runs from the first SetRenderTarget.
Seen in SP menu: RT 'NULL' fourcc 1024x2048 (shadow), A8R8G8B8 1024x768 backbuffer.
Test injector: scratchpad inject.py <pid> <dll>; a loaded DLL can be renamed so it can be rebuilt.

**Working in-game (2026-11-13):** device found (exe+0x410FD20), swapchain Present hooked, floatz = R32F 1024x768 found, map on. Camera scan: 4 image copies (front/back end) a frame apart while turning -> mask lagged. Fix: hook SetVertexShaderConstantF (slot 94); the 4x4 upload equal (or transposed) to a copy's VP = this frame's camera (g_frameVP, reset each Present). facing = clip.w (no V needed). Watch log for "camera: using this frame's matrix".

**Height fog (2026-11-13, offline PASS, awaiting in-game):** psFog in volumetric.hlsl, fogPass() in
volumetric.cpp, runs at Present before the shafts (so it also tints the HUD). World pos = eye + ray*viewz;
eye and corner rays are solved from the frame VP alone (clip x,y,w columns, Cramer). Analytic
exponential height fog + Henyey-Greenstein sun scattering. Depth: the game's own depth buffer made
readable: hkCreateDSS (device slot 29) returns an INTZ texture's surface when the game creates a
non-MSAA D24 buffer (game makes it in R_InitRenderTargets, 2013 MP 0x52640, via CreateDepthStencilSurface);
viewz = P14/(z - P10) with P from the scanned camera copy. Catching creation needs the DLL in before the
device exists: Direct3DCreate9 IAT hook + game IDirect3D9 object found by scan (vt[16] == real
CreateDevice) -> hkCreateDevice. Otherwise vid_restart once. Fallback depth: floatz (only filled when
the game needs it: DOF/zfeather; r_floatz allocates it).
Settings: shafts, fog, fogdensity, fogfalloff, fogheight, fogcolor, fogsun, fogg, fogstart, fogmax,
fogsky, fogskydist, debug 3 = fog amount. Worldspawn (user wants everything under volumetric_): volumetric_fog, volumetric_fog_density/_falloff/_height/_color/_sun/_anisotropy/_start/_max/_sky/_skydist, volumetric_shafts (bare fog_* still read). The .volumetric always lists every fog setting with defaults. modeltest.map has them all.
Test: iw4r_voltest <dll> fog. Shell heredocs turn \n into newlines: write patch scripts with Write.

**Fog in-game run 1 (2026-11-13):** DLL injected 1.5 s after Steam's relaunch = after device creation ->
no INTZ; fog fell back to floatz once it appeared. Eye solved to 0: the game's view matrices are
CAMERA-RELATIVE; origin is the float[3] right after the 4 matrices (ViewParms +256), now taken from the
matched copy. The game's IDirect3D9 (exe+0x410FD18) is the Steam overlay's wrapper (vtable in
gameoverlayrenderer64.dll); CreateDevice originals are now kept per vtable (g_d3dVts). Editor injects
~50 ms after the relaunched process appears (was 1.5 s). Projection: P10 0.9995, P14 -3.998 (near ~4).

**Editor preview (2026-11-13):** light::Volumetric + readVolumetric(doc) (lightbake.h) is the one source of
settings; volumetricConfig formats it (now writes every key). Renderer::postProcess (renderer.cpp kPostShader,
D3D11): depth is R24G8_TYPELESS with an SRV; sky faces + the void write alpha 0 = sky marker; fog
(invViewProj reconstruct) -> mask/2 blurs at half res -> SCREEN blend (additive blew out with the editor's
flat bright sky); then an alpha-only pass makes the view opaque before wireframes. Runs when the bake is
shown + Lighting > Show Volumetrics. iw4radiant --camera x y z yaw pitch for screenshots.

**INTZ fog confirmed in-game (2026-11-13 log):** "depth: ... INTZ", "fog: on (INTZ depth), eye -239.6 -184.3 60.1".
**Under the HUD + full_clip (2026-11-13, offline PASS, awaiting in-game):** effect now runs from hkSetVSConstF at
the first ORTHO 2D matrix upload (2/w, -2/h, offset -1,+1, either layout) after the world VP matched, if the
render target is the one the world VP was uploaded with (g_worldRT); else falls back to Present. Logs
"hud: drawing before the game's 2D pass", or after 300 frames "hud: no switch to 2D seen" + dumps 48 uploads.
player clip faces (light::blocksVolumetricSun = contentsForMaterial & 0x10000: full_clip, clip_player, clip_ai...; user-requested + VERIFIED in-game 2026-11-14) -> "occluder" triangle lines in <map>.volumetric; DLL
draws them black into the mask (vsOccluder/psBlack); editor stamps alpha 1 (psSolid). Shaft sky mask now
prefers INTZ depth (mask.x 2) over floatz over brightness: the brightness fallback made chrome pillars "sky".
voltest modes: (none) | fog | occ, each checks the HUD box is untouched. iw4radiant --hide-tools.
Mid-frame gotcha (2026-11-13): drawing before the HUD left stream 0 = our DrawPrimitiveUP quad (UP clears it) and
the game CACHES its bindings, so the HUD compass drew our quad = "a small box of my view that jumps around".
Fix: saveBindings/restoreBindings (stream 0 + freq, indices, decl, VS/PS, textures 0-2, VS c0-5, PS c0-15) on top
of the D3DSBT_ALL state block. Rule: anything mid-frame must leave every binding exactly as the game left it.
CORRECTION (2026-11-13): the first ortho upload after the world is NOT the HUD - it's the game's post-effects stage,
which StretchRects the world target to a texture first and then draws that copy back full screen (wiping our
effect; with stream 0 broken that copy-back was the "jumping box"). Now the trigger is hkStretchRect (device
slot 34): src == g_worldRT after the VP matched -> render into src first. The ortho trigger is only a fallback
until a copy is seen (g_copyTrigger). voltest now emulates copy + 2D copy-back + HUD.
