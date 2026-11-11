#pragma once
#include <cmath>
#include <limits>

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    double& operator[](int i) { return (&x)[i]; }
    double operator[](int i) const { return (&x)[i]; }
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3& a) {
    double l = length(a);
    return l > 0 ? a * (1.0 / l) : Vec3{};
}

struct AABB {
    Vec3 mins{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
    Vec3 maxs{-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
              -std::numeric_limits<double>::max()};
    bool valid() const { return mins.x <= maxs.x; }
    void add(const Vec3& p) {
        for (int i = 0; i < 3; i++) {
            if (p[i] < mins[i]) mins[i] = p[i];
            if (p[i] > maxs[i]) maxs[i] = p[i];
        }
    }
    void add(const AABB& b) {
        if (b.valid()) { add(b.mins); add(b.maxs); }
    }
    Vec3 center() const { return (mins + maxs) * 0.5; }
};

inline double snapTo(double v, double grid) { return std::round(v / grid) * grid; }
