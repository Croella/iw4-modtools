#include "brush.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr double kOnPlane = 1e-3;
constexpr double kHuge = 262144.0;

bool computePlane(Face& f) {
    Vec3 n = cross(f.p[2] - f.p[0], f.p[1] - f.p[0]);
    if (length(n) < 1e-9) return false;
    f.normal = normalize(n);
    f.dist = dot(f.normal, f.p[0]);
    return true;
}

std::vector<Vec3> baseWinding(const Face& f) {
    int major = 0;
    for (int i = 1; i < 3; i++)
        if (std::fabs(f.normal[i]) > std::fabs(f.normal[major])) major = i;
    Vec3 up = (major == 2) ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    up = normalize(up - f.normal * dot(up, f.normal));
    Vec3 right = cross(up, f.normal);
    Vec3 org = f.normal * f.dist;
    up = up * kHuge;
    right = right * kHuge;
    return {org - right + up, org + right + up, org + right - up, org - right - up};
}

// Keeps the part of the polygon behind the plane (Sutherland-Hodgman).
void clipBehind(std::vector<Vec3>& w, const Vec3& n, double d) {
    if (w.empty()) return;
    std::vector<Vec3> out;
    out.reserve(w.size() + 1);
    for (size_t i = 0; i < w.size(); i++) {
        const Vec3& a = w[i];
        const Vec3& b = w[(i + 1) % w.size()];
        double da = dot(n, a) - d, db = dot(n, b) - d;
        if (da <= kOnPlane) out.push_back(a);
        if ((da > kOnPlane && db < -kOnPlane) || (da < -kOnPlane && db > kOnPlane))
            out.push_back(a + (b - a) * (da / (da - db)));
    }
    w.swap(out);
}

double cleanCoord(double v) {
    double r = std::round(v);
    return std::fabs(v - r) < kOnPlane ? r : v;
}

void baseAxes(const Vec3& n, Vec3& u, Vec3& v) {
    static const Vec3 table[6][3] = {
        {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}}, {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}},
        {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}}};
    int best = 0;
    double bestDot = -2;
    for (int i = 0; i < 6; i++) {
        double d = dot(n, table[i][0]);
        if (d > bestDot) { bestDot = d; best = i; }
    }
    u = table[best][1];
    v = table[best][2];
}
}  // namespace

bool Brush::rebuild() {
    for (Face& f : faces)
        if (!computePlane(f)) return false;
    bounds = AABB{};
    int solidFaces = 0;
    for (size_t i = 0; i < faces.size(); i++) {
        Face& f = faces[i];
        f.winding = baseWinding(f);
        for (size_t j = 0; j < faces.size() && !f.winding.empty(); j++)
            if (i != j) clipBehind(f.winding, faces[j].normal, faces[j].dist);
        if (f.winding.size() < 3) {
            f.winding.clear();
            continue;
        }
        solidFaces++;
        for (Vec3& v : f.winding) {
            for (int k = 0; k < 3; k++) v[k] = cleanCoord(v[k]);
            bounds.add(v);
        }
    }
    if (solidFaces < 4 || !bounds.valid()) return false;
    for (int i = 0; i < 3; i++)
        if (bounds.maxs[i] - bounds.mins[i] < 0.01) return false;
    return true;
}

void Brush::translate(const Vec3& d) {
    for (Face& f : faces) {
        for (Vec3& p : f.p) p += d;
        for (Vec3& v : f.winding) v += d;
        f.dist = dot(f.normal, f.p[0]);
    }
    if (bounds.valid()) {
        bounds.mins += d;
        bounds.maxs += d;
    }
}

bool Brush::moveFaces(int axis, int sign, double amount) {
    Brush next = *this;
    Vec3 d;
    d[axis] = amount;
    bool moved = false;
    for (Face& f : next.faces) {
        if (f.normal[axis] * sign > 1e-6) {
            for (Vec3& p : f.p) p += d;
            moved = true;
        }
    }
    if (!moved || !next.rebuild()) return false;
    *this = std::move(next);
    return true;
}

void Brush::setMaterial(const std::string& name) {
    for (Face& f : faces) f.material = name;
}

Brush makeBox(const Vec3& mins, const Vec3& maxs, const std::string& material) {
    Brush b;
    static const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        // Picks u, v with cross(u, v) == n so the points give an outward normal.
        Vec3 u = std::fabs(n.z) > 0.5 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
        Vec3 v = cross(n, u);
        Vec3 a{n.x > 0 ? maxs.x : mins.x, n.y > 0 ? maxs.y : mins.y, n.z > 0 ? maxs.z : mins.z};
        Face f;
        f.p[0] = a;
        f.p[1] = a + v * 64;
        f.p[2] = a + u * 64;
        f.material = material;
        f.tail = "lightmap_gray 16384 16384 0 0 0 0";
        b.faces.push_back(std::move(f));
    }
    b.rebuild();
    return b;
}

bool rayBrush(const Brush& b, const Vec3& origin, const Vec3& dir, double& tOut, int& faceOut) {
    double tEnter = 0, tExit = std::numeric_limits<double>::max();
    int enterFace = -1;
    for (size_t i = 0; i < b.faces.size(); i++) {
        const Face& f = b.faces[i];
        double denom = dot(f.normal, dir);
        double dist = f.dist - dot(f.normal, origin);
        if (std::fabs(denom) < 1e-12) {
            if (dist < 0) return false;  // parallel and outside this plane
            continue;
        }
        double t = dist / denom;
        if (denom < 0) {
            if (t > tEnter) { tEnter = t; enterFace = static_cast<int>(i); }
        } else if (t < tExit) {
            tExit = t;
        }
        if (tEnter > tExit) return false;
    }
    if (enterFace < 0) return false;  // ray starts inside the brush
    tOut = tEnter;
    faceOut = enterFace;
    return true;
}

bool Brush::twoSided() const {
    for (const std::string& a : attrs)
        if (a == kTwoSidedAttr) return true;
    return false;
}

void Brush::setTwoSided(bool on) {
    attrs.erase(std::remove(attrs.begin(), attrs.end(), std::string(kTwoSidedAttr)), attrs.end());
    if (on) attrs.push_back(kTwoSidedAttr);
}

namespace {
constexpr const char* kSurfaceAttr = "iw4r_surface ";
}

std::string Brush::surfaceType() const {
    for (const std::string& a : attrs)
        if (a.rfind(kSurfaceAttr, 0) == 0) {
            std::string v = a.substr(std::strlen(kSurfaceAttr));
            while (!v.empty() && (v.back() == ';' || v.back() == ' ')) v.pop_back();
            return v;
        }
    return {};
}

void Brush::setSurfaceType(const std::string& name) {
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(), [](const std::string& a) { return a.rfind(kSurfaceAttr, 0) == 0; }),
                attrs.end());
    if (!name.empty()) attrs.push_back(kSurfaceAttr + name + ";");
}

const std::vector<std::string>& surfaceTypeNames() {
    // Order confirmed against stock materials' surfaceTypeBits (1 << (type - 1)): metal 13,
    // brick 2, dirt 6, gravel 11, water 20, wood 21, asphalt 22, slush 30, ...
    static const std::vector<std::string> names = {
        "default", "bark",  "brick", "carpet", "cloth",   "concrete", "dirt",    "flesh", "foliage", "glass",  "grass",
        "gravel",  "ice",   "metal", "mud",    "paper",   "plaster",  "rock",    "sand",  "snow",    "water",  "wood",
        "asphalt", "ceramic", "plastic", "rubber", "cushion", "fruit", "paintedmetal", "riotshield", "slush"};
    return names;
}

int surfaceTypeIndex(const std::string& name) {
    const auto& n = surfaceTypeNames();
    for (size_t i = 0; i < n.size(); i++)
        if (n[i] == name) return static_cast<int>(i);
    return -1;
}

Face mirroredFace(const Face& f) {
    Face m = f;
    std::reverse(m.winding.begin(), m.winding.end());
    m.normal = -f.normal;
    m.dist = -f.dist;
    std::swap(m.p[0], m.p[2]);
    return m;
}

void faceUV(const Face& f, const Vec3& p, float& u, float& v) {
    Vec3 ua, va;
    baseAxes(f.normal, ua, va);
    double s = dot(p, ua), t = dot(p, va);
    double rad = f.tex.rot * 3.14159265358979323846 / 180.0;
    double c = std::cos(rad), sn = std::sin(rad);
    double rs = s * c - t * sn, rt = s * sn + t * c;
    double w = f.tex.w != 0 ? f.tex.w : 64, h = f.tex.h != 0 ? f.tex.h : 64;
    u = static_cast<float>((rs + f.tex.sx) / w);
    v = static_cast<float>((rt + f.tex.sy) / h);
}
