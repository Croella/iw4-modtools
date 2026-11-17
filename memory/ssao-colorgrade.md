---
name: ssao-colorgrade
description: "SSAO + colour grading (2026-11-15): worldspawn volumetric_ssao*/volumetric_grade* -> .volumetric (baked 32^3 LUT as lutrow hex lines) -> iw4r_mapload.dll passes + editor preview; voltest PASS, NOT tested in-game yet"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-16T11:31:51.924Z
---

User asked "What are some cool directx things we could add" (2026-11-15), picked SSAO + colour grading.
Built on the volumetric post pass ([[volumetric-lighting]]); same worldspawn prefix convention (volumetric_*).

**Keys:** volumetric_ssao 1, _ssao_radius (default 64), _ssao_intensity (1.5), _ssao_bias (0.1),
_ssao_near (20: keeps AO off the viewmodel), _ssao_distance (3000); debug 4 = AO only.
volumetric_grade 1, _grade_exposure/_contrast/_saturation/_temperature/_tint, _grade_lift/_gamma/_gain "r g b",
_grade_vignette, _grade_lut <name> = zone_source/luts/<name>.png strip LUT (size*size x size), applied first.
Defaults 64/1.5 picked from editor AO-only screenshots of the zm_test house (32/1 was too faint).

**Code:** src/light/colorgrade.{h,cpp} (bakeGradeLut, loadStripLut, saveNeutralStripLut; CLI
`iw4r_zone neutrallut <out.png> [size]`); lightbake readVolumetric (cachedGradeLut keyed by params +
LUT mtime) / volumetricConfig writes ssao*, grade, vignette, "lut 32" + 1024 "lutrow b g <hex>" lines
(so every existing .volumetric writer carries it). DLL: shaders psAo/psAoBlur/psGrade in
tools/shaders/post/volumetric.hlsl (PS c16-c21, s3 = volume LUT, managed pool); render() order
AO (multiply) -> fog -> shafts -> grade, StretchRect recopy between passes; saveBindings now covers PS
c0-23 + textures 0-3. Editor: renderer.cpp kPostShader psAo/psAoBlur/psGrade, Texture3D LUT, R8 AO targets.
Tests: testPostSettings (tests.cpp), `iw4r_voltest <dll> ssao|grade`.
Screen-space AO limit seen: grazing floors far away get little AO on the wall side (few px of floor in radius).
**DOF + per-area grading (2026-11-16, user: "add dof settings and per scene color grading with per
trigger blending"):** worldspawn volumetric_dof, _dof_near_start/_near_end/_far_start/_far_end, _dof_near_blur/_far_blur
(px at 1080p, max 32), _dof_weapon (24, closer = viewmodel, sharp), _dof_auto (planes become offsets from the
crosshair depth, 1x1 R32F ping-pong on the GPU, no readback), _dof_auto_speed; debug 5 = blur amount. .volumetric:
dof, dofnear a b, doffar a b, dofblur n f, dofweapon, dofauto, dofautospeed. Passes AO -> fog -> DOF -> shafts -> grade.
`trigger_grade` brush entity (document.cpp class list; zonebuild strips it from the zone): grade_* keys inherit
worldspawn (neutral when worldspawn grade is off), blend (units inside the edge, 64), blend_time (s, 0), priority
(higher on top). light::GradeVolume (lightbake.h), header-only src/light/gradevolume.h shared with the DLL
(insideDepth/targetWeight/approach/blendInto). .volumetric: gradevolume i blend time vignette, volbrush i,
volplane i nx ny nz d (outward, inside n.p <= d), vollutrow i b g hex. DLL + editor blend the tables on the CPU from
the camera position and re-upload only when weights move. voltest modes gradevol, dof, dofauto PASS.
**Sun flare, glare, screen-space weather (2026-11-16, user: "add a sun flare and glare. And Screen-space weather"):**
worldspawn volumetric_flare (+_intensity/_halo/_ghosts/_streak/_color), volumetric_glare (+_threshold 0.8/_intensity 0.6/
_radius px@1080 max 64/_sun = veil), volumetric_weather 0/1 rain/2 snow (+_intensity/_speed/_wind "x y"/_distance 1200/
_color/_opacity). trigger_weather brush entity (weather_intensity inside, default 0 = shelter; blend 64; blend_time 0.5),
camera-position only (looking out a window from inside shows no rain). .volumetric: flare, flareparm i h g s, flarecolor,
glare, glareparm t i r s, weather, weatherparm i s d, weatherwind, weathercolor r g b a, wxvolume/wxbrush/wxplane.
Flare visibility = 1x1 R32F ping-pong storing vis+1 (sky taps around the sun; DLL ignores player-clip occluders, editor
counts them). Weather = 4 cylinder layers around the camera, depth-tested. Order: AO, fog, DOF, weather, shafts, glare,
flare, grade. PS constants c26-c33 (saveBindings covers c0-33). voltest modes flare/flareocc/glare/rain/snow/wxshelter.
UI self-test fails 23/38 in 2D drag checks when the user's mouse is active (ImGui reads the real cursor when the hidden
window is foreground); the committed HEAD build fails the same way, so it's environmental.
**Local light shafts (2026-11-16, user: "Local light shafts from point lights, spotlights etc"):** worldspawn
volumetric_lights 1 (+_intensity, _shadows 0.8, _max 8 (<=16), _distance 4000). Light keys: volumetric 0 (exclude),
volumetric_intensity, volumetric_flicker 0..1, volumetric_only 1 (shaft only, not baked). NEW spotlights in readLights
(baker too): fov_outer (half-angle deg; default 45 if only target), fov_inner (default 80%), aim = target entity, else
angles/angle, else straight down; baker multiplies smoothstep cone. .volumetric: lights, lightsparm i s max dist,
llight x y z r g b radius dx dy dz cosOuter cosInner flicker (point: 0 0 0 -2 -2). Per light at half res: 24-step
march of (1-d^2/R^2)^2 * cone in the sphere up to scene depth + exact core integral (k^2=0.02R^2, x2), scale 0.0007;
alpha = free-air mask; radial pass toward the light's screen position averages the mask = occluder streaks; add into a
half-res accumulator, add onto the frame. No real shadow maps (game can't re-render from the light). PS c34-c38
(saveBindings c0-38). Order: ... DOF -> local lights -> weather -> shafts ... voltest lights/lightsocc/lightspot.
**Contact shadows (2026-11-16, user: "Do contact shadows and gpu light map baking"):** worldspawn volumetric_contact 1
(+_intensity 0.6, _length 32 (<=512), _thickness 24, _distance 2500, _near 20); debug 6 = shadow only. .volumetric: contact,
contactparm i len thick dist, contactnear. psContact (DLL, view space; sun to view space via corner-ray basis + Cramer) /
editor psContact (world space, viewProjM in cbuffer): 16-step march towards the sun through depth, skips samples within
2 px, blurred with psAoBlur, multiplied in right after AO. PS c39-c40 (saveBindings c0-40). voltest mode contact.
Limit: can't tell sunlit pixels, so it also deepens existing baked shadows a little (like SSAO). In the editor, brush-edge
wireframe lines drawn over the post pass look like dark outlines in debug 6 - not the effect.
voltest: run it from PowerShell (Start-Process); under Git Bash it segfaults at exit.
**Next:** user's first in-game run -> mapload.log "ssao: on (INTZ depth)" / "grade: 32x32x32 table uploaded".
