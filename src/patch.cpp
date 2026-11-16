#include "patch.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include "document.h"

namespace patch {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Quadratic Bezier.
Vec3 bez(const Vec3& a, const Vec3& b, const Vec3& c, double t) {
    const double u = 1 - t;
    return a * (u * u) + b * (2 * u * t) + c * (t * t);
}
double bez(double a, double b, double c, double t) {
    const double u = 1 - t;
    return a * (u * u) + b * (2 * u * t) + c * (t * t);
}

// Segments a quadratic span needs: the curve's midpoint sits d/2 off the chord (d = control
// point's distance from the chord midpoint), and N segments leave about (d/2)/N^2.
int segmentsFor(const Vec3& a, const Vec3& b, const Vec3& c, int maxSubdiv) {
    const double d = length(b - (a + c) * 0.5);
    if (d < 0.05) return 1;
    return std::clamp(static_cast<int>(std::ceil(std::sqrt(d))), 2, std::max(2, maxSubdiv));
}

uint32_t packColor(const uint8_t c[4]) {
    return static_cast<uint32_t>(c[0]) | (static_cast<uint32_t>(c[1]) << 8) | (static_cast<uint32_t>(c[2]) << 16) |
           (static_cast<uint32_t>(c[3]) << 24);
}

// Basis (u, v) with cross(u, v) = n.
void planeBasis(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 ref = std::fabs(n.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    u = normalize(cross(ref, n));
    v = cross(n, u);
}

}  // namespace

Face planeFace(const Vec3& nIn, double dist, const std::string& material) {
    const Vec3 n = normalize(nIn);
    Vec3 u, v;
    planeBasis(n, u, v);
    Face f;
    f.material = material;
    // computePlane: normal = cross(p2 - p0, p1 - p0); cross(u, v) = n.
    const Vec3 o = n * dist;
    f.p[0] = o;
    f.p[2] = o + u * 64;
    f.p[1] = o + v * 64;
    f.normal = n;
    f.dist = dist;
    return f;
}

bool parse(const std::string& block, Patch& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    out = Patch{};
    std::istringstream in(block);
    std::string line;
    int state = 0;  // 0: before mesh/curve, 1: header lines, 2: size read, 3: inside a row
    std::vector<std::string> header;
    int rowsRead = 0;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t.rfind("//", 0) == 0) continue;
        if (state == 0) {
            if (t == "mesh" || t == "curve") {
                out.curve = t == "curve";
                state = 1;
            } else if (t != "{") {
                return fail("not a patch (" + t + ")");
            }
            continue;
        }
        if (state == 1) {
            if (t == "{") continue;
            const std::string lt = lower(t);
            if (lt.rfind("contents", 0) == 0) {
                if (lt.find("noncolliding") != std::string::npos || lt.find("nonsolid") != std::string::npos) out.nonColliding = true;
                continue;
            }
            if (lt.rfind("toolflags", 0) == 0 || lt.rfind("smoothing", 0) == 0) continue;
            int r, c;
            if (header.size() >= 2 && std::sscanf(t.c_str(), "%d %d", &r, &c) == 2) {
                if (r < 2 || c < 2 || r > 1024 || c > 1024) return fail("bad patch size");
                out.rows = r;
                out.cols = c;
                state = 2;
                continue;
            }
            header.push_back(t);
            if (header.size() == 1) out.material = t;
            continue;
        }
        if (t == "(") {
            if (state != 2) return fail("unexpected (");
            state = 3;
            continue;
        }
        if (t == ")") {
            if (state != 3) return fail("unexpected )");
            rowsRead++;
            if (out.verts.size() != static_cast<size_t>(rowsRead) * out.cols) return fail("patch row has the wrong vertex count");
            state = 2;
            continue;
        }
        if (t == "}") continue;
        if (state == 3) {
            std::istringstream ls(t);
            std::string tok;
            Vert v;
            bool havePos = false;
            while (ls >> tok) {
                if (tok == "v") {
                    ls >> v.p.x >> v.p.y >> v.p.z;
                    havePos = static_cast<bool>(ls);
                } else if (tok == "c") {
                    int c[4];
                    ls >> c[0] >> c[1] >> c[2] >> c[3];
                    for (int k = 0; k < 4; k++) v.rgba[k] = static_cast<uint8_t>(std::clamp(c[k], 0, 255));
                    out.hasColor = true;
                } else if (tok == "t") {
                    ls >> v.st[0] >> v.st[1];
                    double lu, lv;  // lightmap coordinates (optional): not used, the compiler bakes its own
                    std::streampos at = ls.tellg();
                    if (!(ls >> lu >> lv)) {
                        ls.clear();
                        ls.seekg(at);
                    }
                }
            }
            if (!havePos) return fail("patch vertex without a position");
            out.verts.push_back(v);
            continue;
        }
    }
    if (out.material.empty() || out.rows < 2 || rowsRead != out.rows) return fail("incomplete patch");
    if (out.curve && (out.rows % 2 == 0 || out.cols % 2 == 0)) return fail("curve patch size must be odd");
    return true;
}

Grid tessellate(const Patch& p, const TexSizeFn& texSize, int maxSubdiv) {
    double tw = 512, th = 512;
    if (texSize) texSize(p.material, tw, th);
    if (tw <= 0) tw = 512;
    if (th <= 0) th = 512;
    Grid g;
    // Each vertex: position, st, colour; then normals from the finished grid.
    struct GV {
        Vec3 p;
        double s, t;
        double c[4];
    };
    std::vector<GV> out;
    if (!p.curve) {
        g.w = p.cols;
        g.h = p.rows;
        for (const Vert& v : p.verts)
            out.push_back({v.p, v.st[0], v.st[1], {double(v.rgba[0]), double(v.rgba[1]), double(v.rgba[2]), double(v.rgba[3])}});
    } else {
        // Segments per section column / row: the most any span along that direction needs.
        const int sc = (p.cols - 1) / 2, sr = (p.rows - 1) / 2;
        std::vector<int> segC(sc, 1), segR(sr, 1);
        for (int r = 0; r < p.rows; r++)
            for (int s = 0; s < sc; s++)
                segC[s] = std::max(segC[s], segmentsFor(p.at(r, 2 * s).p, p.at(r, 2 * s + 1).p, p.at(r, 2 * s + 2).p, maxSubdiv));
        for (int c = 0; c < p.cols; c++)
            for (int s = 0; s < sr; s++)
                segR[s] = std::max(segR[s], segmentsFor(p.at(2 * s, c).p, p.at(2 * s + 1, c).p, p.at(2 * s + 2, c).p, maxSubdiv));
        // Parameter lists: (section, t) per output column / row.
        std::vector<std::pair<int, double>> cs, rs;
        for (int s = 0; s < sc; s++)
            for (int i = 0; i < segC[s] + (s == sc - 1 ? 1 : 0); i++) cs.push_back({s, static_cast<double>(i) / segC[s]});
        for (int s = 0; s < sr; s++)
            for (int i = 0; i < segR[s] + (s == sr - 1 ? 1 : 0); i++) rs.push_back({s, static_cast<double>(i) / segR[s]});
        g.w = static_cast<int>(cs.size());
        g.h = static_cast<int>(rs.size());
        for (const auto& [rsec, rt] : rs)
            for (const auto& [csec, ct] : cs) {
                // Evaluate the 3 control rows of this section along the columns, then across rows.
                GV q[3];
                for (int k = 0; k < 3; k++) {
                    const Vert& a = p.at(2 * rsec + k, 2 * csec);
                    const Vert& b = p.at(2 * rsec + k, 2 * csec + 1);
                    const Vert& c = p.at(2 * rsec + k, 2 * csec + 2);
                    q[k].p = bez(a.p, b.p, c.p, ct);
                    q[k].s = bez(a.st[0], b.st[0], c.st[0], ct);
                    q[k].t = bez(a.st[1], b.st[1], c.st[1], ct);
                    for (int j = 0; j < 4; j++) q[k].c[j] = bez(a.rgba[j], b.rgba[j], c.rgba[j], ct);
                }
                GV v;
                v.p = bez(q[0].p, q[1].p, q[2].p, rt);
                v.s = bez(q[0].s, q[1].s, q[2].s, rt);
                v.t = bez(q[0].t, q[1].t, q[2].t, rt);
                for (int j = 0; j < 4; j++) v.c[j] = bez(q[0].c[j], q[1].c[j], q[2].c[j], rt);
                out.push_back(v);
            }
    }
    const size_t n = out.size();
    g.p.resize(n);
    g.uv.resize(2 * n);
    g.color.resize(n);
    g.n.resize(n);
    for (size_t i = 0; i < n; i++) {
        g.p[i] = out[i].p;
        g.uv[2 * i] = out[i].s / tw;
        g.uv[2 * i + 1] = out[i].t / th;
        uint8_t c[4];
        for (int k = 0; k < 4; k++) c[k] = static_cast<uint8_t>(std::clamp(std::lround(out[i].c[k]), 0L, 255L));
        g.color[i] = packColor(c);
    }
    // Smooth normals: cross(d/dcol, d/drow) from neighbouring grid points (one-sided at the
    // edges); degenerate spots (a collapsed edge) take the average of their quads' normals.
    for (int r = 0; r < g.h; r++)
        for (int c = 0; c < g.w; c++) {
            const Vec3 dc = g.p[g.at(r, std::min(c + 1, g.w - 1))] - g.p[g.at(r, std::max(c - 1, 0))];
            const Vec3 dr = g.p[g.at(std::min(r + 1, g.h - 1), c)] - g.p[g.at(std::max(r - 1, 0), c)];
            Vec3 nn = cross(dc, dr);
            if (length(nn) < 1e-9) {
                for (int rr = std::max(r - 1, 0); rr < std::min(r + 1, g.h - 1); rr++)
                    for (int cc = std::max(c - 1, 0); cc < std::min(c + 1, g.w - 1); cc++) {
                        const Vec3 a = g.p[g.at(rr, cc)], b = g.p[g.at(rr, cc + 1)], d = g.p[g.at(rr + 1, cc)], e = g.p[g.at(rr + 1, cc + 1)];
                        nn += cross(b - a, d - a) + cross(e - d, e - b);
                    }
            }
            g.n[g.at(r, c)] = length(nn) > 1e-12 ? normalize(nn) : Vec3{0, 0, 1};
        }
    return g;
}

namespace {

// A face from grid vertices (in winding order, clockwise from the front); false if degenerate.
bool makeFace(const Grid& g, const std::vector<size_t>& idx, const std::string& material, Face& f) {
    const Vec3& a = g.p[idx[0]];
    Vec3 nn;
    for (size_t k = 1; k + 1 < idx.size(); k++) nn += cross(g.p[idx[k + 1]] - a, g.p[idx[k]] - a);  // clockwise winding
    if (length(nn) < 1e-6) return false;
    f = Face{};
    f.material = material;
    f.normal = normalize(nn);
    f.dist = dot(f.normal, a);
    f.p[0] = g.p[idx[0]];
    f.p[1] = g.p[idx[1]];
    f.p[2] = g.p[idx[2]];
    for (size_t i : idx) {
        f.winding.push_back(g.p[i]);
        f.uvs.push_back(static_cast<float>(g.uv[2 * i]));
        f.uvs.push_back(static_cast<float>(g.uv[2 * i + 1]));
        f.vnormals.push_back(g.n[i]);
        f.colors.push_back(g.color[i]);
    }
    return true;
}

}  // namespace

std::vector<Face> faces(const Grid& g, const std::string& material) {
    std::vector<Face> out;
    for (int r = 0; r + 1 < g.h; r++)
        for (int c = 0; c + 1 < g.w; c++) {
            const size_t v00 = g.at(r, c), v01 = g.at(r, c + 1), v10 = g.at(r + 1, c), v11 = g.at(r + 1, c + 1);
            // The two triangles (r+1,c) (r,c+1) (r,c) and (r+1,c) (r+1,c+1) (r,c+1); as one quad
            // (r,c) (r+1,c) (r+1,c+1) (r,c+1) clockwise when the four are coplanar.
            Face quad;
            if (makeFace(g, {v00, v10, v11, v01}, material, quad)) {
                bool planar = true;
                for (const Vec3& p : quad.winding) planar &= std::fabs(dot(quad.normal, p) - quad.dist) < 0.02;
                // Convex: every corner turns clockwise (seen from the front).
                for (size_t k = 0; k < 4 && planar; k++) {
                    const Vec3& p0 = quad.winding[k];
                    const Vec3& p1 = quad.winding[(k + 1) % 4];
                    const Vec3& p2 = quad.winding[(k + 2) % 4];
                    planar &= dot(cross(p2 - p1, p0 - p1), quad.normal) < 1e-9;
                }
                if (planar) {
                    out.push_back(std::move(quad));
                    continue;
                }
            }
            Face f;
            if (makeFace(g, {v10, v01, v00}, material, f)) out.push_back(f);
            if (makeFace(g, {v10, v11, v01}, material, f)) out.push_back(f);
        }
    return out;
}

std::vector<Brush> collisionBrushes(const Patch& p, double thickness, int maxSubdiv) {
    std::vector<Brush> out;
    if (p.nonColliding) return out;
    const Grid g = tessellate(p, nullptr, maxSubdiv);
    for (const Face& f : faces(g, p.material)) {
        // Prism: the face, its back side `thickness` behind, one side plane per edge.
        Brush b;
        b.faces.push_back(planeFace(f.normal, f.dist, p.material));
        b.faces.push_back(planeFace(-f.normal, -(f.dist - thickness), p.material));
        Vec3 c;
        for (const Vec3& v : f.winding) c += v;
        c = c * (1.0 / f.winding.size());
        for (size_t k = 0; k < f.winding.size(); k++) {
            const Vec3& a = f.winding[k];
            const Vec3& e = f.winding[(k + 1) % f.winding.size()];
            Vec3 sn = normalize(cross(e - a, f.normal));
            if (length(sn) < 0.5) continue;
            if (dot(sn, c - a) > 0) sn = -sn;  // outward
            b.faces.push_back(planeFace(sn, dot(sn, a), p.material));
        }
        if (b.rebuild()) out.push_back(std::move(b));
    }
    return out;
}

std::vector<Patch> entityPatches(const Entity& e) {
    std::vector<Patch> out;
    for (const std::string& raw : e.rawBlocks) {
        Patch p;
        if (parse(raw, p)) out.push_back(std::move(p));
    }
    return out;
}

}  // namespace patch
