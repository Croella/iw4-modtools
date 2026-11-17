#pragma once
#include <functional>
#include <string>
#include <vector>
#include "renderer.h"

// zone_source models as preview meshes (for the 3D view, the Models browser and Asset Manager
// thumbnails), built with the compiler's own options so they look like what the game will get.
namespace modelpreview {

struct ModelInfo {
    std::string name;
    size_t tris = 0, verts = 0, bones = 0, surfaces = 0;
    Vec3 size;
    std::vector<std::string> textures;
};

// Loads every zone_source/xmodels model into `library` (by live::modelKey) and registers its
// textures with the renderer. infos (optional) gets one entry per model. Returns the model count.
size_t load(const std::string& zoneSourceDir, Renderer& renderer, live::LiveModelLibrary& library, std::vector<ModelInfo>* infos,
            const std::function<void(const std::string&)>& log);

}  // namespace modelpreview
