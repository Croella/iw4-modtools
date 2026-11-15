#include "terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <tuple>

#include "brushshapes.h"

namespace terrain {
namespace {
constexpr double kPi = 3.14159265358979323846;

// Hash-based value noise in [0, 1).
double hash01(int x, int y, unsigned seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / static_cast<double>(0x1000000);
}
double valueNoise(double x, double y, unsigned seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const double fx = x - xi, fy = y - yi;
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = hash01(xi, yi, seed), b = hash01(xi + 1, yi, seed), c = hash01(xi, yi + 1, seed), d = hash01(xi + 1, yi + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
}  // namespace

bool Field::heightAt(double x, double y, double& out) const {
    const double gx = (x - origin.x) / cell, gy = (y - origin.y) / cell;
    if (gx < 0 || gy < 0 || gx > nx || gy > ny) return false;
    const int i = std::min(nx - 1, static_cast<int>(gx)), j = std::min(ny - 1, static_cast<int>(gy));
    const double fx = gx - i, fy = gy - j;
    out = at(i, j) * (1 - fx) * (1 - fy) + at(i + 1, j) * fx * (1 - fy) + at(i, j + 1) * (1 - fx) * fy + at(i + 1, j + 1) * fx * fy;
    return true;
}

bool read(const Entity& e, Field& f) {
    if (e.classname() != kClass) return false;
    f = Field{};
    if (const std::string* v = e.find("terrainid")) f.id = *v;
    e.origin(f.origin);
    if (const std::string* v = e.find("cells")) std::sscanf(v->c_str(), "%d %d", &f.nx, &f.ny);
    if (const std::string* v = e.find("cellsize")) f.cell = std::atof(v->c_str());
    if (const std::string* v = e.find("depth")) f.depth = std::atof(v->c_str());
    if (const std::string* v = e.find("material")) f.material = *v;
    if (const std::string* v = e.find("paintjitter")) f.paintJitter = std::atof(v->c_str());
    if (const std::string* v = e.find("paint")) {
        // "material minSlope maxSlope minHeight maxHeight; ..."
        std::istringstream in(*v);
        std::string item;
        while (std::getline(in, item, ';')) {
            std::istringstream r(item);
            PaintRule pr;
            if (r >> pr.material >> pr.minSlope >> pr.maxSlope) {
                r >> pr.minHeight >> pr.maxHeight;
                f.paint.push_back(pr);
            }
        }
    }
    f.nx = std::clamp(f.nx, 1, 256);
    f.ny = std::clamp(f.ny, 1, 256);
    if (f.cell < 1) f.cell = 64;
    f.h.assign(static_cast<size_t>(f.nx + 1) * (f.ny + 1), 0.0);
    if (const std::string* v = e.find("heights")) {
        std::istringstream in(*v);
        for (double& x : f.h)
            if (!(in >> x)) break;
    }
    return !f.id.empty();
}

void write(const Field& f, Entity& e) {
    e.set("classname", kClass);
    e.set("terrainid", f.id);
    e.setOrigin(f.origin);
    e.set("cells", std::to_string(f.nx) + " " + std::to_string(f.ny));
    char b[64];
    std::snprintf(b, sizeof b, "%g", f.cell);
    e.set("cellsize", b);
    std::snprintf(b, sizeof b, "%g", f.depth);
    e.set("depth", b);
    e.set("material", f.material);
    if (f.paint.empty()) {
        std::erase_if(e.kv, [](const KeyValue& kv) { return kv.key == "paint" || kv.key == "paintjitter"; });
    } else {
        std::string ps;
        for (const PaintRule& r : f.paint) {
            std::snprintf(b, sizeof b, " %g %g %g %g", r.minSlope, r.maxSlope, r.minHeight, r.maxHeight);
            ps += (ps.empty() ? "" : "; ") + r.material + b;
        }
        e.set("paint", ps);
        std::snprintf(b, sizeof b, "%g", f.paintJitter);
        e.set("paintjitter", b);
    }
    std::string hs;
    hs.reserve(f.h.size() * 6);
    for (size_t k = 0; k < f.h.size(); k++) {
        std::snprintf(b, sizeof b, "%s%g", k ? " " : "", std::round(f.h[k] * 100) / 100);
        hs += b;
    }
    e.set("heights", hs);
}

std::string tag(const Field& f, int i, int j, int t) {
    return "iw4r_terrain " + f.id + " " + std::to_string(i) + " " + std::to_string(j) + " " + std::to_string(t) + ";";
}

bool parseTag(const Brush& b, std::string& id, int& i, int& j, int& t) {
    for (const std::string& a : b.attrs) {
        if (a.rfind("iw4r_terrain ", 0) != 0) continue;
        char buf[128];
        if (std::sscanf(a.c_str(), "iw4r_terrain %127s %d %d %d", buf, &i, &j, &t) == 4) {
            id = buf;
            return true;
        }
    }
    return false;
}

void cellBrushes(const Field& f, int i, int j, Brush out[2]) {
    const double zb = f.origin.z - f.depth;
    auto corner = [&](int ci, int cj, double z) { return Vec3{f.worldX(ci), f.worldY(cj), z}; };
    auto top = [&](int ci, int cj) { return corner(ci, cj, std::max(zb + 1.0, f.origin.z + f.at(ci, cj))); };
    // Alternate the diagonal (checkerboard) so slopes don't all lean the same way.
    const bool alt = ((i + j) & 1) != 0;
    // The two triangles as corner index pairs.
    int A[3][2], B[3][2];
    if (!alt) {  // diagonal (i, j) - (i + 1, j + 1)
        const int a[3][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}}, b[3][2] = {{i, j}, {i + 1, j + 1}, {i, j + 1}};
        std::copy(&a[0][0], &a[0][0] + 6, &A[0][0]);
        std::copy(&b[0][0], &b[0][0] + 6, &B[0][0]);
    } else {  // diagonal (i + 1, j) - (i, j + 1)
        const int a[3][2] = {{i, j}, {i + 1, j}, {i, j + 1}}, b[3][2] = {{i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
        std::copy(&a[0][0], &a[0][0] + 6, &A[0][0]);
        std::copy(&b[0][0], &b[0][0] + 6, &B[0][0]);
    }
    for (int t = 0; t < 2; t++) {
        const int (*T)[2] = t == 0 ? A : B;
        Vec3 up[3], dn[3];
        for (int k = 0; k < 3; k++) {
            up[k] = top(T[k][0], T[k][1]);
            dn[k] = corner(T[k][0], T[k][1], zb);
        }
        std::vector<std::vector<Vec3>> polys = {{up[0], up[1], up[2]}, {dn[0], dn[1], dn[2]}};
        for (int k = 0; k < 3; k++) polys.push_back({dn[k], dn[(k + 1) % 3], up[(k + 1) % 3], up[k]});
        const Vec3 n = cross(up[1] - up[0], up[2] - up[0]);
        const double len = length(n);
        const double slope = len > 0 ? std::acos(std::min(1.0, std::fabs(n.z) / len)) * 180 / kPi : 0;
        const double meanH = (up[0].z + up[1].z + up[2].z) / 3 - f.origin.z;
        Brush b;
        shapes::convexBrush(polys, paintMaterial(f, slope, meanH, i, j, t), b);
        const std::string wall = paintMaterial(f, 90, meanH, i, j, t);
        // Only the top and the terrain's outer walls are ever seen: the bottom and the sides shared
        // with neighbouring cells are caulk (not drawn).
        const double x0 = f.worldX(0), x1 = f.worldX(f.nx), y0 = f.worldY(0), y1 = f.worldY(f.ny);
        for (Face& face : b.faces) {
            if (face.normal.z > 0.05) continue;  // top
            bool outer = false;
            if (face.normal.z > -0.9 && !face.winding.empty()) {
                auto all = [&](auto pred) {
                    for (const Vec3& p : face.winding)
                        if (!pred(p)) return false;
                    return true;
                };
                outer = all([&](const Vec3& p) { return std::fabs(p.x - x0) < 0.01; }) || all([&](const Vec3& p) { return std::fabs(p.x - x1) < 0.01; }) ||
                        all([&](const Vec3& p) { return std::fabs(p.y - y0) < 0.01; }) || all([&](const Vec3& p) { return std::fabs(p.y - y1) < 0.01; });
            }
            face.material = outer ? wall : "caulk";
        }
        b.attrs = {tag(f, i, j, t)};
        out[t] = std::move(b);
    }
}

int create(Document& doc, Field f) {
    if (f.id.empty()) {
        int n = 1;
        for (const Entity& e : doc.ents)
            if (e.classname() == kClass) n++;
        f.id = "t" + std::to_string(n);
        for (bool clash = true; clash;) {  // unique among existing terrains
            clash = false;
            for (const Entity& e : doc.ents)
                if (const std::string* v = e.find("terrainid"); v && *v == f.id) clash = true;
            if (clash) f.id = "t" + std::to_string(++n);
        }
    }
    if (f.h.size() != static_cast<size_t>(f.nx + 1) * (f.ny + 1)) f.h.assign(static_cast<size_t>(f.nx + 1) * (f.ny + 1), 0.0);
    Sel s = doc.addPointEntity(kClass, f.origin);
    write(f, doc.ents[s.ent]);
    for (int j = 0; j < f.ny; j++)
        for (int i = 0; i < f.nx; i++) {
            Brush b[2];
            cellBrushes(f, i, j, b);
            doc.ents[0].brushes.push_back(std::move(b[0]));
            doc.ents[0].brushes.push_back(std::move(b[1]));
        }
    doc.touch();
    return s.ent;
}

void rebuildCells(Document& doc, const Field& f, int i0, int j0, int i1, int j1) {
    // Cells touching the changed corners.
    const int ci0 = std::max(0, i0 - 1), cj0 = std::max(0, j0 - 1), ci1 = std::min(f.nx - 1, i1), cj1 = std::min(f.ny - 1, j1);
    if (ci0 > ci1 || cj0 > cj1) return;
    std::map<std::tuple<int, int, int>, size_t> where;
    auto& world = doc.ents[0].brushes;
    for (size_t k = 0; k < world.size(); k++) {
        std::string id;
        int i, j, t;
        if (parseTag(world[k], id, i, j, t) && id == f.id && i >= ci0 && i <= ci1 && j >= cj0 && j <= cj1) where[{i, j, t}] = k;
    }
    for (int j = cj0; j <= cj1; j++)
        for (int i = ci0; i <= ci1; i++) {
            Brush b[2];
            cellBrushes(f, i, j, b);
            for (int t = 0; t < 2; t++) {
                auto it = where.find({i, j, t});
                if (it != where.end()) {
                    // keep the texture of cells painted / retextured by hand, and the brush's other
                    // attributes (surface type, painted mark)
                    const Brush& old = world[it->second];
                    if (!old.faces.empty() && !b[t].faces.empty() && (painted(old) || !autoMaterial(f, old.faces[0].material)))
                        b[t].faces[0].material = old.faces[0].material;  // faces[0] is the top
                    for (const std::string& a : old.attrs)
                        if (a.rfind("iw4r_terrain ", 0) != 0) b[t].attrs.push_back(a);
                    world[it->second] = std::move(b[t]);
                } else {
                    world.push_back(std::move(b[t]));
                }
            }
        }
    doc.touch();
}

std::string paintMaterial(const Field& f, double slopeDeg, double height, int i, int j, int t) {
    const double jit = f.paintJitter > 0 ? (hash01(i * 2 + t, j, 0x9E37u) * 2 - 1) * f.paintJitter : 0;
    const double s = slopeDeg + jit;
    for (const PaintRule& r : f.paint) {
        if (r.material.empty()) continue;
        // a rule reaching 0 or 90 catches everything past that end, whatever the jitter
        const bool lo = r.minSlope <= 0 || s >= r.minSlope, hi = r.maxSlope >= 90 || s < r.maxSlope;
        if (lo && hi && height >= r.minHeight && height <= r.maxHeight) return r.material;
    }
    return f.material;
}

bool autoMaterial(const Field& f, const std::string& m) {
    if (m == f.material || m == "caulk") return true;
    for (const PaintRule& r : f.paint)
        if (r.material == m) return true;
    return false;
}

int repaint(Document& doc, const Field& f, const Field* prev, bool force) {
    int changed = 0;
    for (Brush& b : doc.ents[0].brushes) {
        std::string id;
        int i, j, t;
        if (!parseTag(b, id, i, j, t) || id != f.id || b.faces.empty()) continue;
        Face& top = b.faces[0];
        double meanH = 0;
        for (const Vec3& p : top.winding) meanH += p.z;
        meanH = top.winding.empty() ? 0 : meanH / top.winding.size() - f.origin.z;
        const double slope = std::acos(std::clamp(std::fabs(top.normal.z), 0.0, 1.0)) * 180 / kPi;
        if (force) std::erase(b.attrs, std::string(kPaintedAttr));
        const bool brushPainted = painted(b);
        auto set = [&](Face& face, const std::string& m) {
            const bool hand = (brushPainted && &face == &top) ||
                              (!autoMaterial(f, face.material) && !(prev && autoMaterial(*prev, face.material)));
            if ((hand && !force) || face.material == m) return;
            face.material = m;
            changed++;
        };
        set(top, paintMaterial(f, slope, meanH, i, j, t));
        const std::string wall = paintMaterial(f, 90, meanH, i, j, t);
        for (size_t k = 1; k < b.faces.size(); k++)
            if (b.faces[k].material != "caulk") set(b.faces[k], wall);
    }
    if (changed) doc.touch();
    return changed;
}

const std::vector<PaintPreset>& paintPresets() {
    static const std::vector<PaintPreset> p = {
        {"Grass / dirt / cliff", {{"ch_grass_01", 0, 22}, {"ch_ground_dirt03", 22, 38}, {"ch_cliff01", 38, 90}}},
        {"Desert", {{"me_ground_sand1", 0, 18}, {"ch_ground_gravel01", 18, 34}, {"rock_cliff", 34, 90}}},
        {"Mud / gravel / rock", {{"ch_ground_mud01", 0, 15}, {"ch_ground_gravel02", 15, 35}, {"ch_cliff02", 35, 90}}},
        {"Grass + rocky peaks", {{"ch_cliff01", 0, 90, 600, 1e6}, {"ch_grass_02", 0, 28}, {"ch_cliff02", 28, 90}}},
    };
    return p;
}

void removeBrushes(Document& doc, const std::string& id) {
    auto& world = doc.ents[0].brushes;
    world.erase(std::remove_if(world.begin(), world.end(),
                               [&](const Brush& b) {
                                   std::string bid;
                                   int i, j, t;
                                   return parseTag(b, bid, i, j, t) && bid == id;
                               }),
                world.end());
    doc.sel.clear();
    doc.touch();
}

const char* toolName(Tool t) {
    switch (t) {
        case Tool::Raise: return "Raise";
        case Tool::Lower: return "Lower";
        case Tool::Smooth: return "Smooth";
        case Tool::Flatten: return "Flatten";
        case Tool::Noise: return "Noise";
        case Tool::Melt: return "Melt";
        case Tool::Paint: return "Paint";
    }
    return "?";
}

const char* toolTip(Tool t) {
    switch (t) {
        case Tool::Raise: return "Pull the ground up (Ctrl: push down)";
        case Tool::Lower: return "Push the ground down (Ctrl: pull up)";
        case Tool::Smooth: return "Average out bumps";
        case Tool::Flatten: return "Level everything to the height where the stroke started";
        case Tool::Noise: return "Roughen with random bumps";
        case Tool::Melt: return "Thermal erosion: steep slopes slump downhill, like melting rock or sand";
        case Tool::Paint: return "Paint the current texture onto the ground (Ctrl: erase back to the slope rules)";
    }
    return "";
}

bool painted(const Brush& b) {
    return std::find(b.attrs.begin(), b.attrs.end(), kPaintedAttr) != b.attrs.end();
}

int paint(Document& doc, const Field& f, const Stroke& s, double x, double y, bool erase) {
    if (!erase && s.material.empty()) return 0;
    const double r = std::max(1.0, s.radius), scatter = std::clamp(s.scatter, 0.0, 1.0);
    int changed = 0;
    for (Brush& b : doc.ents[0].brushes) {
        std::string id;
        int i, j, t;
        if (!parseTag(b, id, i, j, t) || id != f.id || b.faces.empty()) continue;
        // quick reject on the cell, then the triangle's centroid against a ragged rim
        if (f.worldX(i + 1) < x - r || f.worldX(i) > x + r || f.worldY(j + 1) < y - r || f.worldY(j) > y + r) continue;
        Face& top = b.faces[0];
        if (top.winding.empty()) continue;
        Vec3 c;
        for (const Vec3& p : top.winding) c += p;
        c = c * (1.0 / top.winding.size());
        const double d = std::hypot(c.x - x, c.y - y);
        const double reach = r * (1 - scatter * hash01(i * 2 + t, j, 0x51Au));  // fixed per triangle: no flicker
        if (d > reach) continue;
        const double slope = std::acos(std::clamp(std::fabs(top.normal.z), 0.0, 1.0)) * 180 / kPi;
        if (erase) {
            if (!painted(b)) continue;
            std::erase(b.attrs, std::string(kPaintedAttr));
            top.material = paintMaterial(f, slope, c.z - f.origin.z, i, j, t);
            changed++;
            continue;
        }
        if (slope < s.minSlope || slope > s.maxSlope) continue;
        if (top.material == s.material && painted(b)) continue;
        top.material = s.material;
        if (!painted(b)) b.attrs.push_back(kPaintedAttr);
        changed++;
    }
    if (changed) doc.touch();
    return changed;
}

bool apply(Field& f, const Stroke& s, double x, double y, double dt, int& i0, int& j0, int& i1, int& j1) {
    if (s.tool == Tool::Paint) return false;  // see paint()
    const double r = std::max(1.0, s.radius);
    const int ia = std::max(0, static_cast<int>(std::floor((x - r - f.origin.x) / f.cell)));
    const int ib = std::min(f.nx, static_cast<int>(std::ceil((x + r - f.origin.x) / f.cell)));
    const int ja = std::max(0, static_cast<int>(std::floor((y - r - f.origin.y) / f.cell)));
    const int jb = std::min(f.ny, static_cast<int>(std::ceil((y + r - f.origin.y) / f.cell)));
    if (ia > ib || ja > jb) return false;
    const std::vector<double> before = f.h;
    auto weight = [&](int i, int j) {
        const double d = std::hypot(f.worldX(i) - x, f.worldY(j) - y);
        return d >= r ? 0.0 : 0.5 * (1 + std::cos(kPi * d / r));
    };
    const double floor = -f.depth + 1;
    bool any = false;
    i0 = ia, j0 = ja, i1 = ib, j1 = jb;
    static unsigned noiseTick = 0;
    noiseTick++;
    for (int j = ja; j <= jb; j++)
        for (int i = ia; i <= ib; i++) {
            const double w = weight(i, j);
            if (w <= 0) continue;
            double& h = f.at(i, j);
            const double rate = std::min(1.0, w * dt * s.strength * 0.05);
            switch (s.tool) {
                case Tool::Raise: h += s.strength * dt * w; break;
                case Tool::Lower: h -= s.strength * dt * w; break;
                case Tool::Flatten: h += (s.flattenTo - h) * rate; break;
                case Tool::Noise: h += (hash01(i, j, s.seed + noiseTick) * 2 - 1) * s.strength * dt * w * 2; break;
                case Tool::Smooth: {
                    double sum = 0;
                    int n = 0;
                    for (int dj = -1; dj <= 1; dj++)
                        for (int di = -1; di <= 1; di++) {
                            const int a = i + di, b = j + dj;
                            if ((di || dj) && a >= 0 && b >= 0 && a <= f.nx && b <= f.ny) sum += before[static_cast<size_t>(b) * (f.nx + 1) + a], n++;
                        }
                    if (n) h += (sum / n - before[static_cast<size_t>(j) * (f.nx + 1) + i]) * rate;
                    break;
                }
                case Tool::Melt: {
                    // Material above the talus slope (~27 degrees) slides to the lowest neighbour.
                    const double talus = f.cell * 0.5;
                    int bi = -1, bj = -1;
                    double drop = 0;
                    for (int dj = -1; dj <= 1; dj++)
                        for (int di = -1; di <= 1; di++) {
                            const int a = i + di, b = j + dj;
                            if ((!di && !dj) || a < 0 || b < 0 || a > f.nx || b > f.ny) continue;
                            const double d = (h - f.at(a, b)) / ((di && dj) ? 1.41421356 : 1.0);
                            if (d > drop) drop = d, bi = a, bj = b;
                        }
                    if (bi >= 0 && drop > talus) {
                        const double move = (drop - talus) * 0.5 * std::min(1.0, rate * 4);
                        h -= move;
                        f.at(bi, bj) += move;
                        i0 = std::min(i0, bi), j0 = std::min(j0, bj), i1 = std::max(i1, bi), j1 = std::max(j1, bj);
                    }
                    break;
                }
            }
            h = std::max(h, floor);
            any = true;
        }
    return any;
}

void mountains(Field& f, double height, int octaves, unsigned seed) {
    const double base = 3.0 / std::max(f.nx, f.ny);  // ~3 big features across the field
    std::vector<double> n(f.h.size());
    double lo = 1e30, hi = -1e30;
    for (int j = 0; j <= f.ny; j++)
        for (int i = 0; i <= f.nx; i++) {
            double v = 0, amp = 1, freq = base, norm = 0;
            for (int o = 0; o < std::max(1, octaves); o++) {
                v += valueNoise(i * freq, j * freq, seed + static_cast<unsigned>(o) * 101u) * amp;
                norm += amp;
                amp *= 0.5;
                freq *= 2;
            }
            v /= norm;
            n[static_cast<size_t>(j) * (f.nx + 1) + i] = v;
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
    for (size_t k = 0; k < n.size(); k++) {
        const double t = hi > lo ? (n[k] - lo) / (hi - lo) : 0;
        f.h[k] = std::pow(t, 1.8) * height;  // broad valleys, sharper peaks
    }
}

}  // namespace terrain
