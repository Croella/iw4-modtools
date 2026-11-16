#include "worldgeom.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "document.h"

namespace worldgeom {

bool isBrushModelClass(const std::string& cls) {
    if (cls.rfind("trigger_", 0) == 0 || cls == "info_volume") return false;  // brush triggers ("?N")
    if (cls == "worldspawn" || cls == "func_group" || cls == "iw4r_terrain") return false;
    return true;
}

std::vector<size_t> brushModelEntities(const Document& doc) {
    std::vector<size_t> out;
    for (size_t i = 1; i < doc.ents.size(); i++)
        if (!doc.ents[i].brushes.empty() && isBrushModelClass(doc.ents[i].classname())) out.push_back(i);
    return out;
}

Vec3 brushModelOrigin(const Entity& e) {
    Vec3 o;
    if (e.origin(o)) return o;
    AABB all;
    for (const Brush& src : e.brushes) {
        Brush b = src;
        if (b.rebuild()) all.add(b.bounds);
    }
    if (!all.valid()) return {};
    o = all.center();
    for (int k = 0; k < 3; k++) o[k] = std::round(o[k]);
    return o;
}

std::vector<patch::Patch> worldPatches(const Document& doc) {
    if (doc.ents.empty()) return {};
    return patch::entityPatches(doc.ents[0]);
}

std::vector<Face> worldPatchFaces(const Document& doc, const patch::TexSizeFn& texSize) {
    std::vector<Face> out;
    for (const patch::Patch& p : worldPatches(doc)) {
        std::vector<Face> f = patch::faces(patch::tessellate(p, texSize), p.material);
        out.insert(out.end(), f.begin(), f.end());
    }
    return out;
}

Placement entityPlacement(const Entity& e) {
    Placement pl;
    e.origin(pl.origin);
    double ang[3] = {0, 0, 0};
    if (const std::string* a = e.find("angles"))
        std::sscanf(a->c_str(), "%lf %lf %lf", &ang[0], &ang[1], &ang[2]);
    else if (const std::string* y = e.find("angle"))
        ang[1] = std::atof(y->c_str());
    if (const std::string* s = e.find("modelscale")) pl.scale = std::atof(s->c_str());
    if (!(pl.scale > 0)) pl.scale = 1;
    constexpr double kPi = 3.14159265358979323846;
    const double p = ang[0] * kPi / 180, y = ang[1] * kPi / 180, r = ang[2] * kPi / 180;
    const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    pl.axis[0] = {cp * cy, cp * sy, -sp};
    pl.axis[1] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
    pl.axis[2] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    return pl;
}

bool dopBrush(const std::vector<Vec3>& pts, const std::string& material, Brush& out) {
    if (pts.size() < 4) return false;
    out = Brush{};
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            for (int z = -1; z <= 1; z++) {
                if (!x && !y && !z) continue;
                const Vec3 n = normalize(Vec3{double(x), double(y), double(z)});
                double hi = -1e300;
                for (const Vec3& p : pts) hi = std::max(hi, dot(p, n));
                out.faces.push_back(patch::planeFace(n, hi, material));
            }
    if (!out.rebuild()) return false;
    // Drop the planes that ended up redundant (no winding).
    // (planes touching only an edge or a corner leave a zero-area sliver).
    std::vector<Face> kept;
    for (const Face& f : out.faces) {
        if (f.winding.size() < 3) continue;
        Vec3 area;
        for (size_t k = 1; k + 1 < f.winding.size(); k++) area += cross(f.winding[k] - f.winding[0], f.winding[k + 1] - f.winding[0]);
        if (length(area) > 0.02) kept.push_back(f);
    }
    out.faces = kept;
    return out.rebuild();
}

}  // namespace worldgeom
