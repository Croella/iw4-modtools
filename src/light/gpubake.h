#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// GPU back end of the light baker (light::bake with Settings::gpu): the same rays as the CPU
// path, traced by D3D11 compute shaders against the same BVH. The baker keeps the geometry
// (sample points, atlas, final combine) on the CPU and hands this the ray work:
//   sun    - visibility of the sun from each sun-mask sample,
//   lights - point / spot light with a shadow ray per light per luxel,
//   gather - cosine-weighted sky + one bounce off the previous round's light per luxel.
// Work goes out in chunks small enough to stay clear of the driver's 2 s GPU timeout, and the
// cancel flag is checked between them. Any failure returns false with a reason, and the baker
// falls back to the CPU.
namespace light::gpubake {

struct Tri {  // 44 bytes, same layout as the shader's
    float a[3], e1[3], e2[3];
    int face;  // lightmapped face index, -1 = blocks light only
    int sky;
};
struct Node {
    float mn[3];
    int start;
    float mx[3];
    int count;  // leaf: tris [start, start + count); inner: 0, children start, start + 1
};
struct Map {  // a face's luxel rect (FaceMap)
    int x, y, w, h, axisU, axisV;
    float minU, minV;
};
struct Sample {
    float p[3], n[3];
};
struct Light {
    float pos[3], radius;
    float color[3], spot;  // spot > 0.5: cone
    float dir[3], cosOuter;
    float cosInner, pad[3];
};

class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Hardware D3D11 device (feature level 11) + compiled kernels. `device` = the adapter's name.
    bool init(std::string& device, std::string& error);
    // Uploads the scene. width = atlas width (luxel lookups), ls = luxel size.
    bool setScene(const std::vector<Tri>& tris, const std::vector<Node>& nodes, const std::vector<Map>& maps, int width, float ls,
                  std::string& error);

    // The callback is told the fraction done (0..1) after each chunk; returning false cancels.
    using Tick = bool (*)(void* user, float done);

    // 255 where the ray towards the sun escapes or hits sky, else 0.
    bool sun(const std::vector<Sample>& samples, const float sunDir[3], std::vector<uint8_t>& vis, Tick tick, void* user,
             std::string& error);
    // Sum of the lights (with shadow rays) at each sample, rgb.
    bool lights(const std::vector<Sample>& samples, const std::vector<Light>& lights, std::vector<float>& rgb, Tick tick, void* user,
                std::string& error);
    // Sky + bounce: `rays` cosine-weighted rays per sample; sky -> skyColor, a lightmapped face ->
    // source (rgb per atlas luxel) at the hit * albedo when bounce is set. Average per sample, rgb.
    bool gather(const std::vector<Sample>& samples, const std::vector<float>& source, const float skyColor[3], int rays, float albedo,
                bool bounce, uint32_t seed, std::vector<float>& rgb, Tick tick, void* user, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace light::gpubake
