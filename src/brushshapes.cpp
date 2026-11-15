#include "brushshapes.h"

#include <algorithm>
#include <cmath>

namespace shapes {
namespace {
constexpr double kPi = 3.14159265358979323846;
const char* kTail = "lightmap_gray 16384 16384 0 0 0 0";

// A convex brush from its face polygons. Each face takes three of its polygon's points, ordered
// so the normal points away from the brush's centre (faces store points clockwise from outside).
bool fromPolygons(const std::vector<std::vector<Vec3>>& polys, const std::string& material, Brush& out) {
    Vec3 centre{0, 0, 0};
    size_t n = 0;
    for (const auto& poly : polys)
        for (const Vec3& v : poly) centre += v, n++;
    if (!n) return false;
    centre = centre * (1.0 / static_cast<double>(n));
    out = Brush{};
    for (const auto& poly : polys) {
        if (poly.size() < 3) continue;
        // three points spanning the largest area (robust against near-collinear triples)
        size_t bi = 0, bj = 1, bk = 2;
        double best = -1;
        for (size_t j = 1; j + 1 < poly.size(); j++)
            for (size_t k = j + 1; k < poly.size(); k++) {
                double a = length(cross(poly[j] - poly[0], poly[k] - poly[0]));
                if (a > best) best = a, bj = j, bk = k;
            }
        if (best < 1e-6) continue;
        Vec3 a = poly[bi], b = poly[bj], c = poly[bk];
        Vec3 normal = cross(b - a, c - a) * -1.0;  // the brush convention: clockwise from outside
        Vec3 mid{0, 0, 0};
        for (const Vec3& v : poly) mid += v;
        mid = mid * (1.0 / static_cast<double>(poly.size()));
        if (dot(normal, mid - centre) < 0) std::swap(b, c);
        Face f;
        f.p[0] = a;
        f.p[1] = b;
        f.p[2] = c;
        f.material = material;
        f.tail = kTail;
        out.faces.push_back(std::move(f));
    }
    return out.faces.size() >= 4 && out.rebuild();
}

// A prism: `base` (a convex polygon) swept by `extrude`.
bool prism(const std::vector<Vec3>& base, const Vec3& extrude, const std::string& material, Brush& out) {
    std::vector<std::vector<Vec3>> polys;
    std::vector<Vec3> top;
    for (const Vec3& v : base) top.push_back(v + extrude);
    polys.push_back(base);
    polys.push_back(top);
    for (size_t i = 0; i < base.size(); i++) {
        size_t j = (i + 1) % base.size();
        polys.push_back({base[i], base[j], top[j], top[i]});
    }
    return fromPolygons(polys, material, out);
}

// A brush from planes (outward normal, point on the plane); the windings come from clipping.
Brush fromPlanes(const std::vector<std::pair<Vec3, Vec3>>& planes, const std::string& material) {
    Brush b;
    for (const auto& [n, a] : planes) {
        Vec3 u = std::fabs(n.z) > 0.9 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
        u = normalize(u - n * dot(u, n));
        Vec3 v = cross(n, u);  // cross(u, v) == n: the points give an outward normal (as makeBox)
        Face f;
        f.p[0] = a;
        f.p[1] = a + v * 64;
        f.p[2] = a + u * 64;
        f.material = material;
        f.tail = kTail;
        b.faces.push_back(std::move(f));
    }
    b.rebuild();
    return b;
}

}  // namespace

namespace {
double cross2(const Point2& o, const Point2& a, const Point2& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}
double signedArea(const std::vector<Point2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); i++) {
        const Point2& q = p[i];
        const Point2& r = p[(i + 1) % p.size()];
        a += q[0] * r[1] - r[0] * q[1];
    }
    return a * 0.5;
}
bool segmentsTouch(const Point2& a, const Point2& b, const Point2& c, const Point2& d) {
    auto orient = [](const Point2& p, const Point2& q, const Point2& r) {
        double v = cross2(p, q, r);
        return v > 1e-9 ? 1 : v < -1e-9 ? -1 : 0;
    };
    auto onSeg = [](const Point2& p, const Point2& q, const Point2& r) {  // r on segment pq (collinear)
        return std::min(p[0], q[0]) - 1e-9 <= r[0] && r[0] <= std::max(p[0], q[0]) + 1e-9 && std::min(p[1], q[1]) - 1e-9 <= r[1] &&
               r[1] <= std::max(p[1], q[1]) + 1e-9;
    };
    const int o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
    if (o1 != o2 && o3 != o4) return true;
    return (o1 == 0 && onSeg(a, b, c)) || (o2 == 0 && onSeg(a, b, d)) || (o3 == 0 && onSeg(c, d, a)) || (o4 == 0 && onSeg(c, d, b));
}
// Drops repeated points and points on a straight line between their neighbours.
std::vector<Point2> simplify(std::vector<Point2> p) {
    for (bool again = true; again && p.size() >= 3;) {
        again = false;
        for (size_t i = 0; i < p.size() && p.size() >= 3; i++) {
            const Point2& prev = p[(i + p.size() - 1) % p.size()];
            const Point2& next = p[(i + 1) % p.size()];
            const bool dup = std::fabs(p[i][0] - next[0]) < 1e-9 && std::fabs(p[i][1] - next[1]) < 1e-9;
            if (dup || std::fabs(cross2(prev, p[i], next)) < 1e-9) {
                p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));
                again = true;
                break;
            }
        }
    }
    return p;
}
bool isConvex(const std::vector<Point2>& p) {  // counter-clockwise, strictly
    for (size_t i = 0; i < p.size(); i++)
        if (cross2(p[(i + p.size() - 1) % p.size()], p[i], p[(i + 1) % p.size()]) <= 1e-9) return false;
    return true;
}
bool insideTriangle(const Point2& a, const Point2& b, const Point2& c, const Point2& p) {
    return cross2(a, b, p) >= -1e-9 && cross2(b, c, p) >= -1e-9 && cross2(c, a, p) >= -1e-9;
}
}  // namespace

std::vector<std::vector<Point2>> convexPieces(std::vector<Point2> outline, std::string& error) {
    std::vector<Point2> p = simplify(std::move(outline));
    if (p.size() < 3) {
        error = "the outline needs at least 3 points that aren't in a line";
        return {};
    }
    for (size_t i = 0; i < p.size(); i++)
        for (size_t j = i + 1; j < p.size(); j++) {
            if (j == i + 1 || (i == 0 && j == p.size() - 1)) continue;  // neighbouring edges share a point
            if (segmentsTouch(p[i], p[(i + 1) % p.size()], p[j], p[(j + 1) % p.size()])) {
                error = "the outline crosses itself";
                return {};
            }
        }
    const double area = signedArea(p);
    if (std::fabs(area) < 1.0) {
        error = "the outline encloses no area";
        return {};
    }
    if (area < 0) std::reverse(p.begin(), p.end());
    if (isConvex(p)) return {p};
    // Ear clipping into triangles (indices into p).
    std::vector<int> idx(p.size());
    for (size_t i = 0; i < p.size(); i++) idx[i] = static_cast<int>(i);
    std::vector<std::vector<int>> polys;
    for (size_t guard = 0; idx.size() > 3 && guard < 10000; guard++) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); i++) {
            const int a = idx[(i + idx.size() - 1) % idx.size()], b = idx[i], c = idx[(i + 1) % idx.size()];
            if (cross2(p[a], p[b], p[c]) <= 1e-9) continue;  // reflex
            bool empty = true;
            for (int q : idx)
                if (q != a && q != b && q != c && insideTriangle(p[a], p[b], p[c], p[q])) {
                    empty = false;
                    break;
                }
            if (!empty) continue;
            polys.push_back({a, b, c});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            error = "couldn't split the outline into convex pieces";
            return {};
        }
    }
    polys.push_back(idx);
    // Merge neighbours across a shared diagonal while the result stays convex.
    auto coords = [&](const std::vector<int>& poly) {
        std::vector<Point2> c;
        for (int i : poly) c.push_back(p[i]);
        return c;
    };
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < polys.size() && !merged; i++)
            for (size_t j = i + 1; j < polys.size() && !merged; j++) {
                const auto& A = polys[i];
                const auto& B = polys[j];
                for (size_t ai = 0; ai < A.size() && !merged; ai++) {
                    const int a = A[ai], b = A[(ai + 1) % A.size()];
                    for (size_t bj = 0; bj < B.size(); bj++) {
                        if (B[bj] != b || B[(bj + 1) % B.size()] != a) continue;
                        // A from b round to a, then B's points strictly between a and b
                        std::vector<int> m;
                        for (size_t k = 0; k < A.size(); k++) m.push_back(A[(ai + 1 + k) % A.size()]);
                        for (size_t k = 2; k < B.size(); k++) m.push_back(B[(bj + k) % B.size()]);
                        if (isConvex(coords(m))) {
                            polys[i] = m;
                            polys.erase(polys.begin() + static_cast<std::ptrdiff_t>(j));
                            merged = true;
                        }
                        break;
                    }
                }
            }
    }
    std::vector<std::vector<Point2>> out;
    for (const auto& poly : polys) out.push_back(coords(poly));
    return out;
}

std::vector<Brush> fromOutline(const std::vector<Point2>& outline, int u, int v, int w, double w0, double w1,
                               const std::string& material, std::string& error) {
    if (w1 < w0) std::swap(w0, w1);
    if (w1 - w0 < 1) {
        error = "no depth to extrude through";
        return {};
    }
    std::vector<Brush> out;
    for (const auto& piece : convexPieces(outline, error)) {
        std::vector<Vec3> base;
        for (const Point2& q : piece) {
            Vec3 pt{0, 0, 0};
            pt[u] = q[0];
            pt[v] = q[1];
            pt[w] = w0;
            base.push_back(pt);
        }
        Vec3 ext{0, 0, 0};
        ext[w] = w1 - w0;
        Brush b;
        if (!prism(base, ext, material, b)) {
            error = "a piece of the outline is too thin to make a brush";
            return {};
        }
        out.push_back(std::move(b));
    }
    return out;
}

bool convexBrush(const std::vector<std::vector<Vec3>>& polygons, const std::string& material, Brush& out) {
    return fromPolygons(polygons, material, out);
}

const char* name(Shape s) {
    switch (s) {
        case Shape::Cylinder: return "Cylinder";
        case Shape::Cone: return "Cone";
        case Shape::Pyramid: return "Pyramid";
        case Shape::Wedge: return "Ramp";
        case Shape::Sphere: return "Sphere";
        case Shape::Stairs: return "Stairs";
        case Shape::Arch: return "Arch";
        case Shape::Pipe: return "Pipe";
        case Shape::Room: return "Room";
    }
    return "?";
}

const char* tooltip(Shape s) {
    switch (s) {
        case Shape::Cylinder: return "N-sided cylinder standing on Z (Sides)";
        case Shape::Cone: return "N-sided cone, point up (Sides)";
        case Shape::Pyramid: return "Four-sided pyramid on the box's base";
        case Shape::Wedge: return "Ramp rising toward +X";
        case Shape::Sphere: return "Faceted sphere / ellipsoid (Sides around, Sides/2 rings)";
        case Shape::Stairs: return "Stairs rising toward +X, one brush per step (Steps)";
        case Shape::Arch: return "Half-ring arch across X, extruded along Y (Sides segments, Thickness)";
        case Shape::Pipe: return "Hollow cylinder, one brush per segment (Sides, Thickness)";
        case Shape::Room: return "Hollow box: floor, ceiling and four walls (Thickness)";
    }
    return "";
}

std::vector<Brush> make(Shape s, const Vec3& mins, const Vec3& maxs, const Params& p, const std::string& mat) {
    std::vector<Brush> out;
    const double x0 = mins.x, y0 = mins.y, z0 = mins.z, x1 = maxs.x, y1 = maxs.y, z1 = maxs.z;
    const double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    if (dx < 1 || dy < 1 || dz < 1) return out;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
    const int sides = std::clamp(p.sides, 3, 64);
    const double t = std::max(1.0, p.thickness);
    auto ring = [&](double rx, double ry, double z, int n) {
        std::vector<Vec3> r;
        for (int i = 0; i < n; i++) {
            const double a = 2 * kPi * i / n + kPi / n;  // flat side facing each axis for even n
            r.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a), z});
        }
        return r;
    };
    auto add = [&](bool ok, Brush& b) {
        if (ok) out.push_back(std::move(b));
    };
    Brush b;
    switch (s) {
        case Shape::Cylinder:
            add(prism(ring(dx / 2, dy / 2, z0, sides), {0, 0, dz}, mat, b), b);
            break;
        case Shape::Cone:
        case Shape::Pyramid: {
            std::vector<Vec3> base = s == Shape::Pyramid ? std::vector<Vec3>{{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}}
                                                         : ring(dx / 2, dy / 2, z0, sides);
            const Vec3 apex{cx, cy, z1};
            std::vector<std::vector<Vec3>> polys = {base};
            for (size_t i = 0; i < base.size(); i++) polys.push_back({base[i], base[(i + 1) % base.size()], apex});
            add(fromPolygons(polys, mat, b), b);
            break;
        }
        case Shape::Wedge: {
            const Vec3 A{x0, y0, z0}, B{x1, y0, z0}, C{x1, y1, z0}, D{x0, y1, z0}, E{x1, y0, z1}, F{x1, y1, z1};
            add(fromPolygons({{A, B, C, D}, {B, C, F, E}, {A, E, F, D}, {A, B, E}, {D, C, F}}, mat, b), b);
            break;
        }
        case Shape::Sphere: {
            // Tangent planes of the ellipsoid in the box (sides around, sides/2 latitude rings).
            const double rx = dx / 2, ry = dy / 2, rz = dz / 2;
            const int rings = std::max(2, sides / 2);
            // (+ the box's sides: tangent-plane facets overshoot it slightly between the planes)
            std::vector<std::pair<Vec3, Vec3>> planes = {{{0, 0, 1}, {cx, cy, z1}},  {{0, 0, -1}, {cx, cy, z0}}, {{1, 0, 0}, {x1, cy, cz}},
                                                         {{-1, 0, 0}, {x0, cy, cz}}, {{0, 1, 0}, {cx, y1, cz}},  {{0, -1, 0}, {cx, y0, cz}}};
            for (int k = 1; k < rings; k++) {
                const double lat = -kPi / 2 + kPi * k / rings;
                for (int j = 0; j < sides; j++) {
                    const double lon = 2 * kPi * j / sides + kPi / sides;
                    const Vec3 d{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
                    const Vec3 pt{cx + rx * d.x, cy + ry * d.y, cz + rz * d.z};
                    planes.push_back({normalize(Vec3{d.x / rx, d.y / ry, d.z / rz}), pt});
                }
            }
            Brush sb = fromPlanes(planes, mat);
            if (sb.rebuild()) out.push_back(std::move(sb));
            break;
        }
        case Shape::Stairs: {
            const int n = std::clamp(p.steps, 1, 128);
            for (int i = 0; i < n; i++)
                out.push_back(makeBox({x0 + dx * i / n, y0, z0}, {x1, y1, z0 + dz * (i + 1) / n}, mat));
            break;
        }
        case Shape::Arch: {
            // Half ring over X (outer = the box), segments extruded across Y.
            const double rxo = dx / 2, rzo = dz, rxi = rxo - t, rzi = rzo - t;
            if (rxi <= 0 || rzi <= 0) return {};
            for (int i = 0; i < sides; i++) {
                const double a0 = kPi * i / sides, a1 = kPi * (i + 1) / sides;
                auto pt = [&](double rx, double rz, double a) { return Vec3{cx + rx * std::cos(a), y0, z0 + rz * std::sin(a)}; };
                std::vector<Vec3> quad = {pt(rxo, rzo, a0), pt(rxo, rzo, a1), pt(rxi, rzi, a1), pt(rxi, rzi, a0)};
                add(prism(quad, {0, dy, 0}, mat, b), b);
            }
            break;
        }
        case Shape::Pipe: {
            const double rxo = dx / 2, ryo = dy / 2, rxi = rxo - t, ryi = ryo - t;
            if (rxi <= 0 || ryi <= 0) return {};
            for (int i = 0; i < sides; i++) {
                const double a0 = 2 * kPi * i / sides, a1 = 2 * kPi * (i + 1) / sides;
                auto pt = [&](double rx, double ry, double a) { return Vec3{cx + rx * std::cos(a), cy + ry * std::sin(a), z0}; };
                std::vector<Vec3> quad = {pt(rxo, ryo, a0), pt(rxo, ryo, a1), pt(rxi, ryi, a1), pt(rxi, ryi, a0)};
                add(prism(quad, {0, 0, dz}, mat, b), b);
            }
            break;
        }
        case Shape::Room: {
            if (dx <= 2 * t || dy <= 2 * t || dz <= 2 * t) return {};
            out.push_back(makeBox({x0, y0, z0}, {x1, y1, z0 + t}, mat));          // floor
            out.push_back(makeBox({x0, y0, z1 - t}, {x1, y1, z1}, mat));          // ceiling
            out.push_back(makeBox({x0, y0, z0 + t}, {x0 + t, y1, z1 - t}, mat));  // walls -X / +X
            out.push_back(makeBox({x1 - t, y0, z0 + t}, {x1, y1, z1 - t}, mat));
            out.push_back(makeBox({x0 + t, y0, z0 + t}, {x1 - t, y0 + t, z1 - t}, mat));  // walls -Y / +Y
            out.push_back(makeBox({x0 + t, y1 - t, z0 + t}, {x1 - t, y1, z1 - t}, mat));
            break;
        }
    }
    return out;
}

}  // namespace shapes
