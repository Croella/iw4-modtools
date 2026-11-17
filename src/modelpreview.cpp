#include "modelpreview.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include "assetmanager.h"
#include "zone/gfxworld.h"
#include "zone/zonebuild.h"

namespace modelpreview {

size_t load(const std::string& dir, Renderer& renderer, live::LiveModelLibrary& library, std::vector<ModelInfo>* infos,
            const std::function<void(const std::string&)>& log) {
    namespace fs = std::filesystem;
    if (infos) infos->clear();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::path(dir) / "xmodels", ec)) return 0;
    // Same build options as the compiler (weapon hideTags drop attachment geometry, ...).
    std::vector<std::string> warnings, weaponWarnings;
    ZoneSourceWeapons zw = loadZoneSourceWeapons(dir, weaponWarnings);
    std::vector<modelbuild::Model> models = loadZoneSourceModels(dir, warnings, &zw.hints);
    for (const std::string& w : warnings) log("  zone_source: " + w);
    renderer.clearThumbnails();
    auto half = [](uint16_t h) {
        uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
        if (e == 0) bits = sign << 31;
        else bits = (sign << 31) | ((e - 15 + 127) << 23) | (m << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };
    for (const modelbuild::Model& md : models) {
        auto lm = std::make_shared<live::LiveModel>();
        lm->name = md.name;
        if (infos) {
            ModelInfo bm;
            bm.name = md.name;
            bm.bones = md.bones.size();
            bm.surfaces = md.surfaces.size();
            for (const modelbuild::Surface& s : md.surfaces) {
                bm.tris += s.tris.size() / 3;
                bm.verts += s.verts.size();
                const std::string& tex = md.materials[s.material].texture;
                const std::string t = tex.empty() ? md.materials[s.material].name : fs::path(tex).filename().string();
                if (std::find(bm.textures.begin(), bm.textures.end(), t) == bm.textures.end()) bm.textures.push_back(t);
            }
            bm.size = Vec3{md.maxs[0] - md.mins[0], md.maxs[1] - md.mins[1], md.maxs[2] - md.mins[2]};
            infos->push_back(std::move(bm));
        }
        for (const modelbuild::Surface& s : md.surfaces) {
            live::LiveModelSurf ls;
            const modelbuild::Material& mm = md.materials[s.material];
            ls.material = "mc/" + md.name + "_" + mm.name;
            // The compiler's texture search (zone_source/images, then beside the models).
            const fs::path tex = assetman::findTexture(dir, mm.texture, mm.name);
            if (!tex.empty()) renderer.setTextureFile(ls.material, tex.string());
            for (const modelbuild::PackedVertex& v : s.verts) {
                ls.pos.insert(ls.pos.end(), v.xyz, v.xyz + 3);
                Vec3 n = gfx::unpackUnitVec(v.normal);
                ls.nrm.push_back(static_cast<float>(n.x));
                ls.nrm.push_back(static_cast<float>(n.y));
                ls.nrm.push_back(static_cast<float>(n.z));
                ls.uv.push_back(half(static_cast<uint16_t>(v.texCoord & 0xFFFF)));
                ls.uv.push_back(half(static_cast<uint16_t>(v.texCoord >> 16)));
                lm->bounds.add(Vec3{v.xyz[0], v.xyz[1], v.xyz[2]});
            }
            // The game's winding is clockwise from the front; the preview draws both sides anyway.
            ls.idx = s.tris;
            lm->surfs.push_back(std::move(ls));
        }
        library[live::modelKey(md.name)] = lm;
    }
    if (!models.empty()) log("Loaded " + std::to_string(models.size()) + " zone_source model(s) for preview from " + dir);
    return models.size();
}

}  // namespace modelpreview
