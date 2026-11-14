#pragma once
#include <string>

// Volumetric light scattering (sun shafts) for the editor's maps, drawn by iw4r_mapload.dll as a
// post pass on the game's D3D9 device (hooked Present). No game addresses: the depth comes from the
// game's own floatz render target (found as the full-screen R32F target it renders to), the camera
// from its GfxViewParms (found by scanning memory for view/projection/viewProjection matrices that
// multiply out), and the settings from "<map>.volumetric" next to the deployed "<map>.ff" (written
// by the compiler from worldspawn keys; re-read live when the file changes).
namespace volumetric {

using LogFn = void (*)(const std::string&);

// Installs the D3D9 hooks (safe before or after the game creates its device).
void init(LogFn log);

// Called for every *.ff the game opens. redirected = served from the deploy folder. A redirected
// map with a .volumetric file next to it turns the effect on; opening another map's zone turns it
// off.
void onZoneOpen(const std::string& path, const std::string& redirectedPath);

}  // namespace volumetric
