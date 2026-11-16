#include "zonebuild.h"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <stdexcept>
#include <miniz.h>
#include "fastfile.h"
#include "zone/effectshaders.h"
#include "model/imageio.h"
#include "model/xmodelexport.h"
#include "model/xanimexport.h"
#include "zone/animbuild.h"
#include "zone/weapondef.h"
#include "zone/pathnodes.h"
#include "zone/efx.h"
#include "zone/bo1fx.h"
#include "zone/zonesim.h"
#include <sstream>
#include <set>

#include "iw4.h"

using zone::ZoneWriter;

namespace {
// A pointer field holding the on-disk "follow" sentinel (-1): the pointee is written inline next.
template <class T>
T* follow() {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(zone::kPtrFollow));
}
// A pointer field holding an encoded back-reference (zone::offsetPointer).
template <class T>
T* ref(uint64_t encoded) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(encoded));
}
uint64_t virtualRef(uint32_t base, size_t index, size_t elemSize) {
    return zone::offsetPointer(zone::BLOCK_VIRTUAL, static_cast<uint32_t>(base + index * elemSize));
}

// Load_<Asset>Ptr, the wrapper every asset pointer goes through (top level or nested):
// push TEMP, align 4, reserve the insert slot for -2, load the struct + sub-data, pop.
// Returns a back-reference to the insert slot (0 for -1): later pointers to the same asset use
// it, and the loader resolves them through the slot (DB_ConvertOffsetToPointer on the slot).
uint64_t loadAssetPtr(ZoneWriter& w, uint64_t header, const std::function<void(ZoneWriter&)>& load) {
    w.push(zone::BLOCK_TEMP);
    w.align(zone::kAlign4);
    uint64_t slot = header == zone::kPtrInsert ? w.insertPointer() : 0;
    load(w);
    w.pop();
    return slot;
}

// A cplane_s array (align 4); returns its VIRTUAL position for back-references.
uint32_t writePlanes(ZoneWriter& w, const std::vector<clip::Plane>& planes) {
    uint32_t at = w.align(zone::kAlign4);
    for (const clip::Plane& p : planes) {
        iw4::cplane_s g{};
        std::memcpy(g.normal, p.normal, sizeof(g.normal));
        g.dist = p.dist;
        g.type = p.type;
        g.pad[0] = p.signbits;  // CoD4 keeps signbits here; harmless if unused
        w.writeStruct(g);
    }
    return at;
}

// Push/pop around a RUNTIME array: the loader zero-fills it, so it has no file bytes.
void runtimeArray(ZoneWriter& w, uint32_t align, size_t bytes) {
    w.push(zone::BLOCK_RUNTIME);
    w.align(align);
    w.zeros(bytes);
    w.pop();
}

// Load_GfxImagePtr (0x11a860) -> Load_GfxImage (0x11a7b0): struct 0x28 (TEMP); push VIRTUAL;
// name; texture = Load_GfxImageLoadDefPtr (0x11ad80): push TEMP, header 16 + resourceSize pixel
// bytes (the pixels only live in TEMP until the texture is created); pop; pop.
uint64_t writeImage(ZoneWriter& w, const gfx::Image& im) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        iw4::GfxImage g{};
        g.texture.loadDef = reinterpret_cast<iw4::GfxImageLoadDef*>(static_cast<uintptr_t>(zone::kPtrInsert));
        g.mapType = static_cast<char>(im.mapType);
        g.semantic = static_cast<iw4::TextureSemantic>(im.semantic);
        g.category = static_cast<char>(im.category);
        g.cardMemory.platform[0] = g.cardMemory.platform[1] = static_cast<int>(im.pixels.size());
        g.width = im.width;
        g.height = im.height;
        g.depth = 1;
        g.delayLoadPixels = true;  // as in stock lightmaps/probes, which carry their pixels inline
        g.name = follow<const char>();
        ww.writeStruct(g);
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring(im.name);
        loadAssetPtr(ww, zone::kPtrInsert, [&](ZoneWriter& w3) {
            uint8_t hdr[16] = {};
            hdr[0] = im.levelCount;
            uint32_t size = static_cast<uint32_t>(im.pixels.size());
            std::memcpy(hdr + 4, &im.loadFlags, 4);
            std::memcpy(hdr + 8, &im.format, 4);
            std::memcpy(hdr + 12, &size, 4);
            w3.write(hdr, sizeof(hdr));
            w3.write(im.pixels.data(), im.pixels.size());
        });
        ww.pop();
    });
}

void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

uint32_t writeEffectTechsetHeader(ZoneWriter& w, const std::string& name);

// Sky cubemap image: the stock header (cube, load-from-file category) under our image name, with
// a 16-byte load def of resourceSize 0, so the game streams images/<name>.iwi from the IWDs.
uint64_t writeSkyImage(ZoneWriter& w, const sky::Source& src, const std::string& name) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> im = src.image;
        put64(im, 0, zone::kPtrInsert);  // texture.loadDef: inline + slot (as stock)
        put64(im, 32, zone::kPtrFollow);  // name
        ww.write(im.data(), im.size());
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring(name);
        loadAssetPtr(ww, zone::kPtrInsert, [&](ZoneWriter& w3) {
            std::vector<uint8_t> ld = src.imageLoadDef;
            std::memset(ld.data() + 12, 0, 4);  // resourceSize 0: pixels come from the .iwi
            w3.write(ld.data(), ld.size());
        });
        ww.pop();
    });
}

// A custom sky: zone_source/skies/<name>.png|.tga, an equirectangular panorama (2:1; the centre
// column looks down world +X, yaw grows to the left as in the game, the top row is straight up).
// Resampled into the six faces of a D3D cubemap sampled by world direction (the stock sky shader
// does texCUBE(world dir)), stored as sqrt (the shader squares rgb * a). Inline A8R8G8B8, one level,
// flags 0x10002 (cube, no mips) as the stock sky's load def.
bool customSkyImage(const std::string& zoneSourceDir, const std::string& skyImage, gfx::Image& out, std::vector<std::string>* warnings) {
    namespace fs = std::filesystem;
    if (zoneSourceDir.empty()) return false;
    std::string stem = skyImage;
    if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, "_ft") == 0) stem.resize(stem.size() - 3);
    std::string found;
    for (const char* ext : {".png", ".tga", ".PNG", ".TGA"}) {
        std::error_code ec;
        fs::path p = fs::path(zoneSourceDir) / "skies" / (stem + ext);
        if (fs::is_regular_file(p, ec)) {
            found = p.string();
            break;
        }
    }
    if (found.empty()) return false;
    imageio::Rgba pano;
    std::string err;
    if (!imageio::load(found, pano, err) || pano.width < 8 || pano.height < 4) {
        if (warnings) warnings->push_back("sky " + found + ": " + (err.empty() ? "too small" : err));
        return false;
    }
    int face = 1;
    while (face * 4 < pano.width && face < 512) face *= 2;  // ~ panorama width / 4, power of two, <= 512 (6 MB)
    out = {};
    out.name = "iw4r_" + stem;
    out.mapType = 5;
    out.semantic = 2;
    out.category = 2;
    out.width = out.height = static_cast<uint16_t>(face);
    out.levelCount = 1;
    out.loadFlags = 0x10002;
    out.format = 0x15;  // A8R8G8B8
    out.pixels.resize(6ull * face * face * 4);
    auto sample = [&](double x, double y, double z, uint8_t* bgra) {  // bilinear in the panorama
        const double kPi = 3.14159265358979323846;
        const double yaw = std::atan2(y, x), pitch = std::atan2(z, std::sqrt(x * x + y * y));
        double u = (0.5 - yaw / (2 * kPi)) * pano.width - 0.5, v = (0.5 - pitch / kPi) * pano.height - 0.5;
        v = std::clamp(v, 0.0, pano.height - 1.0);
        const int u0 = static_cast<int>(std::floor(u)), v0 = static_cast<int>(std::floor(v));
        const double fu = u - u0, fv = v - v0;
        double acc[3] = {0, 0, 0};
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                const int px = ((u0 + i) % pano.width + pano.width) % pano.width, py = std::min(v0 + j, pano.height - 1);
                const double wgt = (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
                const uint8_t* s = &pano.pixels[(static_cast<size_t>(py) * pano.width + px) * 4];
                for (int k = 0; k < 3; k++) {
                    const double lin = std::pow(s[k] / 255.0, 2.2);  // the panorama is ordinary sRGB-ish art
                    acc[k] += wgt * lin;
                }
            }
        for (int k = 0; k < 3; k++) {
            const double stored = std::sqrt(std::max(0.0, acc[k]));  // the shader squares it back
            bgra[2 - k] = static_cast<uint8_t>(std::lround(std::clamp(stored, 0.0, 1.0) * 255));
        }
        bgra[3] = 255;
    };
    for (int f = 0; f < 6; f++)
        for (int ty = 0; ty < face; ty++)
            for (int tx = 0; tx < face; tx++) {
                const double sc = 2.0 * (tx + 0.5) / face - 1, tc = 2.0 * (ty + 0.5) / face - 1;
                double x = 0, y = 0, z = 0;
                switch (f) {  // D3D cube faces: +X, -X, +Y, -Y, +Z, -Z
                    case 0: x = 1, y = -tc, z = -sc; break;
                    case 1: x = -1, y = -tc, z = sc; break;
                    case 2: x = sc, y = 1, z = tc; break;
                    case 3: x = sc, y = -1, z = -tc; break;
                    case 4: x = sc, y = -tc, z = 1; break;
                    default: x = -sc, y = -tc, z = -1; break;
                }
                sample(x, y, z, &out.pixels[((static_cast<size_t>(f) * face + ty) * face + tx) * 4]);
            }
    if (warnings) warnings->push_back("sky: embedded " + found + " as a " + std::to_string(face) + "x" + std::to_string(face) + " cube");
    return true;
}

// The embedded sky material, written in full as Load_Material walks it (see zonesim materialPtr):
// struct; name; technique set inline (only slot 4 "sky": one pass, rebuilt "pp" vertex decl, the
// stock sky.hlsl shaders and args); texture table (colorMap -> the sky image, back-referenced to
// the slot GfxWorld.skies wrote); state bits.
uint64_t writeSkyMaterial(ZoneWriter& w, const sky::Source& src, uint64_t imageSlot, const std::string& name) {
    using namespace zone;
    return loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> m = src.material;
        put64(m, 0, kPtrFollow);   // info.name
        put64(m, 88, kPtrFollow);  // techniqueSet
        put64(m, 96, kPtrFollow);  // textureTable
        put64(m, 104, 0);          // constantTable
        put64(m, 112, src.stateBits.empty() ? 0 : kPtrFollow);
        m[80] = 1;
        m[81] = 0;
        m[82] = static_cast<uint8_t>(src.stateBits.size() / 8);
        ww.write(m.data(), m.size());
        ww.push(BLOCK_VIRTUAL);
        ww.xstring(name);
        loadAssetPtr(ww, kPtrFollow, [&](ZoneWriter& w2) {  // Load_MaterialTechniqueSetPtr
            writeEffectTechsetHeader(w2, "iw4r_sky");
            std::vector<uint8_t> tech = src.technique;
            put64(tech, 0, kPtrFollow);
            tech[10] = 1;
            tech[11] = 0;
            w2.write(tech.data(), tech.size());
            std::vector<uint8_t> pass = src.pass;
            put64(pass, 0, kPtrFollow);   // vertexDecl
            put64(pass, 8, kPtrFollow);   // vertexShader
            put64(pass, 16, kPtrFollow);  // pixelShader
            put64(pass, 32, src.args.empty() ? 0 : kPtrFollow);
            w2.write(pass.data(), pass.size());
            loadAssetPtr(w2, kPtrFollow, [&](ZoneWriter& w3) {  // vertex decl "pp"
                std::vector<uint8_t> vd(0xB0, 0);
                put64(vd, 0, kPtrFollow);
                vd[8] = 1;  // one stream: source position (0) -> dest position (0)
                w3.write(vd.data(), vd.size());
                w3.push(BLOCK_VIRTUAL);
                w3.xstring("iw4r_sky_pp");
                w3.pop();
            });
            auto shader = [&](const sky::ShaderBlob& sh, const char* shName) {
                loadAssetPtr(w2, kPtrFollow, [&](ZoneWriter& w3) {
                    std::vector<uint8_t> h = sh.header;
                    put64(h, 0, kPtrFollow);
                    put64(h, 8, 0);  // D3D object (runtime)
                    put64(h, 16, sh.code.empty() ? 0 : kPtrFollow);
                    w3.write(h.data(), h.size());
                    w3.push(BLOCK_VIRTUAL);
                    w3.xstring(shName);
                    if (!sh.code.empty()) {
                        w3.align(kAlign4);
                        w3.write(sh.code.data(), sh.code.size());
                    }
                    w3.pop();
                });
            };
            shader(src.vs, "iw4r_sky_vs");
            shader(src.ps, "iw4r_sky_ps");
            if (!src.args.empty()) {
                std::vector<uint8_t> args = src.args;
                for (size_t a = 0; a < src.literals.size(); a++)
                    if (!src.literals[a].empty()) put64(args, 16 * a + 8, kPtrFollow);
                w2.align(kAlign4);
                w2.write(args.data(), args.size());
                for (const auto& lit : src.literals)
                    if (!lit.empty()) {
                        w2.align(kAlign4);
                        w2.write(lit.data(), lit.size());
                    }
            }
            w2.xstring("sky");  // technique name
            w2.pop();
        });
        ww.align(kAlign4);
        std::vector<uint8_t> td = src.textureDef;
        put64(td, 8, imageSlot);
        ww.write(td.data(), td.size());
        if (!src.stateBits.empty()) {
            ww.align(kAlign4);
            ww.write(src.stateBits.data(), src.stateBits.size());
        }
        ww.pop();
    });
}

// An embedded technique set's 0x198 header for one inline technique in slot 4 ("unlit"), with the
// lit slots 9..36 back-referencing that same technique, as stock effect/sky sets do (all of their
// lit slots share one technique). The game assumes materials of the same sort key agree on having
// those slots: a material sort comparator (iw4sp+0x496d0) reads techniques[9]->passArray[0] of one
// material whenever the other has it, so leaving them null crashes next to lit materials.
// The slot-4 technique is the first thing allocated after the set's name, so its VIRTUAL offset is
// known before the header is written: align4(VIRTUAL pos + name + NUL). Returns that offset.
uint32_t writeEffectTechsetHeader(ZoneWriter& w, const std::string& name) {
    using namespace zone;
    const uint32_t techAt = (w.posIn(BLOCK_VIRTUAL) + static_cast<uint32_t>(name.size()) + 1 + 3) & ~3u;
    std::vector<uint8_t> ts(0x198, 0);
    put64(ts, 0, kPtrFollow);
    put64(ts, 24 + 8 * 4, kPtrFollow);
    for (int t = 9; t <= 36; t++) put64(ts, 24 + 8 * t, offsetPointer(BLOCK_VIRTUAL, techAt));
    w.write(ts.data(), ts.size());
    w.push(BLOCK_VIRTUAL);
    w.xstring(name);
    if (w.align(kAlign4) != techAt) throw std::logic_error("embedded technique set: technique offset mismatch");
    return techAt;
}

// An image that streams from images/<name>.iwi: the stock header (category "load from file")
// and a 16-byte load def with resourceSize 0. Inline + insert slot, so later uses back-reference it.
uint64_t writeFileImage(ZoneWriter& w, const std::vector<uint8_t>& header, const std::vector<uint8_t>& loadDef,
                        const std::string& name) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> im = header;
        put64(im, 0, zone::kPtrInsert);   // texture.loadDef
        put64(im, 32, zone::kPtrFollow);  // name
        ww.write(im.data(), im.size());
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring(name);
        loadAssetPtr(ww, zone::kPtrInsert, [&](ZoneWriter& w3) {
            std::vector<uint8_t> ld = loadDef.size() == 16 ? loadDef : std::vector<uint8_t>(16, 0);
            std::memset(ld.data() + 12, 0, 4);  // resourceSize 0: pixels come from the .iwi
            w3.write(ld.data(), ld.size());
        });
        ww.pop();
    });
}

// A stock world material written in full (Load_Material order, see zonesim materialPtr): struct;
// name; technique set as a by-name reference (",wc_l_sm_b0c0..." resolves to common.ff's);
// texture table (colorMap / normalMap / specularMap) with each image inline the first time and
// back-referenced after; constants; state bits.
uint64_t writeWorldMaterial(ZoneWriter& w, const matdef::Def& d, std::map<std::string, uint64_t>& imageSlots,
                            const gfx::Image* embeddedColor = nullptr) {
    using namespace zone;
    const std::vector<const matdef::Texture*> tex = matdef::usedTextures(d);
    const std::string techset = matdef::techsetFor(d);
    return loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> m = d.header;
        put64(m, 0, kPtrFollow);
        put64(m, 88, kPtrFollow);
        put64(m, 96, tex.empty() ? 0 : kPtrFollow);
        put64(m, 104, d.constants.empty() ? 0 : kPtrFollow);
        put64(m, 112, d.stateBits.empty() ? 0 : kPtrFollow);
        m[80] = static_cast<uint8_t>(tex.size());
        m[81] = static_cast<uint8_t>(d.constants.size() / 32);
        m[82] = static_cast<uint8_t>(d.stateBits.size() / 8);
        ww.write(m.data(), m.size());
        ww.push(BLOCK_VIRTUAL);
        ww.xstring(d.name);
        loadAssetPtr(ww, kPtrFollow, [&](ZoneWriter& w2) {  // technique set by name
            std::vector<uint8_t> ts(0x198, 0);
            put64(ts, 0, kPtrFollow);
            w2.write(ts.data(), ts.size());
            w2.push(BLOCK_VIRTUAL);
            w2.xstring("," + techset);
            w2.pop();
        });
        if (!tex.empty()) {
            ww.align(kAlign4);
            std::vector<bool> inlineImage(tex.size());
            for (size_t i = 0; i < tex.size(); i++) {
                std::vector<uint8_t> def = tex[i]->def;
                auto it = imageSlots.find(tex[i]->image);
                inlineImage[i] = it == imageSlots.end();
                put64(def, 8, inlineImage[i] ? kPtrInsert : it->second);
                ww.write(def.data(), def.size());
            }
            for (size_t i = 0; i < tex.size(); i++)
                if (inlineImage[i] && !imageSlots.count(tex[i]->image))
                    imageSlots[tex[i]->image] = (embeddedColor && tex[i]->kind() == 'c')
                                                    ? writeImage(ww, *embeddedColor)
                                                    : writeFileImage(ww, tex[i]->imageHeader, tex[i]->loadDef, tex[i]->image);
        }
        if (!d.constants.empty()) {
            ww.align(kAlign16);
            ww.write(d.constants.data(), d.constants.size());
        }
        if (!d.stateBits.empty()) {
            ww.align(kAlign4);
            ww.write(d.stateBits.data(), d.stateBits.size());
        }
        ww.pop();
    });
}

// iw4-radiant's own "falloff + uv-anim + additive" technique set (tools/shaders/falloff_add_ua.hlsl),
// written inline once: slot 4 only, one pass. Arguments are the stock wc_unlit_falloff_add_lin_ua
// pass's (minus its pixel literal, which our pixel shader doesn't need): per-prim worldMatrix,
// per-object viewProjectionMatrix, then material constants falloffBeginColor/EndColor/
// uvAnimParms/falloffParms, the colorMap sampler, and fogConsts/gameTime. The vertex declaration
// routes position, color, texcoord and normal (pairs: 00 00, 01 02, 02 05, 03 01).
void writeFalloffTechset(ZoneWriter& w) {
    using namespace zone;
    writeEffectTechsetHeader(w, "iw4r_falloff_add_ua");
    std::vector<uint8_t> tech(16, 0);
    put64(tech, 0, kPtrFollow);
    tech[10] = 1;  // passCount
    w.write(tech.data(), tech.size());
    struct Arg {
        uint16_t type, dest;
        uint32_t a, b;
    };
    static const Arg args[] = {
        {3, 4, 0x04000062, 0},  // code const: worldMatrix (index 0x62, rows 4)  per-prim
        {3, 0, 0x04000056, 0},  // code const: viewProjectionMatrix (0x56, 4)  per-object
        {0, 14, 0x3d05a1f2, 0},          // material const: falloffBeginColor
        {0, 15, 0x6b1da6fa, 0},          // material const: falloffEndColor
        {0, 20, 0x70ebaf95, 0},          // material const: uvAnimParms
        {0, 13, 0xbdde5cf5, 0},          // material const: falloffParms
        {2, 0, 0xa0ab1041, 0},           // material sampler: colorMap -> s0
        {3, 21, 0x01000025, 0},          // code const: fogConsts
        {3, 22, 0x01000007, 0},          // code const: gameTime
    };
    std::vector<uint8_t> pass(40, 0);
    put64(pass, 0, kPtrFollow);
    put64(pass, 8, kPtrFollow);
    put64(pass, 16, kPtrFollow);
    pass[24] = 1;  // perPrimArgCount
    pass[25] = 1;  // perObjArgCount
    pass[26] = static_cast<uint8_t>(std::size(args) - 2);  // stableArgCount
    put64(pass, 32, kPtrFollow);
    w.write(pass.data(), pass.size());
    loadAssetPtr(w, kPtrFollow, [&](ZoneWriter& w3) {
        std::vector<uint8_t> vd(0xB0, 0);
        put64(vd, 0, kPtrFollow);
        vd[8] = 4;
        const uint8_t routing[8] = {0, 0, 1, 2, 2, 5, 3, 1};
        std::memcpy(vd.data() + 0x10, routing, sizeof routing);
        w3.write(vd.data(), vd.size());
        w3.push(BLOCK_VIRTUAL);
        w3.xstring("iw4r_ppcc0t0t0nn");
        w3.pop();
    });
    auto shader = [&](const uint8_t* code, size_t size, const char* name) {
        loadAssetPtr(w, kPtrFollow, [&](ZoneWriter& w3) {
            std::vector<uint8_t> h(0x20, 0);
            put64(h, 0, kPtrFollow);
            put64(h, 16, kPtrFollow);
            uint16_t dwords = static_cast<uint16_t>(size / 4);
            std::memcpy(h.data() + 24, &dwords, 2);
            // loadForRenderer (+26) stays 0: stock SM3 shaders have 0 there (1 = not created for SM3)
            w3.write(h.data(), h.size());
            w3.push(BLOCK_VIRTUAL);
            w3.xstring(name);
            w3.align(kAlign4);
            w3.write(code, size);
            w3.pop();
        });
    };
    shader(effectshaders::falloff_add_ua_vs, sizeof effectshaders::falloff_add_ua_vs, "iw4r_falloff_add_ua_vs");
    shader(effectshaders::falloff_add_ua_ps, sizeof effectshaders::falloff_add_ua_ps, "iw4r_falloff_add_ua_ps");
    w.align(kAlign4);
    for (const Arg& a : args) {
        w.write(&a.type, 2);
        w.write(&a.dest, 2);
        w.u32(0);
        w.u32(a.a);
        w.u32(a.b);
    }
    w.xstring("effect_add_falloff_fog_lin_ua");
    w.pop();
}

// A stock falloff/additive material (see matdef::Shading::FalloffAdd) with the embedded technique
// set: inline the first time (+ insert slot), back-referenced after. A missing uvAnimParms
// constant (the non-scrolling variant) is added as zero so the shared shader's argument resolves.
uint64_t writeFalloffMaterial(ZoneWriter& w, const matdef::Def& d, std::map<std::string, uint64_t>& imageSlots,
                              uint64_t& techsetSlot) {
    using namespace zone;
    const matdef::Texture* color = nullptr;
    for (const matdef::Texture* t : matdef::usedTextures(d))
        if (t->kind() == 'c') color = t;
    std::vector<uint8_t> constants = d.constants;
    if (!matdef::hasConstant(d, matdef::kHashUvAnimParms)) {
        std::vector<uint8_t> c(32, 0);
        uint32_t h = matdef::kHashUvAnimParms;
        std::memcpy(c.data(), &h, 4);
        std::memcpy(c.data() + 4, "uvAnimParms", 11);
        constants.insert(constants.end(), c.begin(), c.end());
    }
    return loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> m = d.header;
        put64(m, 0, kPtrFollow);
        put64(m, 88, techsetSlot ? techsetSlot : kPtrInsert);
        put64(m, 96, kPtrFollow);
        put64(m, 104, kPtrFollow);
        put64(m, 112, d.stateBits.empty() ? 0 : kPtrFollow);
        m[80] = 1;
        m[81] = static_cast<uint8_t>(constants.size() / 32);
        m[82] = static_cast<uint8_t>(d.stateBits.size() / 8);
        ww.write(m.data(), m.size());
        ww.push(BLOCK_VIRTUAL);
        ww.xstring(d.name);
        if (!techsetSlot) techsetSlot = loadAssetPtr(ww, kPtrInsert, [&](ZoneWriter& w2) { writeFalloffTechset(w2); });
        ww.align(kAlign4);
        std::vector<uint8_t> def = color->def;
        auto it = imageSlots.find(color->image);
        const bool inl = it == imageSlots.end();
        put64(def, 8, inl ? kPtrInsert : it->second);
        ww.write(def.data(), def.size());
        if (inl) imageSlots[color->image] = writeFileImage(ww, color->imageHeader, color->loadDef, color->image);
        ww.align(kAlign16);
        ww.write(constants.data(), constants.size());
        if (!d.stateBits.empty()) {
            ww.align(kAlign4);
            ww.write(d.stateBits.data(), d.stateBits.size());
        }
        ww.pop();
    });
}

// How one of a model's materials is written: a full stock mc/ material, or a custom one built on a
// stock "mc_l_sm_b0c0" template (its header/state bits/constants) with the colorMap swapped for
// the model's texture, embedded from zone_source/images or streamed from the IWDs by name.
struct ModelMaterialPlan {
    matdef::Def def;
    bool embedded = false;
    gfx::Image image;
};

// Load_XModel (0x123260), in its order (struct 408, VIRTUAL): name; boneNames (script string
// indices); parentList / quats / trans for the non-root bones (index - parent, local int16
// quat, local float3); partClassification; baseMat (model-space bind pose, transWeight 2);
// materialHandles (one per surface, Load_MaterialHandle each, repeats back-referenced);
// lodInfo[0].modelSurfs -> XModelSurfs (0x1238c0) -> surfaces (0x123bb0): vertsBlend (skinned),
// verts in VERTEX, rigid vertex list (single-bone models), indices in INDEX; boneInfo per bone;
// no phys preset/collmap. Single-bone layout matches stock viewmodel_nvg, skinned matches
// viewmodel_base_viewhands (deformed surfaces, no rigid lists).
ZoneAsset buildXModel(const modelbuild::Model& md, std::vector<uint16_t> boneNameIndex, std::vector<ModelMaterialPlan> mats) {
    return {IW4_XMODEL, [md, boneNameIndex, mats](ZoneWriter& w) {
                using namespace zone;
                const uint32_t n = static_cast<uint32_t>(md.surfaces.size());
                const size_t nb = md.bones.size(), nr = static_cast<size_t>(md.numRootBones), nonRoot = nb - nr;
                uint32_t partBits[6] = {};
                for (const auto& s : md.surfaces)
                    for (int k = 0; k < 6; k++) partBits[k] |= s.partBits[k];
                iw4::XModel m{};
                m.name = follow<const char>();
                m.numBones = static_cast<unsigned char>(nb);
                m.numRootBones = static_cast<unsigned char>(nr);
                m.numsurfs = static_cast<unsigned char>(n);
                m.scale = 1.0f;
                m.boneNames = follow<unsigned short>();
                if (nonRoot) {
                    m.parentList = follow<unsigned char>();
                    m.quats = follow<short>();
                    m.trans = follow<float>();
                }
                m.partClassification = follow<unsigned char>();
                m.baseMat = follow<iw4::DObjAnimMat>();
                m.materialHandles = follow<iw4::Material*>();
                m.lodInfo[0].dist = 1000.0f;
                m.lodInfo[0].numsurfs = static_cast<unsigned short>(n);
                m.lodInfo[0].modelSurfs = reinterpret_cast<iw4::XModelSurfs*>(static_cast<uintptr_t>(kPtrInsert));
                for (int k = 0; k < 6; k++) m.lodInfo[0].partBits[k] = static_cast<int>(partBits[k]);
                m.numLods = 1;
                m.collLod = 0xFF;  // -1: no collision lod
                m.boneInfo = follow<iw4::XBoneInfo>();
                m.radius = md.radius;
                for (int k = 0; k < 3; k++) {
                    m.bounds.midPoint[k] = (md.mins[k] + md.maxs[k]) * 0.5f;
                    m.bounds.halfSize[k] = (md.maxs[k] - md.mins[k]) * 0.5f;
                }
                size_t mem = 408 + nb * (2 + 1 + 32 + 28) + nonRoot * (1 + 8 + 12);
                for (const auto& s : md.surfaces) mem += 88 + 32 * s.verts.size() + 2 * s.tris.size() + 2 * s.blend.size() + 16;
                m.memUsage = static_cast<int>(mem);
                w.writeStruct(m);
                w.push(BLOCK_VIRTUAL);
                w.xstring(md.name);
                w.align(kAlign2);
                w.write(boneNameIndex.data(), 2 * nb);  // boneNames
                if (nonRoot) {
                    w.align(kAlign1);
                    for (size_t i = nr; i < nb; i++) w.u8(static_cast<uint8_t>(i - static_cast<size_t>(md.bones[i].parent)));
                    w.align(kAlign2);
                    for (size_t i = nr; i < nb; i++)
                        for (int k = 0; k < 4; k++) {
                            int16_t q = static_cast<int16_t>(std::clamp(std::lround(md.bones[i].localQuat[k] * 32767.0f), -32767L, 32767L));
                            w.write(&q, 2);
                        }
                    w.align(kAlign4);
                    for (size_t i = nr; i < nb; i++) w.write(md.bones[i].modelTrans.data(), 12);
                }
                w.align(kAlign1);
                for (size_t i = 0; i < nb; i++) w.u8(0);  // partClassification
                w.align(kAlign4);
                for (const modelbuild::Bone& b : md.bones) {
                    const float qq = b.bindQuat[0] * b.bindQuat[0] + b.bindQuat[1] * b.bindQuat[1] + b.bindQuat[2] * b.bindQuat[2] +
                                     b.bindQuat[3] * b.bindQuat[3];
                    const float mat[8] = {b.bindQuat[0], b.bindQuat[1], b.bindQuat[2], b.bindQuat[3],
                                          b.bindTrans[0], b.bindTrans[1], b.bindTrans[2], qq > 0 ? 2.0f / qq : 2.0f};
                    w.write(mat, sizeof mat);
                }
                // materialHandles: first use of each material inline (+slot), repeats point at its slot.
                w.align(kAlign4);
                for (uint32_t i = 0; i < n; i++) w.u64(kPtrInsert);
                std::map<std::string, uint64_t> imageSlots;
                std::vector<uint64_t> matSlot(mats.size(), 0);
                const size_t handlesAt = w.bytes().size() - 8ull * n;
                for (uint32_t i = 0; i < n; i++) {
                    int mi = md.surfaces[i].material;
                    if (matSlot[mi]) {
                        std::memcpy(w.bytes().data() + handlesAt + 8ull * i, &matSlot[mi], 8);
                        continue;
                    }
                    const ModelMaterialPlan& p = mats[mi];
                    matSlot[mi] = writeWorldMaterial(w, p.def, imageSlots, p.embedded ? &p.image : nullptr);
                }
                // lodInfo[0].modelSurfs (lods 1-3 are null).
                loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) {
                    iw4::XModelSurfs ms{};
                    ms.name = follow<const char>();
                    ms.surfs = follow<iw4::XSurface>();
                    ms.numsurfs = static_cast<unsigned short>(n);
                    for (int k = 0; k < 6; k++) ms.partBits[k] = static_cast<int>(partBits[k]);
                    ww.writeStruct(ms);
                    ww.push(BLOCK_VIRTUAL);
                    ww.xstring(md.name + "_lod0");
                    ww.align(kAlign4);
                    uint16_t baseTri = 0, baseVert = 0;
                    for (const modelbuild::Surface& s : md.surfaces) {
                        iw4::XSurface xs{};
                        xs.deformed = md.skinned;
                        xs.vertCount = static_cast<unsigned short>(s.verts.size());
                        xs.triCount = static_cast<unsigned short>(s.tris.size() / 3);
                        xs.baseTriIndex = baseTri;
                        xs.baseVertIndex = baseVert;
                        xs.triIndices = follow<unsigned short>();
                        if (md.skinned) {
                            for (int k = 0; k < 4; k++) xs.vertInfo.vertCount[k] = s.blendCounts[k];
                            xs.vertInfo.vertsBlend = follow<unsigned short>();
                        } else {
                            xs.vertListCount = 1;
                            xs.vertList = follow<iw4::XRigidVertList>();
                        }
                        xs.verts0 = follow<iw4::GfxPackedVertex>();
                        for (int k = 0; k < 6; k++) xs.partBits[k] = static_cast<int>(s.partBits[k]);
                        ww.writeStruct(xs);
                        baseTri = static_cast<uint16_t>(baseTri + xs.triCount);
                        baseVert = static_cast<uint16_t>(baseVert + xs.vertCount);
                    }
                    for (const modelbuild::Surface& s : md.surfaces) {
                        if (md.skinned) {
                            ww.align(kAlign2);
                            ww.write(s.blend.data(), 2 * s.blend.size());
                        }
                        ww.push(BLOCK_VERTEX);
                        ww.align(kAlign16);
                        ww.write(s.verts.data(), s.verts.size() * sizeof(modelbuild::PackedVertex));
                        ww.pop();
                        if (!md.skinned) {
                            ww.align(kAlign4);
                            iw4::XRigidVertList rl{};
                            rl.vertCount = static_cast<unsigned short>(s.verts.size());
                            rl.triCount = static_cast<unsigned short>(s.tris.size() / 3);
                            ww.writeStruct(rl);
                        }
                        ww.push(BLOCK_INDEX);
                        ww.align(kAlign16);
                        ww.write(s.tris.data(), s.tris.size() * 2);
                        ww.pop();
                    }
                    ww.pop();
                });
                w.align(kAlign4);
                for (const modelbuild::Bone& b : md.bones) {
                    iw4::XBoneInfo bi{};
                    for (int k = 0; k < 3; k++) {
                        bi.bounds.midPoint[k] = b.boundsMid[k];
                        bi.bounds.halfSize[k] = b.boundsHalf[k];
                    }
                    bi.radiusSquared = b.radiusSquared;
                    w.writeStruct(bi);
                }
                w.pop();
            }};
}

// ---- weapons (zone_source/weapons/<name>, IW4 weapon files) ----
uint64_t writeMaterialRef(ZoneWriter& w, const std::string& name);  // below
namespace {
void put64At(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

// Load_XModelPtr for a by-name reference: XModel with only its name (",name"); DB_AddXAsset
// resolves it to the loaded model (ours, written earlier in this zone, or another zone's).
void writeXModelRef(ZoneWriter& w, const std::string& name) {
    loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        iw4::XModel m{};
        m.name = follow<const char>();
        ww.writeStruct(m);
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring("," + name);
        ww.pop();
    });
}
// Load_FxEffectDefHandle (0x119000) -> 0x118f30: struct 40 (name, flags, totalSize,
// msecLoopingLife, elemDefCount looping/oneShot/emission, elemDefs), VIRTUAL; a reference has
// only its name.
void writeFxRef(ZoneWriter& w, const std::string& name) {
    loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        uint8_t fx[40] = {};
        uint64_t f = zone::kPtrFollow;
        std::memcpy(fx, &f, 8);
        ww.write(fx, sizeof fx);
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring("," + name);
        ww.pop();
    });
}
// A weapon sound (0x280b30 -> 0x123b50 -> 0x123a40): align 4, the snd_alias_list_t name slot
// (-1), then the name; the game looks the alias up by name when the weapon loads.
void writeSoundRef(ZoneWriter& w, const std::string& name) {
    w.align(zone::kAlign4);
    w.u64(zone::kPtrFollow);
    w.xstring(name);
}
// Anim names: every slot non-null ("" when unused), as the game's weapon-file loader leaves them.
void writeStringArray(ZoneWriter& w, const std::array<std::string, weapondef::kAnimCount>& s) {
    w.align(zone::kAlign4);
    for (size_t i = 0; i < s.size(); i++) w.u64(zone::kPtrFollow);
    for (const std::string& x : s) w.xstring(x);
}
}  // namespace

// Load_WeaponCompleteDefPtr (0x120b70) -> Load_WeaponCompleteDef (0x120810, 160 bytes, VIRTUAL)
// -> Load_WeaponDef (0x120c30, 2192 bytes), every pointer in the loaders' order. Models, effects
// and materials are by-name references; sounds and anims are names the game looks up.
ZoneAsset buildWeapon(const weapondef::Weapon& wd, std::vector<uint16_t> hideTags,
                      std::array<std::vector<uint16_t>, 4> noteMaps) {
    return {IW4_WEAPON, [wd, hideTags, noteMaps](ZoneWriter& w) {
                using namespace zone;
                namespace o = weapondef::off;
                std::vector<uint8_t> c = wd.complete, d = wd.def;
                const uint64_t F = kPtrFollow, I = kPtrInsert;
                // WeaponCompleteDef pointers
                put64At(c, o::cName, F);
                put64At(c, o::cWeapDef, F);
                for (const auto& [off, s] : wd.completeStrings) put64At(c, off, F);
                put64At(c, o::cHideTags, F);  // always (zeros when none)
                put64At(c, o::cXAnims, F);
                for (const auto& [off, s] : wd.completeMaterials) put64At(c, off, I);
                // WeaponDef pointers
                for (const auto& [off, s] : wd.defStrings) put64At(d, off, F);
                put64At(d, o::dGunModels, F);
                put64At(d, o::dAnimsR, F);
                put64At(d, o::dAnimsL, F);
                for (int k = 0; k < 4; k++) put64At(d, o::dNoteSoundKeys + 8 * k, F);  // always
                for (const auto& [off, s] : wd.defModels) put64At(d, off, I);
                for (const auto& [off, s] : wd.defFx) put64At(d, off, I);
                for (const auto& [off, s] : wd.defMaterials) put64At(d, off, I);
                for (const auto& [off, s] : wd.defSounds) put64At(d, off, F);
                if (!wd.bounceSound.empty()) put64At(d, o::dBounceSound, F);
                put64At(d, o::dWorldModels, F);
                if (wd.hasParallelBounce) put64At(d, o::dParallelBounce, F);
                if (wd.hasPerpendicularBounce) put64At(d, o::dPerpendicularBounce, F);
                if (wd.hasLocationDamage) put64At(d, o::dLocationDamage, F);

                auto str = [&](const std::map<uint16_t, std::string>& m, uint16_t off) {
                    auto it = m.find(off);
                    if (it != m.end()) w.xstring(it->second);
                };
                auto model = [&](uint16_t off) {
                    auto it = wd.defModels.find(off);
                    if (it != wd.defModels.end()) writeXModelRef(w, it->second);
                };
                auto fx = [&](uint16_t off) {
                    auto it = wd.defFx.find(off);
                    if (it != wd.defFx.end()) writeFxRef(w, it->second);
                };
                auto mat = [&](const std::map<uint16_t, std::string>& m, uint16_t off) {
                    auto it = m.find(off);
                    if (it != m.end()) writeMaterialRef(w, it->second);
                };
                auto snd = [&](uint16_t off) {
                    auto it = wd.defSounds.find(off);
                    if (it != wd.defSounds.end()) writeSoundRef(w, it->second);
                };
                auto modelArray = [&](const std::array<std::string, weapondef::kModelCount>& names) {
                    w.align(kAlign4);
                    for (const std::string& n : names) w.u64(n.empty() ? 0 : I);
                    for (const std::string& n : names)
                        if (!n.empty()) writeXModelRef(w, n);
                };
                auto floats = [&](const float* f, size_t n) {
                    w.align(kAlign4);
                    w.write(f, 4 * n);
                };

                w.write(c.data(), c.size());
                w.push(BLOCK_VIRTUAL);
                w.xstring(wd.name);
                // ---- WeaponDef (0x120c30; no push of its own) ----
                w.align(kAlign4);
                w.write(d.data(), d.size());
                str(wd.defStrings, o::dOverlayName);
                modelArray(wd.gunModels);
                model(o::dHandModel);
                writeStringArray(w, wd.anims);
                writeStringArray(w, wd.anims);
                str(wd.defStrings, o::dModeName);
                for (int k = 0; k < 4; k++) {
                        w.align(kAlign2);
                        std::vector<uint16_t> m(16, 0);
                        std::copy(noteMaps[k].begin(), noteMaps[k].begin() + std::min<size_t>(16, noteMaps[k].size()), m.begin());
                        w.write(m.data(), 32);
                    }
                fx(o::dViewFlash);
                fx(o::dWorldFlash);
                for (uint16_t off = o::dSoundsBegin; off < o::dSoundsEnd; off += 8) snd(off);
                if (!wd.bounceSound.empty()) {
                    w.align(kAlign4);
                    for (int s = 0; s < weapondef::kSurfaceTypes; s++) w.u64(F);
                    for (int s = 0; s < weapondef::kSurfaceTypes; s++)
                        writeSoundRef(w, wd.bounceSound + "_" + weapondef::kSurfaceNames[s]);
                }
                fx(o::dViewShellEject);
                fx(o::dWorldShellEject);
                fx(o::dViewLastShotEject);
                fx(o::dWorldLastShotEject);
                mat(wd.defMaterials, o::dReticleCenter);
                mat(wd.defMaterials, o::dReticleSide);
                modelArray(wd.worldModels);
                model(o::dWorldClipModel);
                model(o::dRocketModel);
                model(o::dKnifeModel);
                model(o::dWorldKnifeModel);
                mat(wd.defMaterials, o::dHudIcon);
                mat(wd.defMaterials, o::dPickupIcon);
                mat(wd.defMaterials, o::dAmmoCounterIcon);
                str(wd.defStrings, o::dAmmoName);
                str(wd.defStrings, o::dClipName);
                str(wd.defStrings, o::dSharedAmmoCapName);
                mat(wd.defMaterials, o::dOverlayMaterial);
                mat(wd.defMaterials, o::dOverlayMaterialLowRes);
                mat(wd.defMaterials, o::dOverlayMaterialEMP);
                mat(wd.defMaterials, o::dOverlayMaterialEMPLowRes);
                // physCollmap: null
                model(o::dProjectileModel);
                fx(o::dProjExplosionEffect);
                fx(o::dProjDudEffect);
                snd(o::dProjExplosionSound);
                snd(o::dProjDudSound);
                if (wd.hasParallelBounce) floats(wd.parallelBounce.data(), weapondef::kSurfaceTypes);
                if (wd.hasPerpendicularBounce) floats(wd.perpendicularBounce.data(), weapondef::kSurfaceTypes);
                fx(o::dProjTrailEffect);
                fx(o::dProjBeaconEffect);
                fx(o::dProjIgnitionEffect);
                snd(o::dProjIgnitionSound);
                str(wd.defStrings, o::dAccuracyGraphName0);  // knots: null
                str(wd.defStrings, o::dAccuracyGraphName1);
                str(wd.defStrings, o::dUseHint);
                str(wd.defStrings, o::dDropHint);
                str(wd.defStrings, o::dScript);
                if (wd.hasLocationDamage) floats(wd.locationDamage.data(), weapondef::kHitLocs);
                str(wd.defStrings, o::dFireRumble);
                str(wd.defStrings, o::dMeleeImpactRumble);
                // tracer: null
                snd(o::dTurretOverheatSound);
                fx(o::dTurretOverheatEffect);
                str(wd.defStrings, o::dTurretBarrelSpinRumble);
                snd(o::dTurretBarrelSpinMaxSnd);
                for (int k = 0; k < 4; k++) snd(static_cast<uint16_t>(o::dTurretBarrelSpinUpSnd + 8 * k));
                for (int k = 0; k < 4; k++) snd(static_cast<uint16_t>(o::dTurretBarrelSpinDownSnd + 8 * k));
                snd(o::dMissileConeSoundAlias);
                snd(o::dMissileConeSoundAliasAtBase);
                // ---- rest of WeaponCompleteDef ----
                str(wd.completeStrings, o::cDisplayName);
                {
                    w.align(kAlign2);
                    std::vector<uint16_t> t(32, 0);
                    std::copy(hideTags.begin(), hideTags.begin() + std::min<size_t>(32, hideTags.size()), t.begin());
                    w.write(t.data(), 64);
                }
                writeStringArray(w, wd.anims);
                str(wd.completeStrings, o::cAltWeapon);
                mat(wd.completeMaterials, o::cKillIcon);
                mat(wd.completeMaterials, o::cDpadIcon);
                w.pop();
            }};
}

// Load_XAnimParts (0x1229e0), in its order (struct 136, VIRTUAL): name; names (script string
// indices, align 2); notify / deltaPart null; dataByte (a1); dataShort (a2); dataInt (a4);
// randomDataShort (a2); randomDataByte (a1); randomDataInt / indices null (numframes < 256).
ZoneAsset buildXAnimParts(const animbuild::Anim& an, std::vector<uint16_t> nameIndex, std::vector<uint16_t> noteIndex) {
    return {IW4_XANIMPARTS, [an, nameIndex, noteIndex](ZoneWriter& w) {
                using namespace zone;
                iw4::XAnimParts a{};
                a.name = follow<const char>();
                a.dataByteCount = static_cast<unsigned short>(an.dataByte.size());
                a.dataShortCount = static_cast<unsigned short>(an.dataShort.size());
                a.dataIntCount = static_cast<unsigned short>(an.dataInt.size());
                a.randomDataByteCount = static_cast<unsigned short>(an.randomDataByte.size());
                a.numframes = an.numframes;
                a.flags = static_cast<char>(an.flags);
                std::memcpy(a.boneCount, an.boneCount, 10);
                a.randomDataShortCount = static_cast<unsigned int>(an.randomDataShort.size());
                a.framerate = an.framerate;
                a.frequency = an.frequency;
                a.names = an.names.empty() ? nullptr : follow<unsigned short>();
                a.notifyCount = static_cast<unsigned char>(noteIndex.size());
                a.notify = noteIndex.empty() ? nullptr : follow<iw4::XAnimNotifyInfo>();
                a.dataByte = an.dataByte.empty() ? nullptr : follow<char>();
                a.dataShort = an.dataShort.empty() ? nullptr : follow<short>();
                a.dataInt = an.dataInt.empty() ? nullptr : follow<int>();
                a.randomDataShort = an.randomDataShort.empty() ? nullptr : follow<short>();
                a.randomDataByte = an.randomDataByte.empty() ? nullptr : follow<char>();
                w.writeStruct(a);
                w.push(BLOCK_VIRTUAL);
                w.xstring(an.name);
                if (!an.names.empty()) {
                    w.align(kAlign2);
                    w.write(nameIndex.data(), 2 * nameIndex.size());
                }
                if (!noteIndex.empty()) {
                    w.align(kAlign4);
                    for (size_t i = 0; i < noteIndex.size(); i++) {
                        iw4::XAnimNotifyInfo ni{};
                        ni.name = noteIndex[i];
                        ni.time = an.notes[i].second;
                        w.writeStruct(ni);
                    }
                }
                if (!an.dataByte.empty()) {
                    w.align(kAlign1);
                    w.write(an.dataByte.data(), an.dataByte.size());
                }
                if (!an.dataShort.empty()) {
                    w.align(kAlign2);
                    w.write(an.dataShort.data(), 2 * an.dataShort.size());
                }
                if (!an.dataInt.empty()) {
                    w.align(kAlign4);
                    w.write(an.dataInt.data(), 4 * an.dataInt.size());
                }
                if (!an.randomDataShort.empty()) {
                    w.align(kAlign2);
                    w.write(an.randomDataShort.data(), 2 * an.randomDataShort.size());
                }
                if (!an.randomDataByte.empty()) {
                    w.align(kAlign1);
                    w.write(an.randomDataByte.data(), an.randomDataByte.size());
                }
                w.pop();
            }};
}

// Material plans for a model: stock mc/<name> if the library has it, else a custom material on a
// stock mc_l_sm_b0c0 template with the texture from zone_source/images (embedded) or the IWDs.
std::vector<ModelMaterialPlan> planModelMaterials(const modelbuild::Model& md, const matdef::Library* lib,
                                                  const std::string& zoneSourceDir, std::vector<std::string>* warnings) {
    namespace fs = std::filesystem;
    std::vector<ModelMaterialPlan> plans;
    const matdef::Def* tmpl = nullptr;
    if (lib)
        for (const auto& [k, d] : *lib)
            if (k.rfind("mc/", 0) == 0 && matdef::techsetFor(d) == "mc_l_sm_b0c0" && d.header.size() > 9 && d.header[9] == 1) {
                tmpl = &d;
                break;
            }
    std::vector<bool> used(md.materials.size(), false);
    for (const modelbuild::Surface& sf : md.surfaces) used[sf.material] = true;
    for (size_t mi = 0; mi < md.materials.size(); mi++) {
        const modelbuild::Material& mm = md.materials[mi];
        ModelMaterialPlan p;
        if (!used[mi]) {  // only on hidden geometry: never referenced
            plans.push_back(std::move(p));
            continue;
        }
        std::string key = matdef::key(mm.name.find('/') == std::string::npos ? "mc/" + mm.name : mm.name);
        auto it = lib ? lib->find(key) : lib->end();
        if (lib && it != lib->end() && matdef::shadingFor(it->second) == matdef::Shading::Lit) {
            p.def = it->second;  // the stock model material of that name
            plans.push_back(std::move(p));
            continue;
        }
        if (!tmpl) {
            if (warnings) warnings->push_back(md.name + ": no material library with model materials yet; '" + mm.name +
                                              "' will be the default material");
            p.def.name = "mc/" + mm.name;
            plans.push_back(std::move(p));
            continue;
        }
        p.def = *tmpl;
        p.def.name = "mc/" + md.name + "_" + mm.name;
        p.def.textures.clear();
        p.def.constants = tmpl->constants;
        const matdef::Texture* color = nullptr;
        for (const matdef::Texture& t : tmpl->textures)
            if (t.kind() == 'c') color = &t;
        matdef::Texture tex = *color;
        std::string stem = fs::path(mm.texture).stem().string();
        if (stem.empty()) stem = mm.name;
        tex.image = stem;
        // A texture file is embedded when found as (in order) zone_source/images/<exported name |
        // stem.png | stem.tga>, the same without a t5_/t6_/t7_ prefix, or next to the models in
        // zone_source/xmodels; otherwise the image streams from the IWDs by that name.
        fs::path imgDir = fs::path(zoneSourceDir) / "images", mdlDir = fs::path(zoneSourceDir) / "xmodels";
        std::string bare = stem;
        for (const char* pre : {"t5_", "t6_", "t7_", "T5_", "T6_", "T7_"})
            if (bare.rfind(pre, 0) == 0) bare = bare.substr(3);
        std::string found;
        std::vector<fs::path> cands;
        for (const fs::path& dir : {imgDir, mdlDir})
            for (const std::string& st : {stem, bare}) {
                if (!mm.texture.empty() && st == stem) cands.push_back(dir / fs::path(mm.texture).filename());
                for (const char* ext : {".png", ".tga", ".PNG", ".TGA"}) cands.push_back(dir / (st + ext));
            }
        for (const fs::path& cand : cands) {
            std::error_code fec;
            if (!cand.filename().empty() && fs::is_regular_file(cand, fec)) {
                found = cand.string();
                break;
            }
        }
        if (!found.empty()) {
            imageio::Rgba rgba;
            std::string err;
            if (imageio::load(found, rgba, err)) {
                imageio::toPowerOfTwo(rgba);
                p.embedded = true;
                p.image.name = "iw4r_" + md.name + "_" + stem;
                tex.image = p.image.name;
                p.image.mapType = 3;
                p.image.semantic = 2;
                p.image.category = 2;
                p.image.width = static_cast<uint16_t>(rgba.width);
                p.image.height = static_cast<uint16_t>(rgba.height);
                p.image.levelCount = 1;
                p.image.loadFlags = 2;
                p.image.format = 0x15;  // A8R8G8B8
                int levels = 0;
                std::vector<uint8_t> chain = imageio::bgraMipChain(rgba, levels);
                p.image.pixels.assign(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(4ull * rgba.width * rgba.height));
            } else if (warnings) {
                warnings->push_back(md.name + ": " + err);
            }
        } else if (warnings && !mm.texture.empty()) {
            warnings->push_back(md.name + ": texture '" + stem + "' not in zone_source/images; streaming it by name from the game's IWDs");
        }
        p.def.textures.push_back(std::move(tex));
        plans.push_back(std::move(p));
    }
    return plans;
}

// The editor (like .map files) names world materials without their "wc/" asset prefix.
std::string worldMaterialAssetName(const std::string& n) { return n.find('/') == std::string::npos ? "wc/" + n : n; }

// Load_MaterialHandle (0x11d120) -> Load_Material (0x11cf00) for a by-name reference: the name
// starts with ',' so DB_AddXAsset (0x12e700) resolves it to the already-loaded material (or a
// default one) instead of adding it. Only MaterialInfo.name is set; every other pointer is null,
// which Load_Material checks before loading.
uint64_t writeMaterialRef(ZoneWriter& w, const std::string& name) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        iw4::Material m{};
        m.info.name = follow<const char>();
        ww.writeStruct(m);
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring("," + name);
        ww.pop();
    });
}

// Load_MapEnts (0x11cb80, SP 0xe0590 identical): struct 0x58; push VIRTUAL; name (XString);
// entityString (byte-aligned, numEntityChars bytes); MapTriggers inline (all null here); stages
// (align 4, 24 bytes each, then each stage's name as an XString); pop.
// withDefaultStage: one stage 0 (whole map, sun = primary light 1). SP needs it: its per-frame
// stage lookup (iw4sp+0x46c20) returns 0 when the player is in no stage trigger and the caller
// (iw4sp+0x85f8) then reads stages[0] with no null check. MP zones keep stages null.
iw4::Bounds toGame(const clip::Bounds& b);
void writeMapEnts(ZoneWriter& w, const std::string& name, const std::string& entityString,
                  bool withDefaultStage = false, const clip::ClipMapData* trig = nullptr) {
    iw4::MapEnts m{};
    m.name = follow<const char>();
    m.entityString = follow<char>();
    m.numEntityChars = static_cast<int>(entityString.size() + 1);  // incl. NUL
    const bool triggers = trig && !trig->triggerModels.empty();
    if (triggers) {  // brush triggers: entity "model" "?N" (clip::buildTriggers)
        m.trigger.count = static_cast<unsigned>(trig->triggerModels.size());
        m.trigger.models = follow<iw4::TriggerModel>();
        m.trigger.hullCount = static_cast<unsigned>(trig->triggerHulls.size());
        m.trigger.hulls = trig->triggerHulls.empty() ? nullptr : follow<iw4::TriggerHull>();
        m.trigger.slabCount = static_cast<unsigned>(trig->triggerSlabs.size());
        m.trigger.slabs = trig->triggerSlabs.empty() ? nullptr : follow<iw4::TriggerSlab>();
    }
    if (withDefaultStage) {
        m.stages = follow<iw4::Stage>();
        m.stageCount = 1;
    }
    w.writeStruct(m);
    w.push(zone::BLOCK_VIRTUAL);
    w.xstring(name);
    w.align(zone::kAlign1);
    w.write(entityString.c_str(), entityString.size() + 1);
    if (triggers) {  // Load_MapTriggers: models (8), hulls (32), slabs (20), each align 4
        w.align(zone::kAlign4);
        for (const clip::TriggerModel& t : trig->triggerModels) {
            iw4::TriggerModel g{};
            g.contents = static_cast<int>(t.contents);
            g.hullCount = t.hullCount;
            g.firstHull = t.firstHull;
            w.writeStruct(g);
        }
        if (!trig->triggerHulls.empty()) {
            w.align(zone::kAlign4);
            for (const clip::TriggerHull& h : trig->triggerHulls) {
                iw4::TriggerHull g{};
                g.bounds = toGame(h.bounds);
                g.contents = static_cast<int>(h.contents);
                g.slabCount = h.slabCount;
                g.firstSlab = h.firstSlab;
                w.writeStruct(g);
            }
        }
        if (!trig->triggerSlabs.empty()) {
            w.align(zone::kAlign4);
            for (const clip::TriggerSlab& sl : trig->triggerSlabs) {
                iw4::TriggerSlab g{};
                std::memcpy(g.dir, sl.dir, sizeof g.dir);
                g.midPoint = sl.midPoint;
                g.halfSize = sl.halfSize;
                w.writeStruct(g);
            }
        }
    }
    if (withDefaultStage) {
        w.align(zone::kAlign4);
        iw4::Stage st{};
        st.name = follow<const char>();
        st.sunPrimaryLightIndex = 1;  // ComWorld/GfxWorld primary light 1 is the sun
        w.writeStruct(st);
        w.xstring("default");
    }
    w.pop();
}
static_assert(sizeof(iw4::TriggerModel) == 8 && sizeof(iw4::TriggerHull) == 32 && sizeof(iw4::TriggerSlab) == 20 &&
                  offsetof(iw4::MapEnts, trigger) == 0x18,
              "MapTriggers layout differs from Load_MapTriggers");
static_assert(sizeof(iw4::Stage) == 24 && offsetof(iw4::MapEnts, stages) == 0x48 &&
                  offsetof(iw4::MapEnts, stageCount) == 0x50,
              "Stage/MapEnts layout differs from Load_MapEnts");

iw4::Bounds toGame(const clip::Bounds& b) {
    iw4::Bounds r{};
    std::memcpy(r.midPoint, b.midPoint, sizeof(r.midPoint));
    std::memcpy(r.halfSize, b.halfSize, sizeof(r.halfSize));
    return r;
}

iw4::cLeaf_t toGame(const clip::Leaf& l) {
    iw4::cLeaf_t r{};
    r.firstCollAabbIndex = l.firstCollAabbIndex;
    r.collAabbCount = l.collAabbCount;
    r.brushContents = static_cast<int>(l.brushContents);
    r.terrainContents = static_cast<int>(l.terrainContents);
    r.bounds = toGame(l.bounds);
    r.leafBrushNode = static_cast<int>(l.leafBrushNode);
    return r;
}
}  // namespace

std::string documentToEntityString(const Document& doc) {
    std::string s;
    for (const Entity& e : doc.ents) {
        if (e.kv.empty()) continue;
        s += "{\n";
        for (const KeyValue& kv : e.kv) s += "\"" + kv.key + "\" \"" + kv.value + "\"\n";
        s += "}\n";
    }
    return s;
}

// Load_AddonMapEnts: struct 0x48 (name, entityString, numEntityChars, MapTriggers - MapEnts
// without stages, as in stock so_* add-on zones); push VIRTUAL; name; entity string; pop.
ZoneAsset buildAddonMapEnts(const std::string& name, const std::string& entityString) {
    return {IW4_ADDON_MAP_ENTS, [name, entityString](ZoneWriter& w) {
                iw4::AddonMapEnts m{};
                m.name = follow<const char>();
                m.entityString = follow<char>();
                m.numEntityChars = static_cast<int>(entityString.size() + 1);
                w.writeStruct(m);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                w.align(zone::kAlign1);
                w.write(entityString.c_str(), entityString.size() + 1);
                w.pop();
            }};
}
static_assert(sizeof(iw4::AddonMapEnts) == 0x48, "AddonMapEnts (stock so_* zones: 0x48)");

const std::vector<WeaponPack>& weaponPacks() {
    static const std::vector<WeaponPack> packs = {
        {"so_demo_so_bridge", "m4_grenadier, ak47, ak47_grenadier, scar_h, scar_h_reflex, m16_grenadier, at4, m79, claymore, c4"},
        {"so_crossing_so_bridge", "scar_h, scar_h_reflex, deserteagle, tavor, m79, claymore, c4"},
        {"so_escape_airport", "deserteagle, rpg, m240, m240_heartbeat_reflex, claymore, c4"},
        {"so_hidden_so_ghillies", "m1014, mp5_silencer, striker, uzi, claymore, c4"},
        {"so_intel_boneyard", "m4m203_reflex, m1014, m240, cheytac"},
        {"so_takeover_oilrig", "m240, m240_reflex, barrett, striker"},
        {"so_juggernauts_favela", "m240, m240_reflex, m79, claymore, c4"},
        {"so_defuse_favela_escape", "m1014, m240, glock, p90"},
        {"so_chopper_invasion", "deserteagle, mp5, m240, c4"},
    };
    return packs;
}

std::string weaponPackLevel(const std::string& mapName) { return "so_w_" + mapName; }

bool deployWeaponPack(const std::string& gameDir, const std::string& deployDir, const std::string& pack,
                      const std::string& mapName, std::string& error) {
    namespace fs = std::filesystem;
    const fs::path src = fs::path(gameDir) / "zone" / "english" / (pack + ".ff");
    const fs::path dst = fs::path(deployDir) / (weaponPackLevel(mapName) + ".ff");
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        error = "weapon pack zone not found: " + src.string();
        return false;
    }
    std::vector<uint8_t> file;
    {
        std::ifstream in(src, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    Fastfile ff;
    if (!readFastfile(file, ff, error)) {
        error = "can't read the weapon pack " + src.string() + ": " + error;
        return false;
    }
    if (!retargetAddonMapEnts(ff.zone, "maps/" + pack + ".mapents", "maps/" + weaponPackLevel(mapName) + ".mapents", error))
        return false;
    std::vector<uint8_t> out;
    if (!writeFastfile(ff, out, error)) return false;
    std::ofstream o(dst, std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!o) {
        error = "can't write the weapon pack to " + dst.string();
        return false;
    }
    return true;
}

// The add-on lump is AddonMapEnts (0x48: name -1, entityString -1, numEntityChars, triggers)
// followed by the name and numEntityChars bytes of entity string, both align 1. Rewriting name +
// string in the same number of bytes keeps every later offset in the zone valid; the stock
// mission entities are dropped (blank padding), the trigger data after them is kept.
bool retargetAddonMapEnts(std::vector<uint8_t>& zone, const std::string& oldName, const std::string& newName,
                          std::string& error) {
    const std::string key = oldName + std::string(1, '\0');
    auto it = std::search(zone.begin(), zone.end(), key.begin(), key.end());
    const size_t at = static_cast<size_t>(it - zone.begin());
    const size_t head = 0x48;
    if (it == zone.end() || at < head) {
        error = "weapon pack has no add-on entity lump " + oldName;
        return false;
    }
    uint64_t namePtr, entPtr;
    int32_t chars;
    std::memcpy(&namePtr, &zone[at - head], 8);
    std::memcpy(&entPtr, &zone[at - head + 8], 8);
    std::memcpy(&chars, &zone[at - head + 16], 4);
    const size_t total = key.size() + static_cast<size_t>(chars);
    if (namePtr != ~0ull || entPtr != ~0ull || chars < 1 || at + total > zone.size() || newName.size() + 2 > total) {
        error = "weapon pack's add-on entity lump " + oldName + " isn't laid out as expected";
        return false;
    }
    uint8_t* p = &zone[at];
    std::memcpy(p, newName.c_str(), newName.size() + 1);
    const size_t n = total - (newName.size() + 1);
    std::memset(p + newName.size() + 1, '\n', n - 1);
    p[total - 1] = 0;
    const int32_t newChars = static_cast<int32_t>(n);
    std::memcpy(&zone[at - head + 16], &newChars, 4);
    return true;
}

ZoneAsset buildMapEnts(const std::string& name, const std::string& entityString) {
    return {IW4_MAPENTS, [name, entityString](ZoneWriter& w) { writeMapEnts(w, name, entityString); }};
}

// Load_clipMap_t (0x1242b0). Field order below is the loader's, which is NOT struct order
// (leafbrushes before leafbrushNodes, smodelNodes before mapEnts). Element pointers inside the
// arrays are back-references into the arrays already written (sides -> planes, nodes -> planes,
// brushes -> brushsides/brushEdges, leaf-brush nodes -> leafbrushes); a -1 there would load a
// single element inline instead. Every array is under push(VIRTUAL) except the dynEnt
// pose/client/coll lists (RUNTIME, none emitted). Validated against stock boneyard: walking its
// clipMap in this order lands exactly on its inline MapEnts.
ZoneAsset buildClipMap(const std::string& name, const clip::ClipMapData& cm,
                       const std::string& entityString, std::shared_ptr<SharedWorld> shared,
                       bool withDefaultStage) {
    return {IW4_CLIPMAP_MP, [name, cm, entityString, shared, withDefaultStage](ZoneWriter& w) {
                using namespace zone;
                const size_t nb = cm.brushes.size();
                // Stock clipMaps back-reference GfxWorld's plane array instead of their own copy.
                const bool sharedPlanes = shared && shared->havePlanes && !cm.planes.empty();
                iw4::clipMap_t c{};
                c.name = follow<const char>();
                c.planeCount = static_cast<unsigned>(cm.planes.size());
                c.planes = cm.planes.empty() ? nullptr
                           : sharedPlanes    ? ref<iw4::cplane_s>(virtualRef(shared->planesAt, 0, 20))
                                             : follow<iw4::cplane_s>();
                c.numMaterials = static_cast<unsigned>(cm.materials.size());
                c.materials = cm.materials.empty() ? nullptr : follow<iw4::ClipMaterial>();
                c.numBrushSides = static_cast<unsigned>(cm.sides.size());
                c.brushsides = cm.sides.empty() ? nullptr : follow<iw4::cbrushside_t>();
                c.numBrushEdges = static_cast<unsigned>(cm.brushEdges.size());
                c.brushEdges = cm.brushEdges.empty() ? nullptr : follow<unsigned char>();
                c.numNodes = static_cast<unsigned>(cm.nodes.size());
                c.nodes = cm.nodes.empty() ? nullptr : follow<iw4::cNode_t>();
                c.numLeafs = static_cast<unsigned>(cm.leafs.size());
                c.leafs = cm.leafs.empty() ? nullptr : follow<iw4::cLeaf_t>();
                c.leafbrushNodesCount = static_cast<unsigned>(cm.leafBrushNodes.size());
                c.leafbrushNodes = cm.leafBrushNodes.empty() ? nullptr : follow<iw4::cLeafBrushNode_s>();
                c.numLeafBrushes = static_cast<unsigned>(cm.leafBrushes.size());
                c.leafbrushes = cm.leafBrushes.empty() ? nullptr : follow<unsigned short>();
                c.numSubModels = static_cast<unsigned>(cm.cmodels.size());
                c.cmodels = cm.cmodels.empty() ? nullptr : follow<iw4::cmodel_t>();
                c.numBrushes = static_cast<unsigned short>(nb);
                c.brushes = nb ? follow<iw4::cbrush_t>() : nullptr;
                c.brushBounds = nb ? follow<iw4::Bounds>() : nullptr;
                c.brushContents = nb ? follow<int>() : nullptr;
                c.mapEnts = ref<iw4::MapEnts>(kPtrInsert);  // inline + added as its own asset
                // One empty static-model AABB node, so a walk from node 0 finds nothing to test.
                c.smodelNodeCount = 1;
                c.smodelNodes = follow<iw4::SModelAabbNode>();
                c.checksum = cm.checksum;
                w.writeStruct(c);

                w.push(BLOCK_VIRTUAL);
                w.xstring(name);

                uint32_t planesAt = sharedPlanes ? shared->planesAt : 0;
                if (!cm.planes.empty() && !sharedPlanes) planesAt = writePlanes(w, cm.planes);
                if (!cm.materials.empty()) {
                    w.align(kAlign4);
                    for (const clip::Material& m : cm.materials) {
                        iw4::ClipMaterial g{};
                        g.name = follow<const char>();
                        g.surfaceFlags = static_cast<int>(m.surfaceFlags);
                        g.contents = static_cast<int>(m.contents);
                        w.writeStruct(g);
                    }
                    for (const clip::Material& m : cm.materials) w.xstring(m.name);
                }
                uint32_t sidesAt = 0;
                if (!cm.sides.empty()) {
                    sidesAt = w.align(kAlign4);
                    for (const clip::Side& s : cm.sides) {
                        iw4::cbrushside_t g{};
                        g.plane = ref<iw4::cplane_s>(virtualRef(planesAt, s.plane, sizeof(iw4::cplane_s)));
                        g.materialNum = s.material;
                        g.firstAdjacentSideOffset = static_cast<char>(s.firstAdjacentSideOffset);
                        g.edgeCount = static_cast<char>(s.edgeCount);
                        w.writeStruct(g);
                    }
                }
                uint32_t edgesAt = 0;
                if (!cm.brushEdges.empty()) {
                    edgesAt = w.align(kAlign1);
                    w.write(cm.brushEdges.data(), cm.brushEdges.size());
                }
                if (!cm.nodes.empty()) {
                    w.align(kAlign4);
                    for (const clip::Node& n : cm.nodes) {
                        iw4::cNode_t g{};
                        g.plane = ref<iw4::cplane_s>(virtualRef(planesAt, n.plane, sizeof(iw4::cplane_s)));
                        g.children[0] = n.children[0];
                        g.children[1] = n.children[1];
                        w.writeStruct(g);
                    }
                }
                if (!cm.leafs.empty()) {
                    w.align(kAlign4);
                    for (const clip::Leaf& l : cm.leafs) w.writeStruct(toGame(l));
                }
                uint32_t leafBrushesAt = 0;
                if (!cm.leafBrushes.empty()) {
                    leafBrushesAt = w.align(kAlign2);
                    w.write(cm.leafBrushes.data(), cm.leafBrushes.size() * sizeof(uint16_t));
                }
                if (!cm.leafBrushNodes.empty()) {
                    w.align(kAlign4);
                    for (const clip::LeafBrushNode& n : cm.leafBrushNodes) {
                        iw4::cLeafBrushNode_s g{};
                        g.axis = n.axis;
                        g.leafBrushCount = n.leafBrushCount;
                        g.contents = static_cast<int>(n.contents);
                        if (n.leafBrushCount > 0) {
                            g.data.leaf.brushes = ref<unsigned short>(
                                virtualRef(leafBrushesAt, n.firstLeafBrush, sizeof(uint16_t)));
                        } else {  // interior split node
                            g.data.children.dist = n.dist;
                            g.data.children.range = n.range;
                            g.data.children.childOffset[0] = n.childOffset[0];
                            g.data.children.childOffset[1] = n.childOffset[1];
                        }
                        w.writeStruct(g);
                    }
                }
                // leafsurfaces, verts, triIndices, triEdgeIsWalkable, borders, partitions,
                // aabbTrees: none until patches/terrain are compiled.
                if (!cm.cmodels.empty()) {
                    w.align(kAlign4);
                    for (const clip::CModel& m : cm.cmodels) {
                        iw4::cmodel_t g{};
                        g.bounds = toGame(m.bounds);
                        g.radius = m.radius;
                        g.leaf = toGame(m.leaf);
                        w.writeStruct(g);
                    }
                }
                if (nb) {
                    w.align(127);  // brushes and brushBounds are 128-byte aligned
                    for (const clip::Brush& b : cm.brushes) {
                        iw4::cbrush_t g{};
                        g.numsides = b.numSides;
                        if (b.numSides)
                            g.sides = ref<iw4::cbrushside_t>(virtualRef(sidesAt, b.firstSide, sizeof(iw4::cbrushside_t)));
                        if (b.edgeBytes) g.baseAdjacentSide = ref<unsigned char>(virtualRef(edgesAt, b.firstEdge, 1));
                        std::memcpy(g.axialMaterialNum, b.axialMaterial, sizeof(g.axialMaterialNum));
                        std::memcpy(g.firstAdjacentSideOffsets, b.firstAdjacentSideOffsets,
                                    sizeof(g.firstAdjacentSideOffsets));
                        std::memcpy(g.edgeCount, b.edgeCount, sizeof(g.edgeCount));
                        w.writeStruct(g);
                    }
                    w.align(127);
                    for (const clip::Brush& b : cm.brushes) w.writeStruct(toGame(b.bounds));
                    w.align(kAlign4);
                    for (const clip::Brush& b : cm.brushes) w.i32(static_cast<int>(b.contents));
                }
                w.align(kAlign4);
                w.writeStruct(iw4::SModelAabbNode{});
                // Load_MapEntsPtr (0x11cd50), nested: the entity lump becomes its own asset.
                loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) { writeMapEnts(ww, name, entityString, withDefaultStage, &cm); });
                // dynEnt def/pose/client/coll lists: none.
                w.pop();
            }};
}

// Load_ComWorld (0x1188a0): struct 0x18; push VIRTUAL; name; primaryLights (null); pop.
ZoneAsset buildComWorld(const std::string& name, bool withSun, const light::Environment* env,
                        const std::vector<gfx::PrimaryLight>& primaries) {
    // Sun primary light: from the map's worldspawn (via the bake) or a fixed default.
    Vec3 sunDir = normalize(Vec3{-0.4, 0.3, 0.86}), sunColor{1, 1, 1};
    if (env && env->hasSun) {
        sunDir = env->sunDir;
        sunColor = env->sunColor;
    }
    return {IW4_COMWORLD, [name, withSun, sunDir, sunColor, primaries](ZoneWriter& w) {
                iw4::ComWorld c{};
                c.name = follow<const char>();
                c.isInUse = 1;
                // Primary light 0 is the empty slot; light 1 is the sun (lastSunPrimaryLightIndex
                // 1 in GfxWorld). The count must match GfxWorld.primaryLightCount.
                c.primaryLightCount = withSun ? 2 + static_cast<unsigned>(primaries.size()) : 0;
                c.primaryLights = withSun ? follow<iw4::ComPrimaryLight>() : nullptr;
                w.writeStruct(c);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                if (withSun) {
                    w.align(zone::kAlign4);
                    iw4::ComPrimaryLight none{}, sun{};
                    sun.type = 1;  // GFX_LIGHT_TYPE_DIR
                    for (int k = 0; k < 3; k++) {
                        sun.color[k] = static_cast<float>(sunColor[k]);  // linear
                        sun.dir[k] = static_cast<float>(sunDir[k]);      // towards the sun
                    }
                    w.writeStruct(none);
                    w.writeStruct(sun);  // defName (+64) null in both
                    // Primary spotlights (stock values: shadow-mapped, exponent 1, rotation limit 1,
                    // the expanded cone = the outer one, light def "light_point_linear" for the falloff).
                    for (const gfx::PrimaryLight& p : primaries) {
                        iw4::ComPrimaryLight l{};
                        l.type = 2;  // GFX_LIGHT_TYPE_SPOT
                        l.canUseShadowMap = 1;
                        l.exponent = 1;
                        for (int k = 0; k < 3; k++) l.color[k] = p.color[k], l.dir[k] = p.dir[k], l.origin[k] = p.origin[k];
                        l.radius = p.radius;
                        l.cosHalfFovOuter = p.cosOuter;
                        l.cosHalfFovInner = p.cosInner;
                        l.cosHalfFovExpanded = p.cosOuter;
                        l.rotationLimit = 1;
                        l.translationLimit = 0;
                        l.defName = follow<const char>();
                        w.writeStruct(l);
                    }
                    for (size_t i = 0; i < primaries.size(); i++) w.xstring("light_point_linear");
                }
                w.pop();
            }};
}

// Load_GfxWorld (0x11ae60), in its exact order; see gfxworld.h for the data conventions. All
// world arrays are VIRTUAL; the listed RUNTIME arrays are zero-filled by the loader (pointer -1,
// no file bytes). Materials are loaded once each as ','-name references through materialMemory
// (which loads before dpvs), and every surface back-references the material's insert slot.
// GfxWorld owns the plane array; clipMap back-references it (stock layout).
ZoneAsset buildGfxWorld(const std::string& name, const std::string& baseName, const gfx::GfxWorldData& gw,
                        const clip::ClipMapData& cm, std::shared_ptr<SharedWorld> shared,
                        std::shared_ptr<const sky::Source> skySrc, std::shared_ptr<const matdef::Library> materials) {
    return {IW4_GFXWORLD, [name, baseName, gw, planes = cm.planes, checksum = cm.checksum, shared, skySrc,
                           materials](ZoneWriter& w) {
                using namespace zone;
                const auto rt = follow<unsigned int>();  // RUNTIME arrays: allocated + zeroed
                const uint32_t ns = static_cast<uint32_t>(gw.surfaces.size());
                const uint32_t cellCount = 1, primaryLights = 2 + static_cast<uint32_t>(gw.primaries.size());
                const uint32_t visCount = (ns + 63) >> 5;
                iw4::GfxWorld g{};
                g.name = follow<const char>();
                g.baseName = follow<const char>();
                g.planeCount = static_cast<int>(planes.size());
                g.nodeCount = 1;
                g.surfaceCount = ns;
                const bool hasSky = skySrc && gw.skyMaterial >= 0 && !gw.skySurfaces.empty();
                g.skyCount = hasSky ? 1 : 0;
                g.skies = hasSky ? follow<iw4::GfxSky>() : nullptr;
                g.lastSunPrimaryLightIndex = 1;
                g.primaryLightCount = primaryLights;
                g.sortKeyLitDecal = 6;  // stock values (boneyard)
                g.sortKeyEffectDecal = 39;
                g.sortKeyEffectAuto = 48;
                g.sortKeyDistortion = 43;
                g.dpvsPlanes.cellCount = static_cast<int>(cellCount);
                g.dpvsPlanes.planes = planes.empty() ? nullptr : follow<iw4::cplane_s>();
                g.dpvsPlanes.nodes = follow<unsigned short>();
                g.dpvsPlanes.sceneEntCellBits = rt;
                g.aabbTreeCounts = follow<iw4::GfxCellTreeCount>();
                g.aabbTrees = follow<iw4::GfxCellTree>();
                g.cells = follow<iw4::GfxCell>();
                iw4::GfxWorldDraw& d = g.draw;
                const uint32_t probeCount = static_cast<uint32_t>(gw.reflectionProbes.size());
                d.reflectionProbeCount = probeCount;
                d.reflectionProbes = follow<iw4::GfxImage*>();
                d.reflectionProbeOrigins = follow<iw4::GfxReflectionProbe>();
                d.reflectionProbeTextures = follow<iw4::GfxTexture>();
                const int lmCount = static_cast<int>(gw.lightmapPrimaries.size());
                d.lightmapCount = lmCount;
                d.lightmaps = follow<iw4::GfxLightmapArray>();
                d.lightmapPrimaryTextures = follow<iw4::GfxTexture>();
                d.lightmapSecondaryTextures = follow<iw4::GfxTexture>();
                d.vertexCount = static_cast<unsigned>(gw.vertices.size());
                d.vd.vertices = gw.vertices.empty() ? nullptr : follow<iw4::GfxWorldVertex>();
                d.vertexLayerDataSize = 4;  // stock: 4 bytes of layer data even when unused
                d.vld.data = follow<char>();
                d.indexCount = static_cast<unsigned>(gw.indices.size());
                d.indices = gw.indices.empty() ? nullptr : follow<unsigned short>();
                const gfx::LightGrid& lg = gw.lightGrid;
                g.lightGrid.hasLightRegions = false;
                g.lightGrid.lastSunPrimaryLightIndex = 1;
                std::memcpy(g.lightGrid.mins, lg.mins, sizeof(lg.mins));
                std::memcpy(g.lightGrid.maxs, lg.maxs, sizeof(lg.maxs));
                g.lightGrid.rowAxis = lg.rowAxis;
                g.lightGrid.colAxis = lg.colAxis;
                g.lightGrid.rowDataStart = follow<unsigned short>();
                g.lightGrid.rawRowDataSize = static_cast<unsigned>(lg.rawRowData.size());
                g.lightGrid.rawRowData = follow<char>();
                g.lightGrid.entryCount = lg.entryCount;
                g.lightGrid.entries = follow<iw4::GfxLightGridEntry>();
                g.lightGrid.colorCount = lg.colorCount;
                g.lightGrid.colors = follow<iw4::GfxLightGridColors>();
                g.modelCount = 1;
                g.models = follow<iw4::GfxBrushModel>();
                for (int k = 0; k < 3; k++) {
                    g.bounds.midPoint[k] = (gw.mins[k] + gw.maxs[k]) * 0.5f;
                    g.bounds.halfSize[k] = (gw.maxs[k] - gw.mins[k]) * 0.5f;
                }
                g.checksum = checksum;  // stock: GfxWorld and clipMap checksums match
                g.materialMemoryCount = static_cast<int>(gw.materials.size());
                g.materialMemory = gw.materials.empty() ? nullptr : follow<iw4::MaterialMemory>();
                g.cellCasterBits = rt;
                g.cellHasSunLitSurfsBits = rt;
                g.sceneDynModel = follow<iw4::GfxSceneDynModel>();
                g.sceneDynBrush = follow<iw4::GfxSceneDynBrush>();
                g.primaryLightEntityShadowVis = rt;
                g.primaryLightDynEntShadowVis[0] = g.primaryLightDynEntShadowVis[1] = rt;
                g.nonSunPrimaryLightForModelDynEnt = follow<char>();
                g.shadowGeom = follow<iw4::GfxShadowGeometry>();
                g.lightRegion = follow<iw4::GfxLightRegion>();
                iw4::GfxWorldDpvsStatic& v = g.dpvs;
                v.smodelCount = 0;
                v.staticSurfaceCount = v.staticSurfaceCountNoDecal = ns;
                const uint32_t opaque = std::min(gw.opaqueSurfaceCount, ns);
                v.litOpaqueSurfsBegin = 0;
                v.litOpaqueSurfsEnd = v.litTransSurfsBegin = opaque;  // sorted by material sort key
                v.litTransSurfsEnd = ns;
                v.shadowCasterSurfsBegin = v.shadowCasterSurfsEnd = ns;
                v.emissiveSurfsBegin = v.emissiveSurfsEnd = ns;
                v.smodelVisDataCount = (0 + 63) >> 5;
                v.surfaceVisDataCount = visCount;
                for (int k = 0; k < 3; k++) {
                    v.smodelVisData[k] = follow<unsigned char>();
                    v.surfaceVisData[k] = follow<unsigned char>();
                }
                v.sortedSurfIndex = ns ? follow<unsigned short>() : nullptr;
                v.surfaces = ns ? follow<iw4::GfxSurface>() : nullptr;
                v.surfacesBounds = ns ? follow<iw4::GfxSurfaceBounds>() : nullptr;
                v.surfaceMaterials = follow<iw4::GfxDrawSurf>();
                v.surfaceCastsSunShadow = rt;
                for (int k = 0; k < 2; k++) {
                    g.dpvsDyn.dynEntCellBits[k] = rt;
                    for (int j = 0; j < 3; j++) g.dpvsDyn.dynEntVisData[k][j] = follow<char>();
                }
                g.fogTypesAllowed = 2;  // stock value
                w.writeStruct(g);

                w.push(BLOCK_VIRTUAL);
                w.xstring(name);
                w.xstring(baseName);
                // skies (one): GfxSky, its surface list, then the cube image (inline + slot, which
                // the sky material's texture table back-references later).
                uint64_t skyImageSlot = 0;
                if (hasSky) {
                    w.align(kAlign4);
                    iw4::GfxSky s{};
                    s.skySurfCount = static_cast<int>(gw.skySurfaces.size());
                    s.skyStartSurfs = follow<int>();
                    s.skyImage = reinterpret_cast<iw4::GfxImage*>(static_cast<uintptr_t>(kPtrInsert));
                    s.skySamplerState = static_cast<char>(skySrc->textureDef[6]);  // the sky texture's sampler
                    w.writeStruct(s);
                    w.align(kAlign4);
                    for (uint32_t i : gw.skySurfaces) w.u32(i);
                    skyImageSlot = gw.customSky.pixels.empty() ? writeSkyImage(w, *skySrc, gw.skyImage) : writeImage(w, gw.customSky);
                }
                // dpvsPlanes (0x11ba20): planes, nodes, RUNTIME sceneEntCellBits.
                if (!planes.empty()) {
                    uint32_t at = writePlanes(w, planes);
                    if (shared) {
                        shared->planesAt = at;
                        shared->havePlanes = true;
                    }
                }
                w.align(kAlign2);
                w.write(std::array<uint16_t, 1>{1}.data(), 2);  // one leaf: cell 0
                runtimeArray(w, kAlign4, cellCount << 11);
                // aabbTreeCounts, then per cell its tree array (one leaf tree with every surface).
                w.align(kAlign4);
                w.u32(1);
                w.align(127);
                w.u64(kPtrFollow);  // GfxCellTree.aabbTree
                w.align(kAlign4);
                {
                    iw4::GfxAabbTree t{};
                    for (int k = 0; k < 3; k++) {
                        t.bounds.midPoint[k] = g.bounds.midPoint[k];
                        t.bounds.halfSize[k] = g.bounds.halfSize[k];
                    }
                    t.surfaceCount = t.surfaceCountNoDecal = static_cast<unsigned short>(ns);
                    t.startSurfIndex = 0;
                    t.startSurfIndexNoDecal = static_cast<unsigned short>(ns);  // second half
                    w.writeStruct(t);
                }
                // cells: one, no portals, sees reflection probe 0.
                w.align(kAlign4);
                {
                    iw4::GfxCell c{};
                    c.bounds = g.bounds;
                    c.reflectionProbeCount = static_cast<unsigned char>(probeCount);
                    c.reflectionProbes = follow<char>();
                    w.writeStruct(c);
                    w.align(kAlign1);
                    for (uint32_t i = 0; i < probeCount; i++) w.u8(static_cast<uint8_t>(i));
                }
                // draw (0x11bfd0)
                w.align(kAlign4);
                for (uint32_t i = 0; i < probeCount; i++) w.u64(kPtrInsert);  // reflectionProbes[i] -> images follow
                for (const gfx::Image& im : gw.reflectionProbes) writeImage(w, im);
                w.align(kAlign4);
                for (const auto& o : gw.probeOrigins) w.write(o.data(), 12);  // probe origins
                runtimeArray(w, kAlign4, 8ull * probeCount);                  // probe textures
                w.align(kAlign4);
                for (int i = 0; i < lmCount; i++) w.u64(kPtrInsert), w.u64(kPtrInsert);  // lightmaps[i].primary / .secondary
                for (int i = 0; i < lmCount; i++) {
                    writeImage(w, gw.lightmapPrimaries[i]);
                    writeImage(w, gw.lightmapSecondaries[i]);
                }
                runtimeArray(w, kAlign4, 8ull * lmCount);  // lightmapPrimaryTextures
                runtimeArray(w, kAlign4, 8ull * lmCount);  // lightmapSecondaryTextures
                // lightmapOverridePrimary/Secondary (+64/+72): null. vertices, layer data, indices.
                if (!gw.vertices.empty()) {
                    w.align(kAlign4);
                    for (const gfx::Vertex& vx : gw.vertices) {
                        iw4::GfxWorldVertex o{};
                        std::memcpy(o.xyz, vx.xyz, sizeof(o.xyz));
                        o.binormalSign = vx.binormalSign;
                        o.color.packed = vx.color;
                        std::memcpy(o.texCoord, vx.texCoord, sizeof(o.texCoord));
                        std::memcpy(o.lmapCoord, vx.lmapCoord, sizeof(o.lmapCoord));
                        o.normal.packed = vx.normal;
                        o.tangent.packed = vx.tangent;
                        w.writeStruct(o);
                    }
                }
                w.align(kAlign1);
                w.zeros(4);
                if (!gw.indices.empty()) {
                    w.align(kAlign2);
                    w.write(gw.indices.data(), gw.indices.size() * 2);
                }
                // lightGrid (0x11aaa0): rowDataStart, rawRowData, entries, colors.
                w.align(kAlign2);
                w.write(lg.rowDataStart.data(), lg.rowDataStart.size() * 2);
                w.align(kAlign1);
                w.write(lg.rawRowData.data(), lg.rawRowData.size());
                w.align(kAlign4);
                if (lg.entries.empty())
                    w.zeros(4ull * lg.entryCount);  // every entry: colors 0, no primary light
                else
                    w.write(lg.entries.data(), 4ull * lg.entries.size());
                w.align(kAlign4);
                if (lg.colors.empty()) {
                    std::vector<uint8_t> color(sizeof(iw4::GfxLightGridColors), lg.gray);
                    for (uint32_t i = 0; i < lg.colorCount; i++) w.write(color.data(), color.size());
                } else {
                    static_assert(sizeof(iw4::GfxLightGridColors) == 168, "56 rgb");
                    for (const auto& rgb : lg.colors) w.write(rgb.data(), rgb.size());
                }
                // models: the world brush model covers every static surface.
                w.align(kAlign4);
                {
                    iw4::GfxBrushModel m{};
                    m.bounds = g.bounds;
                    m.radius = std::sqrt(g.bounds.halfSize[0] * g.bounds.halfSize[0] +
                                         g.bounds.halfSize[1] * g.bounds.halfSize[1] +
                                         g.bounds.halfSize[2] * g.bounds.halfSize[2]);
                    m.surfaceCount = m.surfaceCountNoDecal = static_cast<unsigned short>(ns);
                    w.writeStruct(m);
                }
                // materialMemory: each material once, as a by-name reference.
                std::vector<uint64_t> matSlot(gw.materials.size());
                if (!gw.materials.empty()) {
                    w.align(kAlign4);
                    for (size_t i = 0; i < gw.materials.size(); i++) {
                        w.u64(kPtrInsert);
                        w.u64(0);  // memory
                    }
                    std::map<std::string, uint64_t> imageSlots;
                    uint64_t falloffTechset = 0;
                    for (size_t i = 0; i < gw.materials.size(); i++) {
                        const std::string full = worldMaterialAssetName(gw.materials[i]);
                        const matdef::Def* def = nullptr;
                        if (materials) {
                            auto it = materials->find(matdef::key(full));
                            if (it != materials->end()) def = &it->second;
                        }
                        const matdef::Shading shading = def ? matdef::shadingFor(*def) : matdef::Shading::Unsupported;
                        if (hasSky && static_cast<int>(i) == gw.skyMaterial)
                            matSlot[i] = writeSkyMaterial(w, *skySrc, skyImageSlot, gfx::kSkyMaterial);
                        else if (shading == matdef::Shading::Lit)
                            matSlot[i] = writeWorldMaterial(w, *def, imageSlots);
                        else if (shading == matdef::Shading::FalloffAdd)
                            matSlot[i] = writeFalloffMaterial(w, *def, imageSlots, falloffTechset);
                        else
                            matSlot[i] = writeMaterialRef(w, full);
                    }
                }
                // sun (inline, no sprite/flare materials), outdoorImage (null), then RUNTIME bits.
                runtimeArray(w, kAlign4, 4ull * cellCount * ((cellCount + 31) >> 5));  // cellCasterBits
                runtimeArray(w, kAlign4, 4ull * ((cellCount + 31) >> 5));              // cellHasSunLitSurfsBits
                runtimeArray(w, kAlign4, 0);  // sceneDynModel (no dyn ents)
                runtimeArray(w, kAlign4, 0);  // sceneDynBrush
                runtimeArray(w, kAlign4, static_cast<size_t>(primaryLights - 1 - 1) << 15);  // entity shadow vis
                runtimeArray(w, kAlign4, 0);  // primaryLightDynEntShadowVis[0]
                runtimeArray(w, kAlign4, 0);  // primaryLightDynEntShadowVis[1]
                runtimeArray(w, kAlign1, 0);  // nonSunPrimaryLightForModelDynEnt
                // shadowGeom / lightRegion: empty for lights 0 (none) and 1 (sun); the primary
                // spotlights get their shadow casters and one region hull (k-DOP, no extra axes).
                w.align(kAlign4);
                w.zeros(sizeof(iw4::GfxShadowGeometry) * 2);
                for (const gfx::PrimaryLight& p : gw.primaries) {
                    iw4::GfxShadowGeometry sg{};
                    sg.surfaceCount = static_cast<uint16_t>(p.shadowSurfs.size());
                    sg.sortedSurfIndex = p.shadowSurfs.empty() ? nullptr : follow<unsigned short>();
                    w.writeStruct(sg);
                }
                for (const gfx::PrimaryLight& p : gw.primaries) {
                    if (p.shadowSurfs.empty()) continue;
                    w.align(kAlign2);
                    w.write(p.shadowSurfs.data(), 2 * p.shadowSurfs.size());
                }
                w.align(kAlign4);
                w.zeros(sizeof(iw4::GfxLightRegion) * 2);
                for (size_t i = 0; i < gw.primaries.size(); i++) {
                    iw4::GfxLightRegion r{};
                    r.hullCount = 1;
                    r.hulls = follow<iw4::GfxLightRegionHull>();
                    w.writeStruct(r);
                }
                for (const gfx::PrimaryLight& p : gw.primaries) {
                    w.align(kAlign4);
                    iw4::GfxLightRegionHull h{};
                    std::memcpy(h.kdopMidPoint, p.kdopMid, sizeof h.kdopMidPoint);
                    std::memcpy(h.kdopHalfSize, p.kdopHalf, sizeof h.kdopHalfSize);
                    h.axisCount = 0;
                    h.axis = nullptr;
                    w.writeStruct(h);
                }
                // dpvs (0x11bb30)
                for (int k = 0; k < 3; k++) runtimeArray(w, kAlign1, 0);   // smodelVisData
                for (int k = 0; k < 3; k++) runtimeArray(w, kAlign1, ns);  // surfaceVisData
                if (ns) {
                    w.align(kAlign2);
                    for (int half = 0; half < 2; half++)
                        for (uint32_t i = 0; i < ns; i++) w.write(&i, 2);  // sortedSurfIndex
                    w.align(kAlign4);
                    for (const gfx::Surface& s : gw.surfaces) {
                        iw4::GfxSurface o{};
                        o.tris.vertexLayerData = 0;
                        o.tris.firstVertex = s.firstVertex;
                        o.tris.vertexCount = s.vertexCount;
                        o.tris.triCount = s.triCount;
                        o.tris.baseIndex = s.baseIndex;
                        o.material = ref<iw4::Material>(matSlot[s.material]);
                        o.laf.fields.lightmapIndex = (s.sky || s.unlit) ? 31 : s.lightmap;  // 31 = no lightmap
                        o.laf.fields.reflectionProbeIndex = s.probe;
                        o.laf.fields.primaryLightIndex = s.primaryLight;
                        o.laf.fields.flags = 1;  // stock world surfaces
                        w.writeStruct(o);
                    }
                    w.align(kAlign4);
                    for (const gfx::Surface& s : gw.surfaces) {
                        iw4::GfxSurfaceBounds b{};
                        for (int k = 0; k < 3; k++) {
                            b.bounds.midPoint[k] = (s.mins[k] + s.maxs[k]) * 0.5f;
                            b.bounds.halfSize[k] = (s.maxs[k] - s.mins[k]) * 0.5f;
                        }
                        w.writeStruct(b);
                    }
                }
                runtimeArray(w, kAlign8, 8ull * ns);           // surfaceMaterials (game-built sort keys)
                runtimeArray(w, 127, 4ull * visCount);         // surfaceCastsSunShadow
                // dpvsDyn (0x11b760): no dyn ents, all sizes 0.
                for (int k = 0; k < 2; k++) runtimeArray(w, kAlign4, 0);
                for (int k = 0; k < 6; k++) runtimeArray(w, kAlign16, 0);
                w.pop();
            }};
}

// Load_GameWorldMp (0x11a1e0): struct 0x10; push VIRTUAL; name; g_glassData (null); pop.
ZoneAsset buildGameWorldMp(const std::string& name) {
    return {IW4_GAMEWORLD_MP, [name](ZoneWriter& w) {
                iw4::GameWorldMp g{};
                g.name = follow<const char>();
                w.writeStruct(g);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                w.pop();
            }};
}

// Load_GameWorldSp (MP 2013 0x11a360, SP 2.0.14 0xddd70, identical): struct 0x70; push VIRTUAL;
// name; PathData inline at +8 (0x11e110: every array pointer checked, nothing read when null);
// vehicleTrack.segments at +0x58 (null); g_glassData at +0x68; pop.
// g_glassData must NOT be null: SP's glass init (iw4sp+0x16ad40) reads ->pieceCount and memsets
// ->glassPieces unconditionally (MP checks for null). So emit an empty one: Load_G_GlassData
// (SP 0xdda40) = struct 0x90, align 4, then glassPieces (+0) and glassNames (+0x18) only if non-null.
// Path data (Load_PathData MP 0x11e110, written in its order): nodes (align 4, 168 each; then
// per node its links, align 4, 12 each), basenodes in RUNTIME (align 16, 16 each), the chain
// arrays (none), pathVis (align 1), nodeTree (align 4, 24 each; then per leaf its node list,
// align 2; children are back-references into the array). See zone/pathnodes.h for the format.
ZoneAsset buildGameWorldSp(const std::string& name, std::shared_ptr<const pathnodes::Graph> path,
                           std::vector<std::array<uint16_t, 4>> nodeStrings) {
    return {IW4_GAMEWORLD_SP, [name, path, nodeStrings](ZoneWriter& w) {
                iw4::GameWorldSp g{};
                g.name = follow<const char>();
                g.g_glassData = follow<iw4::G_GlassData>();
                const size_t n = path ? path->nodes.size() : 0;
                if (n) {
                    g.path.nodeCount = static_cast<unsigned>(n);
                    g.path.nodes = follow<iw4::pathnode_t>();
                    g.path.basenodes = follow<iw4::pathbasenode_t>();
                    g.path.visBytes = static_cast<int>(path->vis.size());
                    g.path.pathVis = path->vis.empty() ? nullptr : follow<char>();
                    g.path.nodeTreeCount = static_cast<int>(path->tree.size());
                    g.path.nodeTree = follow<iw4::pathnode_tree_t>();
                }
                w.writeStruct(g);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                if (n) {
                    w.align(zone::kAlign4);
                    for (size_t i = 0; i < n; i++) {
                        const pathnodes::Node& s = path->nodes[i];
                        iw4::pathnode_t p{};
                        p.constant.type = static_cast<iw4::nodeType>(s.type);
                        p.constant.spawnflags = s.spawnflags;
                        p.constant.targetname = nodeStrings[i][0];
                        p.constant.script_linkName = nodeStrings[i][1];
                        p.constant.script_noteworthy = nodeStrings[i][2];
                        p.constant.target = nodeStrings[i][3];
                        for (int k = 0; k < 3; k++) p.constant.vOrigin[k] = s.origin[k];
                        p.constant.fAngle = s.angle;
                        const double yaw = s.angle * 3.14159265358979323846 / 180;
                        p.constant.forward[0] = static_cast<float>(std::cos(yaw));
                        p.constant.forward[1] = static_cast<float>(std::sin(yaw));
                        p.constant.wOverlapNode[0] = p.constant.wOverlapNode[1] = -1;
                        p.constant.totalLinkCount = static_cast<uint16_t>(path->links[i].size());
                        p.constant.Links = path->links[i].empty() ? nullptr : follow<iw4::pathlink_s>();
                        w.writeStruct(p);
                    }
                    for (size_t i = 0; i < n; i++) {
                        if (path->links[i].empty()) continue;
                        w.align(zone::kAlign4);
                        for (const auto& [to, dist] : path->links[i]) {
                            iw4::pathlink_s l{};
                            l.fDist = dist;
                            l.nodeNum = to;
                            w.writeStruct(l);
                        }
                    }
                    w.push(zone::BLOCK_RUNTIME);
                    w.align(zone::kAlign16);
                    w.zeros(16 * n);
                    w.pop();
                    if (!path->vis.empty()) {
                        w.align(zone::kAlign1);
                        w.write(path->vis.data(), path->vis.size());
                    }
                    w.align(zone::kAlign4);
                    const uint32_t treeAt = w.pos();
                    for (const pathnodes::TreeNode& t : path->tree) {
                        iw4::pathnode_tree_t e{};
                        e.axis = t.axis;
                        e.dist = t.dist;
                        if (t.axis < 0) {
                            e.u.s.nodeCount = static_cast<int>(t.nodes.size());
                            e.u.s.nodes = t.nodes.empty() ? nullptr : follow<unsigned short>();
                        } else {
                            for (int c = 0; c < 2; c++)
                                e.u.child[c] = reinterpret_cast<iw4::pathnode_tree_t*>(
                                    zone::offsetPointer(zone::BLOCK_VIRTUAL, treeAt + 24u * static_cast<uint32_t>(t.child[c])));
                        }
                        w.writeStruct(e);
                    }
                    for (const pathnodes::TreeNode& t : path->tree) {
                        if (t.axis >= 0 || t.nodes.empty()) continue;
                        w.align(zone::kAlign2);
                        w.write(t.nodes.data(), 2 * t.nodes.size());
                    }
                }
                w.align(zone::kAlign4);
                w.writeStruct(iw4::G_GlassData{});  // no pieces, no names
                w.pop();
            }};
}
static_assert(sizeof(iw4::pathnode_t) == 168 && sizeof(iw4::pathnode_constant_t) == 72 &&
                  offsetof(iw4::pathnode_constant_t, totalLinkCount) == 56 && offsetof(iw4::pathnode_constant_t, Links) == 64 &&
                  sizeof(iw4::pathlink_s) == 12 && sizeof(iw4::pathnode_tree_t) == 24 && sizeof(iw4::PathData) == 80 &&
                  offsetof(iw4::PathData, pathVis) == 56 && offsetof(iw4::PathData, nodeTree) == 72,
              "path structs differ from Load_PathData / trainer.ff");
static_assert(sizeof(iw4::G_GlassData) == 0x90 && offsetof(iw4::G_GlassData, glassNames) == 0x18,
              "G_GlassData layout differs from Load_G_GlassData");
static_assert(sizeof(iw4::GameWorldSp) == 0x70 && offsetof(iw4::GameWorldSp, vehicleTrack) == 0x58 &&
                  offsetof(iw4::GameWorldSp, g_glassData) == 0x68,
              "GameWorldSp layout differs from Load_GameWorldSp");

// Load_FxWorld (0x119ed0): struct 0xb0; push VIRTUAL; name; FxGlassSystem inline (every array
// pointer null = no glass, each one is checked before loading); pop.
ZoneAsset buildFxWorld(const std::string& name) {
    return {IW4_FXWORLD, [name](ZoneWriter& w) {
                iw4::FxWorld f{};
                f.name = follow<const char>();
                w.writeStruct(f);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                w.pop();
            }};
}

// Load_RawFile (0x11e730): struct 0x18; push VIRTUAL; name; buffer (byte-aligned,
// compressedLen bytes, or len+1 when stored uncompressed); pop.
ZoneAsset buildRawFile(const std::string& name, int compressedLen, int len,
                       std::vector<uint8_t> buffer) {
    return {IW4_RAWFILE, [name, compressedLen, len, buffer = std::move(buffer)](ZoneWriter& w) {
                iw4::RawFile r{};
                r.name = follow<const char>();
                r.compressedLen = compressedLen;
                r.len = len;
                r.buffer = buffer.empty() ? nullptr : follow<const char>();
                w.writeStruct(r);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                if (!buffer.empty()) {
                    w.align(zone::kAlign1);
                    w.write(buffer.data(), buffer.size());
                }
                w.pop();
            },
            zone::kPtrFollow};
}

ZoneAsset buildCompressedRawFile(const std::string& name, const std::string& text) {
    // Stock rawfiles with content are zlib streams (compressedLen > 0, data starts 78 da).
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(text.size()));
    std::vector<uint8_t> z(cap);
    if (mz_compress2(z.data(), &cap, reinterpret_cast<const unsigned char*>(text.data()),
                     static_cast<mz_ulong>(text.size()), MZ_BEST_COMPRESSION) != MZ_OK)
        cap = 0;
    z.resize(cap);
    const int compressedLen = static_cast<int>(z.size());  // before the move below: argument
    return buildRawFile(name, compressedLen, static_cast<int>(text.size()), std::move(z));  // order is unspecified
}

std::string mapAssetDir(MapTarget target) { return target == MapTarget::SP ? "maps/" : "maps/mp/"; }

std::string zoneSourcePath(const std::string& mapPath) {
    return (std::filesystem::path(mapPath).parent_path() / "zone_source").string();
}

namespace zsrc {
std::string lower(std::string x) {
    for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return x;
}
// <stem>_lod1.._lod9: extra LODs, not compiled yet (LOD0 / unsuffixed is the model).
bool isExtraLod(const std::string& lowerStem) {
    size_t n = lowerStem.size();
    return n > 5 && lowerStem.compare(n - 5, 4, "_lod") == 0 && lowerStem[n - 1] >= '1' && lowerStem[n - 1] <= '9';
}
std::vector<std::filesystem::path> list(const std::string& zoneSourceDir, const char* sub, const char* ext) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code ec;
    fs::path dir = fs::path(zoneSourceDir) / sub;
    if (zoneSourceDir.empty() || !fs::is_directory(dir, ec)) return files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (ext && lower(e.path().extension().string()) != ext) continue;
        if (ext && std::string(ext) == ".xmodel_export" && isExtraLod(lower(e.path().stem().string()))) continue;
        files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}
// Asset name of a zone_source file: its stem, lowercase, without a trailing _lod0.
std::string assetName(const std::filesystem::path& f) {
    std::string n = lower(f.stem().string());
    if (n.size() > 5 && n.compare(n.size() - 5, 5, "_lod0") == 0) n.resize(n.size() - 5);
    return n;
}
// A weapon file's model/anim name -> a zone_source asset: exact (any case) first, else the same
// name ignoring t5_/t6_/t7_/weapon_/viewmodel_ tokens and _lodN, preferring the candidate that
// agrees on "viewmodel" (viewmodel_ak74u -> t5_weapon_ak74u_viewmodel, weapon_ak74u ->
// t5_weapon_ak74u_lod0, viewmodel_ak74u_idle -> ak74u_idle).
std::string resolve(const std::string& ref, const std::vector<std::string>& names) {
    const std::string r = lower(ref);
    for (const std::string& n : names)
        if (n == r) return n;
    auto core = [](const std::string& x, bool& vm) {
        std::string out, tok;
        vm = false;
        auto flush = [&] {
            if (tok == "viewmodel") vm = true;
            bool lod = tok.size() == 4 && tok.compare(0, 3, "lod") == 0 && std::isdigit(static_cast<unsigned char>(tok[3]));
            if (!tok.empty() && tok != "t5" && tok != "t6" && tok != "t7" && tok != "weapon" && tok != "viewmodel" && !lod)
                out += (out.empty() ? "" : "_") + tok;
            tok.clear();
        };
        for (char c : x) {
            if (c == '_') flush();
            else tok += c;
        }
        flush();
        return out;
    };
    bool rvm;
    const std::string rc = core(r, rvm);
    std::string best;
    int bestScore = -1;
    for (const std::string& n : names) {
        bool nvm;
        if (core(n, nvm) != rc) continue;
        int score = (nvm == rvm ? 2 : 0) + (n.find("_lod") == std::string::npos ? 1 : 0);
        if (score > bestScore) bestScore = score, best = n;
    }
    return best;
}
}  // namespace zsrc

// ---- effects (zone_source/fx/**/*.efx -> FxEffectDef; see zone/efx.h) ----
namespace {

// A sprite material built for an effect: a stock effect technique set from common.ff by name (the
// sets every stock effect material uses: effect_[zfeather_]add|blend[_nofog]) with the material
// layout, state bits and constants copied from common.ff's own effect materials (gfx_smk_white_atlas,
// gfx_muzflash_m16, gfx_glow_hs ...), and the texture embedded from zone_source.
struct FxMaterialPlan {
    std::string name;
    bool additive = false;
    bool fog = true;
    bool cloud = false;  // particle_cloud set (cloud elements), laid out as common.ff's cloud_outdoor
    float feather = 0;   // soft-particle depth (zfeather sets); 0 = hard edges
    int rows = 1, cols = 1;
    gfx::Image image;
    std::string techset() const {
        if (cloud) return "particle_cloud";
        return std::string(feather > 0 ? "effect_zfeather_" : "effect_") + (additive ? "add" : "blend") + (fog ? "" : "_nofog");
    }
};

// A BO1 effect material (bo1::Install) as a plan: its blend/fog/feather/atlas and its image (DXT
// as stored, top level).
bool planBo1FxMaterial(bo1::Install& bo, const std::string& name, FxMaterialPlan& p, std::vector<std::string>* warnings) {
    bo1::EffectMaterial m;
    if (!bo.material(name, m)) return false;
    bo1::Image im;
    std::string err;
    if (!bo.image(m.image, im, err)) {
        if (warnings) warnings->push_back("fx material " + name + " (Black Ops): " + err);
        return false;
    }
    p = FxMaterialPlan{};
    p.name = name;
    p.additive = m.additive;
    p.fog = m.fog;
    p.cloud = m.cloud;
    p.feather = m.feather;
    p.rows = m.rows;
    p.cols = m.cols;
    p.image.name = "iw4r_bo1_" + zsrc::lower(m.image);
    p.image.mapType = 3;
    p.image.semantic = 2;
    p.image.category = 2;
    p.image.width = static_cast<uint16_t>(im.width);
    p.image.height = static_cast<uint16_t>(im.height);
    p.image.levelCount = 1;
    p.image.loadFlags = 2;
    p.image.format = im.format;
    p.image.pixels = std::move(im.pixels);
    return true;
}

// zone_source/fx/materials/<name>.txt (optional): "image <file>", "blend add|blend", "feather <depth>",
// "fog on|off", "atlas <rows> <cols>". Without it: the texture is zone_source/images/<name> (or the
// name without its gfx_ prefix), blend mode additive when the name has "add"/"glow"/"flash", else
// alpha blend; no feathering; one frame.
bool planFxMaterial(const std::string& zoneSourceDir, const std::string& name, FxMaterialPlan& p, std::vector<std::string>* warnings) {
    namespace fs = std::filesystem;
    if (zoneSourceDir.empty()) return false;
    p = FxMaterialPlan{};
    p.name = name;
    const std::string lname = zsrc::lower(name);
    p.additive = lname.find("add") != std::string::npos || lname.find("glow") != std::string::npos ||
                 lname.find("flash") != std::string::npos;
    std::string imageFile;
    const fs::path settings = fs::path(zoneSourceDir) / "fx" / "materials" / (name + ".txt");
    std::error_code ec;
    const bool hasSettings = fs::is_regular_file(settings, ec);
    if (hasSettings) {
        std::ifstream in(settings);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string k;
            ls >> k;
            k = zsrc::lower(k);
            if (k == "image") std::getline(ls >> std::ws, imageFile);
            else if (k == "blend") {
                std::string v;
                ls >> v;
                p.additive = zsrc::lower(v) == "add";
            } else if (k == "feather") ls >> p.feather;
            else if (k == "fog") {
                std::string v;
                ls >> v;
                p.fog = zsrc::lower(v) != "off";
            } else if (k == "atlas") ls >> p.rows >> p.cols;
        }
    }
    p.rows = std::clamp(p.rows, 1, 64);
    p.cols = std::clamp(p.cols, 1, 64);
    std::vector<fs::path> cands;
    const fs::path img = fs::path(zoneSourceDir) / "images";
    if (!imageFile.empty()) cands.push_back(img / imageFile), cands.push_back(fs::path(zoneSourceDir) / "fx" / "materials" / imageFile);
    std::string bare = lname.rfind("gfx_", 0) == 0 ? name.substr(4) : name;
    for (const std::string& st : {name, bare})
        for (const char* ext : {".png", ".tga", ".PNG", ".TGA"}) cands.push_back(img / (st + ext));
    std::string found;
    for (const fs::path& c : cands)
        if (fs::is_regular_file(c, ec)) {
            found = c.string();
            break;
        }
    if (found.empty()) {
        if (hasSettings && warnings) warnings->push_back("fx material " + name + ": image '" + imageFile + "' not found");
        return false;
    }
    imageio::Rgba rgba;
    std::string err;
    if (!imageio::load(found, rgba, err)) {
        if (warnings) warnings->push_back("fx material " + name + ": " + err);
        return false;
    }
    imageio::toPowerOfTwo(rgba);
    p.image.name = "iw4r_fx_" + lname;
    p.image.mapType = 3;
    p.image.semantic = 2;
    p.image.category = 2;
    p.image.width = static_cast<uint16_t>(rgba.width);
    p.image.height = static_cast<uint16_t>(rgba.height);
    p.image.levelCount = 1;
    p.image.loadFlags = 2;
    p.image.format = 0x15;  // A8R8G8B8
    int levels = 0;
    std::vector<uint8_t> chain = imageio::bgraMipChain(rgba, levels);
    p.image.pixels.assign(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(4ull * rgba.width * rgba.height));
    return true;
}

// Load_Material order (zonesim materialPtr): struct 0x78; name; technique set by name; texture
// table (colorMap) + the image inline; constants (align 16); state bits. Layout as common.ff's
// effect materials: sortKey 48, atlas rows/cols at +10/+11, techniques 4..8 -> state bits 0 and
// 44 -> 1, textureCount 1, stateFlags 0x13, cameraRegion 2.
void writeFxMaterial(ZoneWriter& w, const FxMaterialPlan& p) {
    using namespace zone;
    std::vector<uint8_t> m(0x78, 0);
    put64(m, 0, kPtrFollow);
    m[9] = p.cloud ? 47 : 48;
    m[10] = static_cast<uint8_t>(p.rows);
    m[11] = static_cast<uint8_t>(p.cols);
    for (int i = 0; i < 48; i++) m[32 + i] = 0xFF;
    for (int i = 4; i <= 8; i++) m[32 + i] = 0;
    if (!p.cloud) m[32 + 44] = 1;  // cloud_outdoor has no technique-44 state
    struct Const {
        uint32_t hash;
        char name[12];
        float v[4];
    };
    std::vector<Const> consts;
    if (p.feather > 0) consts.push_back({0x4D7EA234u, {'f', 'e', 'a', 't', 'h', 'e', 'r', 'P', 'a', 'r', 'm', 's'}, {1.0f / p.feather, p.feather, 0, 0}});
    consts.push_back({0xB60C3B3Au, {'c', 'o', 'l', 'o', 'r', 'T', 'i', 'n', 't', 0, 0, 0}, {1, 1, 1, 1}});
    static_assert(sizeof(Const) == 32, "MaterialConstantDef");
    if (p.cloud) consts.clear();  // as cloud_outdoor
    m[80] = 1;
    m[81] = static_cast<uint8_t>(consts.size());
    m[82] = p.cloud ? 1 : 2;
    m[83] = 0x13;
    m[84] = 2;
    put64(m, 88, kPtrFollow);
    put64(m, 96, kPtrFollow);
    put64(m, 104, consts.empty() ? 0 : kPtrFollow);
    put64(m, 112, kPtrFollow);
    w.write(m.data(), m.size());
    w.push(BLOCK_VIRTUAL);
    w.xstring(p.name);
    loadAssetPtr(w, kPtrFollow, [&](ZoneWriter& w2) {  // technique set by name (common.ff)
        std::vector<uint8_t> ts(0x198, 0);
        put64(ts, 0, kPtrFollow);
        w2.write(ts.data(), ts.size());
        w2.push(BLOCK_VIRTUAL);
        w2.xstring("," + p.techset());
        w2.pop();
    });
    w.align(kAlign4);
    uint8_t tex[16] = {0x41, 0x10, 0xAB, 0xA0, 'c', 'p', 0x0B, 2};  // colorMap, sampler 0x0b, semantic 2
    const uint64_t ins = kPtrInsert;
    std::memcpy(tex + 8, &ins, 8);
    w.write(tex, sizeof tex);
    writeImage(w, p.image);
    if (!consts.empty()) {
        w.align(kAlign16);
        w.write(consts.data(), sizeof(Const) * consts.size());
    }
    w.align(kAlign4);
    const uint32_t sb[4] = {p.additive ? (p.feather > 0 ? 0x09218922u : 0x19218922u) : 0x19289165u, 0x0000000Cu, 0x88128812u, 0xE49E492Cu};
    w.write(sb, p.cloud ? 8 : sizeof sb);
    w.pop();
}

// Load_FxEffectDefHandle order (zonesim fxPtrAt / fxElemDef): struct 40; name; elemDefs (288
// each, align 4); per element velSamples, visSamples (align 4), visuals (array or inline),
// effectOnImpact/Death/Emitted names, extended (trail).
void writeFx(ZoneWriter& w, const efx::Compiled& c) {
    using namespace zone;
    uint8_t fx[40] = {};
    const uint64_t F = kPtrFollow, I = kPtrInsert;
    std::memcpy(fx, &F, 8);
    const int32_t h[6] = {c.flags, c.totalSize, c.msecLoopingLife, c.countLooping, c.countOneShot, c.countEmission};
    std::memcpy(fx + 8, h, 24);
    std::memcpy(fx + 32, &F, 8);
    w.write(fx, sizeof fx);
    w.push(BLOCK_VIRTUAL);
    w.xstring(c.name);
    w.align(kAlign4);
    for (const efx::ElemOut& e : c.elems) {
        std::array<uint8_t, 288> d = e.def;
        std::memcpy(d.data() + 184, &F, 8);
        std::memcpy(d.data() + 192, &F, 8);
        const bool decal = e.type == 11, light = e.type == 8 || e.type == 9;
        const size_t count = decal ? e.visuals.size() / 2 : e.visuals.size();
        if (!light && !e.visuals.empty()) {
            if (decal || count > 1) {
                std::memcpy(d.data() + 200, &F, 8);
            } else {
                const efx::Visual& v = e.visuals[0];
                std::memcpy(d.data() + 200, v.kind == efx::Visual::Name ? &F : &I, 8);
            }
        }
        if (!e.onImpact.empty()) std::memcpy(d.data() + 232, &F, 8);
        if (!e.onDeath.empty()) std::memcpy(d.data() + 240, &F, 8);
        if (!e.emitted.empty()) std::memcpy(d.data() + 248, &F, 8);
        if (!e.trail.empty()) std::memcpy(d.data() + 272, &F, 8);
        w.write(d.data(), d.size());
    }
    auto visual = [&](const efx::Visual& v) {
        switch (v.kind) {
            case efx::Visual::Material: writeMaterialRef(w, v.name); break;
            case efx::Visual::Model: writeXModelRef(w, v.name); break;
            case efx::Visual::Name: w.xstring(v.name); break;
            default: break;
        }
    };
    for (const efx::ElemOut& e : c.elems) {
        w.align(kAlign4);
        w.write(e.velSamples.data(), e.velSamples.size());
        w.align(kAlign4);
        w.write(e.visSamples.data(), e.visSamples.size());
        const bool decal = e.type == 11, light = e.type == 8 || e.type == 9;
        const size_t count = decal ? e.visuals.size() / 2 : e.visuals.size();
        if (!light && !e.visuals.empty()) {
            if (decal || count > 1) {
                w.align(kAlign4);
                for (const efx::Visual& v : e.visuals) w.u64(v.kind == efx::Visual::Name ? F : I);
                for (const efx::Visual& v : e.visuals) visual(v);
            } else {
                visual(e.visuals[0]);
            }
        }
        if (!e.onImpact.empty()) w.xstring(e.onImpact);
        if (!e.onDeath.empty()) w.xstring(e.onDeath);
        if (!e.emitted.empty()) w.xstring(e.emitted);
        if (!e.trail.empty()) {
            std::vector<uint8_t> t = e.trail;
            put64(t, 24, F);
            put64(t, 40, F);
            w.align(kAlign4);
            w.write(t.data(), t.size());
            w.align(kAlign4);
            w.write(e.trailVerts.data(), e.trailVerts.size());
            w.align(kAlign2);
            w.write(e.trailInds.data(), 2 * e.trailInds.size());
        }
    }
    w.pop();
}

// Atlas sizes of the stock materials common.ff (SP) / common_mp.ff (MP) carry, which effects may
// name without supplying them (gfx_smk_white_atlas, ...). Read once from the game's zone, cached.
std::map<std::string, std::pair<int, int>> stockFxMaterials(const std::string& gameDir, const std::string& cacheDir, MapTarget target) {
    namespace fs = std::filesystem;
    std::map<std::string, std::pair<int, int>> out;
    const std::string zoneName = target == MapTarget::SP ? "common" : "common_mp";
    const fs::path cache = cacheDir.empty() ? fs::path() : fs::path(cacheDir) / ("fxmaterials_" + zoneName + ".txt");
    std::error_code ec;
    if (!cache.empty() && fs::is_regular_file(cache, ec)) {
        std::ifstream in(cache);
        std::string n;
        int r, c;
        while (in >> n >> r >> c) out[n] = {r, c};
        return out;
    }
    if (gameDir.empty()) return out;
    std::ifstream in(fs::path(gameDir) / "zone" / "english" / (zoneName + ".ff"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    std::string err;
    if (bytes.empty() || !readFastfile(bytes, zf, err)) return out;
    zone::SimulatedZone sim;
    size_t loaded = 0;
    std::string stopped;
    zone::simulatePrefix(zf.zone, sim, loaded, stopped);
    for (const zone::LoadedAsset& a : sim.assets) {
        if (a.type != 5 || a.reference) continue;
        const uint8_t* m = static_cast<const uint8_t*>(a.header);
        const char* n;
        std::memcpy(&n, m, 8);
        if (n && *n) out[zsrc::lower(n)] = {std::max<int>(1, m[10]), std::max<int>(1, m[11])};
    }
    if (!cache.empty()) {
        std::ofstream o(cache);
        for (const auto& [n, rc] : out) o << n << " " << rc.first << " " << rc.second << "\n";
    }
    return out;
}

}  // namespace

ZoneSourceEffects loadZoneSourceEffects(const std::string& zoneSourceDir, const std::string& gameDir, const std::string& cacheDir,
                                        MapTarget target, std::vector<std::string>& warnings) {
    namespace fs = std::filesystem;
    ZoneSourceEffects out;
    const fs::path dir = fs::path(zoneSourceDir) / "fx";
    std::error_code ec;
    if (zoneSourceDir.empty() || !fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        if (it->is_regular_file(ec) && zsrc::lower(it->path().extension().string()) == ".efx") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    if (files.empty()) return out;
    const auto stock = stockFxMaterials(gameDir, cacheDir, target);
    bo1::Install bo(bo1::defaultRoot(gameDir));
    // Material name -> the plan this zone builds (null: referenced by name). zone_source first; then
    // a stock material the game's common zone has; then a Black Ops effect material.
    std::map<std::string, std::shared_ptr<FxMaterialPlan>> plans;
    size_t fromBo1 = 0;
    auto lookup = [&](const std::string& name) {
        efx::MaterialInfo mi;
        auto it = plans.find(name);
        if (it == plans.end()) {
            auto p = std::make_shared<FxMaterialPlan>();
            bool ours = planFxMaterial(zoneSourceDir, name, *p, &warnings);
            if (!ours && !stock.count(zsrc::lower(name)) && bo.valid() && planBo1FxMaterial(bo, name, *p, &warnings)) {
                ours = true;
                fromBo1++;
            }
            it = plans.emplace(name, ours ? p : nullptr).first;
        }
        if (it->second) {
            mi.known = true;
            mi.rows = it->second->rows;
            mi.cols = it->second->cols;
        } else if (auto s = stock.find(zsrc::lower(name)); s != stock.end()) {
            mi.known = true;
            mi.rows = s->second.first;
            mi.cols = s->second.second;
        } else if (bo1::EffectMaterial bm; bo.valid() && bo.materialTechset(name, bm.techset) &&
                   zsrc::lower(bm.techset).rfind("distortion", 0) == 0 && stock.count("gfx_distortion_heat_far")) {
            mi.known = true;  // BO1 heat distortion -> the stock one (common.ff)
            mi.replacement = "gfx_distortion_heat_far";
        } else if (stock.empty()) {
            mi.known = true;  // no game zone to check against: trust the name
        }
        return mi;
    };
    std::vector<efx::Compiled> compiled;
    for (const fs::path& f : files) {
        std::string rel = zsrc::lower(fs::relative(f, dir, ec).replace_extension().generic_string());
        if (rel.rfind("materials/", 0) == 0) continue;
        std::ifstream in(f, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        efx::Effect fx;
        std::string err;
        efx::Compiled c;
        if (!efx::parse(text, fx, err) || !efx::compile(fx, rel, lookup, c, warnings, err)) {
            warnings.push_back("fx " + rel + ": " + err);
            continue;
        }
        compiled.push_back(std::move(c));
    }
    // Effects that reference others (runners, impacts, emission) load after them.
    std::set<std::string> names, placed;
    for (const efx::Compiled& c : compiled) names.insert(c.name);
    std::vector<const efx::Compiled*> ordered;
    for (size_t pass = 0; pass <= compiled.size() && ordered.size() < compiled.size(); pass++)
        for (const efx::Compiled& c : compiled) {
            if (placed.count(c.name)) continue;
            bool ready = true;
            for (const std::string& d : c.dependencies) ready &= !names.count(zsrc::lower(d)) || placed.count(zsrc::lower(d)) != 0;
            if (ready || pass == compiled.size()) {
                ordered.push_back(&c);
                placed.insert(c.name);
            }
        }
    if (fromBo1) warnings.push_back("fx: " + std::to_string(fromBo1) + " material(s) built from Black Ops (" + bo.root() + ")");
    // Materials first (the effects reference them by name), then the effects.
    for (const auto& [n, p] : plans)
        if (p) out.assets.push_back({IW4_MATERIAL, [plan = *p](ZoneWriter& w) { writeFxMaterial(w, plan); }});
    for (const efx::Compiled* c : ordered) {
        out.assets.push_back({IW4_FX, [cc = *c](ZoneWriter& w) { writeFx(w, cc); }});
        out.looping[c->name] = c->countLooping > 0;
        warnings.push_back("fx " + c->name + ": " + std::to_string(c->elems.size()) + " element(s)");
    }
    return out;
}

// Script for the map's fx_origin entities (fx, angles, delay): loads every effect in main() and,
// once the level runs, spawns each: delay <= 0 -> spawnfx + triggerfx once (an effect with looping
// elements keeps playing), else playfx every `delay` seconds.
struct FxPreviewMaterials::Impl {
    std::string zoneSourceDir;
    std::map<std::string, std::pair<int, int>> stock;
    bo1::Install bo;
    std::map<std::string, FxPreviewMaterial> cache;
    Impl(const std::string& zs, const std::string& gameDir, const std::string& cacheDir)
        : zoneSourceDir(zs), stock(stockFxMaterials(gameDir, cacheDir, MapTarget::SP)), bo(bo1::defaultRoot(gameDir)) {}
};

FxPreviewMaterials::FxPreviewMaterials(const std::string& zoneSourceDir, const std::string& gameDir, const std::string& cacheDir)
    : impl_(std::make_unique<Impl>(zoneSourceDir, gameDir, cacheDir)) {}
FxPreviewMaterials::~FxPreviewMaterials() = default;

const FxPreviewMaterial& FxPreviewMaterials::get(const std::string& name) {
    auto it = impl_->cache.find(name);
    if (it != impl_->cache.end()) return it->second;
    FxPreviewMaterial out;
    FxMaterialPlan p;
    const std::string lname = zsrc::lower(name);
    bool ours = planFxMaterial(impl_->zoneSourceDir, name, p, nullptr);
    if (!ours && !impl_->stock.count(lname) && impl_->bo.valid()) ours = planBo1FxMaterial(impl_->bo, name, p, nullptr);
    if (ours) {
        out.found = true;
        out.additive = p.additive;
        out.rows = p.rows, out.cols = p.cols;
        out.width = p.image.width, out.height = p.image.height;
        out.format = p.image.format;
        out.pixels = std::move(p.image.pixels);
    } else if (auto s = impl_->stock.find(lname); s != impl_->stock.end()) {
        out.found = out.stock = true;
        out.rows = s->second.first, out.cols = s->second.second;
        // stock effect materials: additive by the usual naming (glows, flashes, flares, sparks)
        out.additive = lname.find("add") != std::string::npos || lname.find("glow") != std::string::npos ||
                       lname.find("flash") != std::string::npos || lname.find("flare") != std::string::npos ||
                       lname.find("spark") != std::string::npos || lname.find("fire") != std::string::npos;
        if (lname.find("distortion") != std::string::npos) out.found = false;  // a screen warp: nothing to draw
    }
    return impl_->cache.emplace(name, std::move(out)).first->second;
}

std::string fxPlacementScript(const std::vector<FxPlacement>& fx) {
    std::vector<std::string> names;
    for (const FxPlacement& p : fx)
        if (std::find(names.begin(), names.end(), p.effect) == names.end()) names.push_back(p.effect);
    std::string s = "// Generated by iw4-radiant from the map's fx_origin entities. Do not edit.\nmain()\n{\n\tlevel._iw4r_fx = [];\n";
    for (size_t i = 0; i < names.size(); i++) s += "\tlevel._iw4r_fx[" + std::to_string(i) + "] = loadfx(\"" + names[i] + "\");\n";
    s += "\tthread place();\n}\n\nplace()\n{\n\twaittillframeend;\n\twait 0.05;\n";
    auto num = [](float v) {  // 4 decimals, no -0 / 1e-15 noise
        double r = std::round(static_cast<double>(v) * 10000.0) / 10000.0;
        if (r == 0) r = 0;
        char b[32];
        std::snprintf(b, sizeof b, "%g", r);
        return std::string(b);
    };
    auto vec = [&](const float* v) { return "(" + num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]) + ")"; };
    auto quote = [](const std::string& t) {
        std::string q = "\"";
        for (char c : t)
            if (c != '"' && c != '\\' && c >= 32) q += c;
        return q + "\"";
    };
    std::vector<std::string> watched;  // fx groups with triggers
    bool dynamic = false;
    for (const FxPlacement& p : fx) {
        const size_t id = static_cast<size_t>(std::find(names.begin(), names.end(), p.effect) - names.begin());
        const std::string args = std::to_string(id) + ", " + vec(p.origin) + ", " + vec(p.forward) + ", " + vec(p.up) + ", " + num(p.delay);
        if (p.name.empty() && p.duration <= 0) {
            s += "\tthread play(" + args + ");\n";
            continue;
        }
        dynamic = true;
        s += "\tthread dynamic(" + args + ", " + quote(p.name) + ", " + num(p.duration) + ", " + (p.once ? "1" : "0") + ", " +
             (p.waitStart ? "1" : "0") + ");\n";
        if (!p.triggers.empty() && std::find(watched.begin(), watched.end(), p.name) == watched.end()) watched.push_back(p.name);
    }
    for (const std::string& w : watched) s += "\tthread watch(" + quote(w) + ");\n";
    s += "}\n\n"
         "play(id, org, fwd, up, delay)\n{\n"
         "\tif (delay <= 0)\n\t{\n"
         "\t\tent = spawnfx(level._iw4r_fx[id], org, fwd, up);\n"
         "\t\ttriggerfx(ent);\n"
         "\t\treturn;\n\t}\n"
         "\tfor (;;)\n\t{\n"
         "\t\tplayfx(level._iw4r_fx[id], org, fwd, up);\n"
         "\t\twait delay;\n\t}\n}\n";
    if (!dynamic) return s;
    // A dynamic fx_origin: waits for level notify(name) (unless it starts on), then runs until
    // level notify(name + "_stop") or `duration` seconds; again on the next start unless `once`.
    // Looping effects: spawnfx/triggerfx, deleted on stop. One-shot: playfx, every `delay` seconds
    // while running (delay 0: once per start).
    s += "\n"
         "dynamic(id, org, fwd, up, delay, name, duration, once, waitstart)\n{\n"
         "\tfirst = true;\n"
         "\tfor (;;)\n\t{\n"
         "\t\tif (!first || waitstart)\n"
         "\t\t\tlevel waittill(name);\n"
         "\t\tfirst = false;\n"
         "\t\tif (level._iw4r_fxloop[id])\n\t\t{\n"
         "\t\t\tent = spawnfx(level._iw4r_fx[id], org, fwd, up);\n"
         "\t\t\ttriggerfx(ent);\n"
         "\t\t\trun = spawnstruct();\n"
         "\t\t\trun waitstop(name, duration);\n"
         "\t\t\tent delete();\n\t\t}\n"
         "\t\telse if (delay > 0)\n\t\t{\n"
         "\t\t\trun = spawnstruct();\n"
         "\t\t\trun thread repeat(id, org, fwd, up, delay);\n"
         "\t\t\trun waitstop(name, duration);\n\t\t}\n"
         "\t\telse\n"
         "\t\t\tplayfx(level._iw4r_fx[id], org, fwd, up);\n"
         "\t\tif (once)\n"
         "\t\t\treturn;\n"
         "\t\tif (name == \"\")\n"
         "\t\t\treturn;\n"
         "\t\twait 0.05;\n\t}\n}\n\n"
         "repeat(id, org, fwd, up, delay)\n{\n"
         "\tself endon(\"done\");\n"
         "\tfor (;;)\n\t{\n"
         "\t\tplayfx(level._iw4r_fx[id], org, fwd, up);\n"
         "\t\twait delay;\n\t}\n}\n\n"
         // returns on level notify(name + "_stop") or after duration (> 0), whichever comes first
         "waitstop(name, duration)\n{\n"
         "\tif (name != \"\")\n"
         "\t\tself thread stopon(name + \"_stop\");\n"
         "\tif (duration > 0)\n"
         "\t\tself thread stopin(duration);\n"
         "\tif (name != \"\" || duration > 0)\n"
         "\t\tself waittill(\"stop\");\n"
         "\telse\n"
         "\t\tlevel waittill(\"_iw4r_never\");\n"
         "\tself notify(\"done\");\n}\n\n"
         "stopon(msg)\n{\n"
         "\tself endon(\"done\");\n"
         "\tlevel waittill(msg);\n"
         "\tself notify(\"stop\");\n}\n\n"
         "stopin(t)\n{\n"
         "\tself endon(\"done\");\n"
         "\twait t;\n"
         "\tself notify(\"stop\");\n}\n\n"
         // every trigger whose "target" is the group: walking in starts it (or stops it, with
         // script_noteworthy "stop"); once per entry
         "watch(name)\n{\n"
         "\ttrigs = getentarray(name, \"target\");\n"
         "\tfor (i = 0; i < trigs.size; i++)\n"
         "\t\ttrigs[i] thread watchtrigger(name);\n}\n\n"
         "watchtrigger(name)\n{\n"
         "\tmsg = name;\n"
         "\tif (isdefined(self.script_noteworthy) && self.script_noteworthy == \"stop\")\n"
         "\t\tmsg = name + \"_stop\";\n"
         "\tfor (;;)\n\t{\n"
         "\t\tself waittill(\"trigger\", who);\n"
         "\t\tlevel notify(msg);\n"
         "\t\twhile (isdefined(who) && isdefined(self) && who istouching(self))\n"
         "\t\t\twait 0.1;\n\t}\n}\n";
    // which effects loop (spawnfx) vs one-shot (playfx), set in main()
    std::string loops = "\tlevel._iw4r_fxloop = [];\n";
    for (size_t i = 0; i < names.size(); i++) {
        bool looping = false;
        for (const FxPlacement& p : fx)
            if (p.effect == names[i] && p.looping) looping = true;
        loops += "\tlevel._iw4r_fxloop[" + std::to_string(i) + "] = " + (looping ? "true" : "false") + ";\n";
    }
    const std::string anchor = "\tthread place();\n";
    s.insert(s.find(anchor), loops);
    return s;
}

std::vector<FxPlacement> fxPlacements(Document& doc, const std::map<std::string, bool>& looping, std::vector<std::string>* warnings) {
    auto warn = [&](const std::string& m) {
        if (warnings) warnings->push_back(m);
    };
    // fx groups: fx_origin targetnames
    std::set<std::string> groups;
    for (const Entity& e : doc.ents)
        if (e.classname() == "fx_origin")
            if (const std::string* t = e.find("targetname"); t && !t->empty()) groups.insert(*t);
    // triggers pointing at a group
    std::map<std::string, std::vector<std::string>> triggers;
    for (Entity& e : doc.ents) {
        const std::string* tp = e.find("target");
        if (!tp || !groups.count(*tp) || e.classname() == "fx_origin") continue;
        const std::string t = *tp, cls = e.classname();  // copies: set() below may move the keys
        if (cls == "trigger_radius") {
            if (!e.find("radius")) e.set("radius", "64"), warn("trigger_radius -> fx \"" + t + "\" has no radius; using 64");
            if (!e.find("height")) e.set("height", "128"), warn("trigger_radius -> fx \"" + t + "\" has no height; using 128");
            triggers[t].push_back(cls);
        } else if (clip::isBrushTriggerClass(cls)) {
            if (e.brushes.empty()) warn(cls + " -> fx \"" + t + "\" has no brushes: it can't be touched");
            triggers[t].push_back(cls);  // a brush trigger (compiled by clip::buildTriggers)
        } else {
            triggers[t].push_back(cls);  // anything else that targets it: the script still listens
        }
    }
    std::vector<FxPlacement> out;
    for (const Entity& e : doc.ents) {
        if (e.classname() != "fx_origin") continue;
        const std::string* fxName = e.find("fx");
        if (!fxName || fxName->empty()) {
            warn("fx_origin without an \"fx\" key; ignored");
            continue;
        }
        FxPlacement p{};
        p.effect = zsrc::lower(*fxName);
        Vec3 o;
        if (e.origin(o)) p.origin[0] = static_cast<float>(o.x), p.origin[1] = static_cast<float>(o.y), p.origin[2] = static_cast<float>(o.z);
        double pitch = 270, yaw = 0, roll = 0;  // default: pointing up, as createfx
        if (const std::string* a = e.find("angles")) std::sscanf(a->c_str(), "%lf %lf %lf", &pitch, &yaw, &roll);
        else if (const std::string* a1 = e.find("angle")) pitch = 0, yaw = std::atof(a1->c_str());
        const double d2r = 3.14159265358979 / 180.0, sinP = std::sin(pitch * d2r), cp = std::cos(pitch * d2r), sy = std::sin(yaw * d2r),
                     cy = std::cos(yaw * d2r), sr = std::sin(roll * d2r), cr = std::cos(roll * d2r);
        const double fwd[3] = {cp * cy, cp * sy, -sinP}, up[3] = {cr * sinP * cy + sr * sy, cr * sinP * sy - sr * cy, cr * cp};
        for (int k = 0; k < 3; k++) p.forward[k] = static_cast<float>(fwd[k]), p.up[k] = static_cast<float>(up[k]);
        auto lp = looping.find(p.effect);
        p.looping = lp == looping.end() || lp->second;
        if (const std::string* t = e.find("targetname")) p.name = *t;
        if (const std::string* du = e.find("duration")) p.duration = static_cast<float>(std::atof(du->c_str()));
        if (const std::string* on = e.find("once")) p.once = std::atoi(on->c_str()) != 0;
        const std::string* so = e.find("start_off");
        if (auto tr = triggers.find(p.name); !p.name.empty() && tr != triggers.end()) p.triggers = tr->second;
        p.waitStart = !p.name.empty() && (!p.triggers.empty() || (so && std::atoi(so->c_str()) != 0));
        // one-shot effects repeat every 2 s unless told otherwise; a dynamic one plays once per start
        const bool dyn = !p.name.empty() || p.duration > 0;
        p.delay = !p.looping && !dyn ? 2.0f : 0.0f;
        if (const std::string* dl = e.find("delay")) p.delay = static_cast<float>(std::atof(dl->c_str()));
        out.push_back(p);
    }
    return out;
}

// Inserts `call` as the first statement of the script's main() (after its opening brace) unless the
// script already mentions it.
std::string injectIntoMain(const std::string& script, const std::string& call) {
    if (script.find(call) != std::string::npos) return script;
    size_t m = script.find("main(");
    while (m != std::string::npos && m > 0 && (std::isalnum(static_cast<unsigned char>(script[m - 1])) || script[m - 1] == '_'))
        m = script.find("main(", m + 1);
    if (m == std::string::npos) return script;
    size_t brace = script.find('{', m);
    if (brace == std::string::npos) return script;
    return script.substr(0, brace + 1) + "\n\t" + call + "\n" + script.substr(brace + 1);
}

std::vector<modelbuild::Model> loadZoneSourceModels(const std::string& zoneSourceDir, std::vector<std::string>& warnings,
                                                    const std::map<std::string, modelbuild::BuildOptions>* hints) {
    std::vector<modelbuild::Model> out;
    for (const std::filesystem::path& f : zsrc::list(zoneSourceDir, "xmodels", ".xmodel_export")) {
        xmodelexport::Model src;
        std::string err;
        modelbuild::Model md;
        const std::string name = zsrc::assetName(f);
        modelbuild::BuildOptions opt;
        if (hints)
            if (auto it = hints->find(name); it != hints->end()) opt = it->second;
        if (!xmodelexport::load(f.string(), src, err) || !modelbuild::build(src, name, md, err, opt)) {
            warnings.push_back(f.filename().string() + ": " + err);
            continue;
        }
        for (const std::string& w : md.warnings) warnings.push_back(w);
        out.push_back(std::move(md));
    }
    return out;
}

ZoneSourceWeapons loadZoneSourceWeapons(const std::string& zoneSourceDir, std::vector<std::string>& warnings) {
    ZoneSourceWeapons out;
    std::vector<std::string> modelNames, animNames;
    for (const auto& f : zsrc::list(zoneSourceDir, "xmodels", ".xmodel_export")) modelNames.push_back(zsrc::assetName(f));
    for (const auto& f : zsrc::list(zoneSourceDir, "xanims", ".xanim_export")) animNames.push_back(zsrc::assetName(f));
    for (const std::filesystem::path& f : zsrc::list(zoneSourceDir, "weapons", nullptr)) {
        std::ifstream in(f, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        weapondef::Weapon w;
        std::string err;
        const std::string wname = zsrc::lower(f.filename().string());
        if (!weapondef::parse(text, wname, w, warnings, err)) {
            warnings.push_back(err);
            continue;
        }
        const std::string tag = "weapon " + wname + ": ";
        std::vector<std::string> hide;
        for (const std::string& t : w.hideTags) hide.push_back(zsrc::lower(t));
        w.hideTags = hide;
        auto resolveModel = [&](std::string& ref, bool gun, bool world, bool hands) {
            if (ref.empty()) return;
            std::string n = zsrc::resolve(ref, modelNames);
            if (n.empty() && hands)
                for (const std::string& m : modelNames)
                    if (m.rfind("viewhands", 0) == 0) {
                        n = m;
                        break;
                    }
            if (n.empty()) {
                warnings.push_back(tag + "model '" + ref + "' isn't in zone_source; referenced by name (default model unless a loaded zone has it)");
                return;
            }
            if (zsrc::lower(ref) != n) warnings.push_back(tag + "model '" + ref + "' -> zone_source " + n);
            ref = n;
            modelbuild::BuildOptions& o = out.hints[n];
            if (gun || world) {
                o.dropUntextured = true;
                for (const std::string& t : hide)
                    if (std::find(o.hideBones.begin(), o.hideBones.end(), t) == o.hideBones.end()) o.hideBones.push_back(t);
            }
            if (gun || hands) o.zeroTrans = true;
        };
        for (std::string& m : w.gunModels) resolveModel(m, true, false, false);
        for (std::string& m : w.worldModels) resolveModel(m, false, true, false);
        for (auto& [off, m] : w.defModels) resolveModel(m, false, false, off == weapondef::off::dHandModel);
        for (int a = 0; a < weapondef::kAnimCount; a++) {
            std::string& ref = w.anims[a];
            if (ref.empty()) continue;
            std::string n = zsrc::resolve(ref, animNames);
            if (n.empty()) {
                warnings.push_back(tag + "anim '" + ref + "' isn't in zone_source/xanims; left empty");
                ref.clear();
                continue;
            }
            if (zsrc::lower(ref) != n) warnings.push_back(tag + "anim '" + ref + "' -> " + n);
            ref = n;
        }
        out.weapons.push_back(std::move(w));
    }
    return out;
}

std::string mapScriptPath(const std::string& mapPath) {
    return std::filesystem::path(mapPath).replace_extension(".gsc").string();
}

bool readMapScript(const std::string& mapPath, std::string& out) {
    out.clear();
    if (mapPath.empty()) return false;
    std::ifstream f(mapScriptPath(mapPath), std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

std::string mapScriptSource(const std::string& mapName, MapTarget target) {
    // The game runs maps/[mp/]<map>::main() when the map starts; without the script it errors out.
    // SP: just the shared level loader.
    if (target == MapTarget::SP)
        return "// generated by iw4-radiant for " + mapName + "\nmain()\n{\n\tmaps\\_load::main();\n}\n";
    // Minimal MP setup: the shared map loader plus the team roles every stock map sets.
    return "// generated by iw4-radiant for " + mapName +
           "\nmain()\n{\n\tmaps\\mp\\_load::main();\n\n\tgame[\"attackers\"] = \"allies\";\n"
           "\tgame[\"defenders\"] = \"axis\";\n}\n";
}

std::vector<uint8_t> buildZoneImage(const std::vector<ZoneAsset>& assets,
                                    const std::vector<std::string>& scriptStrings) {
    ZoneWriter w;

    // XFile (40 bytes), read directly by DB_LoadXFile; size/blockSize are patched at the end.
    uint8_t xfile[sizeof(iw4::XFile)] = {};
    w.writeUntracked(xfile, sizeof(xfile));

    // XAssetList (32 bytes). Counted in TEMP at the root, per the linker convention.
    const uint32_t stringCount =
        scriptStrings.empty() ? 0 : static_cast<uint32_t>(scriptStrings.size() + 1);
    iw4::XAssetList list{};
    list.stringList.count = static_cast<int>(stringCount);
    list.stringList.strings = stringCount ? follow<const char*>() : nullptr;
    list.assetCount = static_cast<int>(assets.size());
    list.assets = assets.empty() ? nullptr : follow<iw4::XAsset>();
    w.writeStruct(list);

    // Load_ScriptStringList (0x11e930): push VIRTUAL; pointer array (align 4); each -1 string.
    w.push(zone::BLOCK_VIRTUAL);
    if (stringCount) {
        w.align(zone::kAlign4);
        w.u64(0);  // entry 0: the null script string
        for (size_t i = 0; i < scriptStrings.size(); i++) w.u64(zone::kPtrFollow);
        for (const std::string& s : scriptStrings) w.xstring(s);
    }
    w.pop();

    // Asset array (align 4, 16 bytes per XAsset), then each asset via Load_XAsset.
    w.push(zone::BLOCK_VIRTUAL);
    if (!assets.empty()) {
        w.align(zone::kAlign4);
        for (const ZoneAsset& a : assets) {
            w.u32(a.type);
            w.u32(0);
            w.u64(a.header);
        }
        for (const ZoneAsset& a : assets) loadAssetPtr(w, a.header, a.load);
    }
    w.pop();

    std::vector<uint8_t>& z = w.bytes();
    iw4::XFile hdr{};
    hdr.size = static_cast<unsigned int>(z.size() - sizeof(iw4::XFile));
    hdr.externalSize = 0;
    for (int b = 0; b < zone::BLOCK_COUNT; b++) hdr.blockSize[b] = w.blockSize(b);
    std::memcpy(z.data(), &hdr, sizeof(hdr));
    return std::move(z);
}

bool compileMapToFastfile(const Document& doc, const std::string& mapName,
                          std::vector<uint8_t>& fastfileOut, std::string& error, MapTarget target,
                          const CompileOptions& options) {
    if (mapName.empty()) {
        error = "empty map name";
        return false;
    }
    // Material library (stock definitions to embed): the caller's, or the cached one.
    std::shared_ptr<const matdef::Library> materials = options.materials;
    if (!materials && !options.cacheDir.empty()) {
        auto lib = std::make_shared<matdef::Library>();
        if (matdef::load(options.cacheDir + "/materialdefs.bin", *lib)) materials = lib;
    }
    // Unlit effect materials (additive falloff sheets): no lightmap, no sun, don't block light.
    std::set<std::string> unlit = options.light.unlitMaterials;
    if (materials)
        for (const Brush& b : doc.ents[0].brushes)
            for (const Face& f : b.faces) {
                auto it = materials->find(matdef::key(worldMaterialAssetName(f.material)));
                if (it != materials->end() && matdef::shadingFor(it->second) == matdef::Shading::FalloffAdd) {
                    std::string n = matdef::key(f.material);
                    unlit.insert(n.rfind("wc/", 0) == 0 ? n.substr(3) : n);
                }
            }
    clip::ClipMapData cm;  // effect materials are nonsolid
    if (!clip::buildClipMapData(doc, cm, error, &unlit)) return false;
    // Surface types (footstep sounds, impact effects, bullettrace "surfacetype", AI groundtype):
    // stock clipMap materials carry type << 20 in surfaceFlags (trainer: asphalt 22, concrete 5,
    // plaster 16, metal 13, wood 21); the type is the stock material's MaterialInfo.surfaceTypeBits
    // (+24) = 1 << (type - 1).
    for (clip::Material& m : cm.materials) {
        if (m.surfaceType >= 0) {  // the brush's own (editor: Brush > Surface type)
            m.surfaceFlags = (m.surfaceFlags & ~0x3F00000u) | (static_cast<uint32_t>(m.surfaceType) << 20);
            continue;
        }
        if (materials) {
            auto it = materials->find(matdef::key(worldMaterialAssetName(m.name)));
            if (it == materials->end() || it->second.header.size() < 28) continue;
            uint32_t bits;
            std::memcpy(&bits, it->second.header.data() + 24, 4);
            if (!bits) continue;
            uint32_t type = 1;
            while (!(bits & 1)) bits >>= 1, type++;
            m.surfaceFlags = (m.surfaceFlags & ~0x3F00000u) | (type << 20);
        }
    }
    // Lighting: reuse the caller's bake, bake now, or (bakeLighting off) uniform placeholder light.
    light::Bake localBake;
    const light::Bake* bake = options.prebaked;
    if (!bake && options.bakeLighting) {
        light::Settings ls = options.light;
        ls.unlitMaterials = unlit;
        if (!light::bake(doc, ls, localBake, error, options.progress)) {
            error = "lighting: " + error;
            return false;
        }
        bake = &localBake;
    }
    // Sky: copy the stock sky shader set once (cached) if the map has sky faces.
    std::shared_ptr<const sky::Source> skySrc;
    bool wantSky = false;
    for (const Brush& b : doc.ents[0].brushes)
        for (const Face& f : b.faces)
            if (light::isSkyMaterial(f.material)) wantSky = true;
    if (wantSky) {
        auto s = std::make_shared<sky::Source>();
        std::string skyErr;
        if (sky::load(options.gameDir, options.cacheDir, *s, skyErr))
            skySrc = s;
        else if (options.warnings)
            options.warnings->push_back("sky left out: " + skyErr);
    }
    gfx::GfxWorldData gw;
    if (!gfx::buildGfxWorldData(doc, gw, error, bake, skySrc != nullptr, &unlit)) return false;
    {
        // Material sort keys (MaterialInfo.sortKey, byte 9 of the struct) put surfaces in the
        // right dpvs range: embedded materials carry their stock key, the sky its own (2),
        // by-name references are lit opaque world materials (1).
        std::vector<uint8_t> keys(gw.materials.size(), 1);
        for (size_t i = 0; i < gw.materials.size(); i++) {
            if (static_cast<int>(i) == gw.skyMaterial && skySrc) {
                keys[i] = skySrc->material.size() > 9 ? skySrc->material[9] : 2;
            } else if (materials) {
                auto it = materials->find(matdef::key(worldMaterialAssetName(gw.materials[i])));
                if (it != materials->end() && matdef::shadingFor(it->second) != matdef::Shading::Unsupported &&
                    it->second.header.size() > 9)
                    keys[i] = it->second.header[9];
            }
        }
        gfx::sortSurfaces(gw, keys);
    }
    // World materials: embedded in full from the stock definitions; any we don't know stay
    // by-name references (which the game resolves to its default material).
    if (options.warnings) {
        std::string missing;
        int count = 0;
        for (size_t i = 0; i < gw.materials.size(); i++) {
            if (static_cast<int>(i) == gw.skyMaterial) continue;
            const std::string full = worldMaterialAssetName(gw.materials[i]);
            bool known = false;
            if (materials) {
                auto it = materials->find(matdef::key(full));
                known = it != materials->end() && matdef::shadingFor(it->second) != matdef::Shading::Unsupported;
            }
            if (!known) {
                if (count++ < 8) missing += (missing.empty() ? "" : ", ") + full;
            }
        }
        if (count)
            options.warnings->push_back(std::to_string(count) + " material(s) not embedded (no stock definition" +
                                        std::string(materials ? "" : "; the material library isn't built yet") +
                                        "), they'll show as the default material: " + missing);
    }
    // Every world asset is named after the bsp path the game loads ("maps/mp/<map>.d3dbsp" for
    // MP, "maps/<map>.d3dbsp" for SP).
    const bool sp = target == MapTarget::SP;
    const std::string bsp = mapAssetDir(target) + mapName + ".d3dbsp";
    // Asset order follows stock map zones: ComWorld, FxWorld, GfxWorld, GameWorld, then clipMap
    // last with the entity lump (MapEnts) inline through clipMap_t.mapEnts.
    auto shared = std::make_shared<SharedWorld>();
    // SP and MP clipMaps are the same struct and loader; only the asset type differs.
    // misc_model entities using a zone_source model become script_models (the game spawns those;
    // misc_models are compile-time static models, which this compiler doesn't bake yet).
    Document entDoc;
    entDoc.ents = doc.ents;
    // SP path nodes go into the GameWorldSp path data, not the entity lump.
    std::shared_ptr<const pathnodes::Graph> pathGraph;
    if (sp) pathGraph = std::make_shared<const pathnodes::Graph>(pathnodes::build(doc, cm, options.warnings));
    entDoc.ents.erase(std::remove_if(entDoc.ents.begin(), entDoc.ents.end(),
                                     [](const Entity& e) { return pathnodes::isPathNodeClass(e.classname()); }),
                      entDoc.ents.end());
    // Effects: zone_source/fx compiled into the zone; fx_origin entities become a generated script
    // (maps/[mp/]<map>_fx.gsc) that the map script's main() calls first.
    std::vector<std::string> fxWarnings;
    ZoneSourceEffects zfx = loadZoneSourceEffects(options.zoneSourceDir, options.gameDir, options.cacheDir, target, fxWarnings);
    if (options.warnings)
        for (const std::string& x : fxWarnings) options.warnings->push_back(x);
    std::vector<FxPlacement> placements = fxPlacements(entDoc, zfx.looping, options.warnings);
    // fx_origin -> the generated script; iw4r_terrain is editor data (its brushes are world brushes);
    // trigger_grade volumes only drive the post pass (written to the .volumetric file).
    entDoc.ents.erase(std::remove_if(entDoc.ents.begin(), entDoc.ents.end(),
                                     [](const Entity& e) {
                                         return e.classname() == "fx_origin" || e.classname() == "iw4r_terrain" || e.classname() == "trigger_grade" ||
                                                e.classname() == "trigger_weather";
                                     }),
                      entDoc.ents.end());
    const std::string fxScriptName = mapAssetDir(target) + mapName + "_fx";
    std::string fxCall;
    if (!placements.empty()) {
        std::string module = fxScriptName;
        std::replace(module.begin(), module.end(), '/', '\\');
        fxCall = module + "::main();";
    }
    auto mapScriptFor = [&](const std::string& level) {
        std::string s = options.mapScript.empty() ? mapScriptSource(level, target) : options.mapScript;
        if (!fxCall.empty()) {
            std::string withFx = injectIntoMain(s, fxCall);
            if (withFx == s && s.find(fxCall) == std::string::npos && options.warnings)
                options.warnings->push_back("fx: couldn't find main() in the map script; call " + fxCall + " yourself");
            s = withFx;
        }
        return s;
    };
    // Script strings (bone names, notetracks, node names, ...); index 0 is the null string.
    std::vector<std::string> scriptStrings;
    auto scriptIndex = [&](const std::string& str) {
        auto at = std::find(scriptStrings.begin(), scriptStrings.end(), str);
        if (at == scriptStrings.end()) at = scriptStrings.insert(scriptStrings.end(), str);
        return static_cast<uint16_t>(1 + (at - scriptStrings.begin()));
    };
    std::vector<std::array<uint16_t, 4>> nodeStrings;
    if (pathGraph)
        for (const pathnodes::Node& nd : pathGraph->nodes) {
            auto idx = [&](const std::string& s) { return s.empty() ? uint16_t(0) : scriptIndex(s); };
            nodeStrings.push_back({idx(nd.targetname), idx(nd.linkname), idx(nd.noteworthy), idx(nd.target)});
        }
    {
        std::vector<std::string> unused;
        std::set<std::string> custom;
        for (const modelbuild::Model& md : loadZoneSourceModels(options.zoneSourceDir, unused)) custom.insert(matdef::key(md.name));
        for (Entity& e : entDoc.ents)
            if (e.classname() == "misc_model")
                if (const std::string* mdl = e.find("model"); mdl && custom.count(matdef::key(*mdl))) e.set("classname", "script_model");
    }
    // brush triggers (trigger_multiple...): "?N" trigger models in the entity lump
    clip::buildTriggers(entDoc, cm, options.warnings);
    if (!cm.triggerModels.empty() && options.warnings)
        options.warnings->push_back("triggers: " + std::to_string(cm.triggerModels.size()) + " brush trigger(s), " +
                                    std::to_string(cm.triggerHulls.size()) + " hull(s)");
    if (gw.skyMaterial >= 0) customSkyImage(options.zoneSourceDir, gw.skyImage, gw.customSky, options.warnings);
    ZoneAsset clipMap = buildClipMap(bsp, cm, documentToEntityString(entDoc), shared, sp);
    if (sp) clipMap.type = IW4_CLIPMAP_SP;
    std::vector<ZoneAsset> assets = {
        buildComWorld(bsp, true, bake ? &bake->env : nullptr, gw.primaries),
        buildFxWorld(bsp),
        buildGfxWorld(bsp, mapName, gw, cm, shared, skySrc, materials),
        sp ? buildGameWorldSp(bsp, pathGraph, nodeStrings) : buildGameWorldMp(bsp),
        std::move(clipMap),
        buildCompressedRawFile(mapAssetDir(target) + mapName + ".gsc", mapScriptFor(mapName)),
    };
    if (!placements.empty()) {
        assets.push_back(buildCompressedRawFile(fxScriptName + ".gsc", fxPlacementScript(placements)));
        if (options.warnings)
            options.warnings->push_back("fx: " + std::to_string(placements.size()) + " fx_origin(s) -> " + fxScriptName +
                                        ".gsc (called first in the map script's main())");
    }
    if (!options.addonLevel.empty() && sp) {
        // Weapon-pack level so_w_<map>: its script (same source as the map's).
        const std::string& level = options.addonLevel;
        // No add-on entity lump here: the pack carries one (pool size 1, a second is fatal), and
        // deployWeaponPack renames it to maps/<level>.mapents.
        assets.push_back(buildCompressedRawFile("maps/" + level + ".gsc", mapScriptFor(level)));
    }
    // zone_source/scripts/<path>.gsc|.csv -> rawfile <path> (scripts/maps/_zombies.gsc ->
    // maps/_zombies.gsc; scripts/aitype/zombie.gsc is what an actor_zombie spawner loads).
    if (!options.zoneSourceDir.empty()) {
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(options.zoneSourceDir) / "scripts";
        std::error_code ec;
        if (fs::is_directory(dir, ec))
            for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = zsrc::lower(it->path().extension().string());
                if (ext != ".gsc" && ext != ".csv") continue;
                const std::string rel = zsrc::lower(fs::relative(it->path(), dir, ec).generic_string());
                if (rel == mapAssetDir(target) + mapName + ".gsc") continue;  // the map script has its own slot
                std::ifstream in(it->path(), std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                assets.push_back(buildCompressedRawFile(rel, text));
                if (options.warnings) options.warnings->push_back("script " + rel + " (zone_source)");
            }
    }
    // zone_source models: XModel assets (bone names go in the zone's script strings).
    {
        std::vector<std::string> mw;
        ZoneSourceWeapons zw = loadZoneSourceWeapons(options.zoneSourceDir, mw);
        std::vector<modelbuild::Model> models = loadZoneSourceModels(options.zoneSourceDir, mw, &zw.hints);
        if (options.warnings)
            for (const std::string& x : mw) options.warnings->push_back(x);
        std::vector<ZoneAsset> modelAssets;
        for (const modelbuild::Model& md : models) {
            std::vector<uint16_t> boneIdx;
            for (const modelbuild::Bone& b : md.bones) boneIdx.push_back(scriptIndex(b.name));
            modelAssets.push_back(
                buildXModel(md, boneIdx, planModelMaterials(md, materials.get(), options.zoneSourceDir, options.warnings)));
        }
        // zone_source/xanims/*.xanim_export -> XAnimParts (parents from the zone_source models that
        // have the bones, best match first), plus animtrees/custom.atr listing them for
        // #using_animtree("custom").
        std::vector<std::string> animNames;
        for (const std::filesystem::path& f : zsrc::list(options.zoneSourceDir, "xanims", ".xanim_export")) {
            xanimexport::Anim src;
            std::string aerr;
            if (!xanimexport::load(f.string(), src, aerr)) {
                if (options.warnings) options.warnings->push_back(f.filename().string() + ": " + aerr);
                continue;
            }
            std::vector<std::pair<size_t, const modelbuild::Model*>> ranked;
            for (const modelbuild::Model& md : models) {
                size_t hits = 0;
                for (const std::string& part : src.parts)
                    for (const modelbuild::Bone& b : md.bones) hits += b.name == part;
                if (hits) ranked.push_back({hits, &md});
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<const modelbuild::Model*> skels;
            for (const auto& r : ranked) skels.push_back(r.second);
            if (skels.empty() && options.warnings)
                options.warnings->push_back(f.filename().string() + ": no zone_source model has its bones; "
                                            "posing it as if every bone were a root");
            const std::string stem = zsrc::assetName(f);
            const bool loop = stem.size() > 5 && (stem.compare(stem.size() - 5, 5, "_loop") == 0 ||
                                                  stem.compare(stem.size() - 5, 5, "_idle") == 0);
            animbuild::Anim an;
            if (!animbuild::build(src, stem, skels, loop, an, aerr)) {
                if (options.warnings) options.warnings->push_back(f.filename().string() + ": " + aerr);
                continue;
            }
            std::vector<uint16_t> nameIdx;
            for (const std::string& part : an.names) nameIdx.push_back(scriptIndex(part));
            std::vector<uint16_t> noteIdx;
            for (const auto& n : an.notes) noteIdx.push_back(scriptIndex(n.first));
            modelAssets.push_back(buildXAnimParts(an, nameIdx, noteIdx));
            animNames.push_back(stem);
        }
        // Effects (after the models they may use, before the weapons that name them).
        modelAssets.insert(modelAssets.end(), zfx.assets.begin(), zfx.assets.end());
        // zone_source/weapons/<name> -> WeaponCompleteDef (after the models it references).
        for (const weapondef::Weapon& w : zw.weapons) {
            std::vector<uint16_t> hide;
            for (const std::string& t : w.hideTags) hide.push_back(scriptIndex(t));
            std::array<std::vector<uint16_t>, 4> notes;
            for (const auto& [k, v] : w.noteSounds) notes[0].push_back(scriptIndex(k)), notes[1].push_back(scriptIndex(v));
            for (const auto& [k, v] : w.noteRumbles) notes[2].push_back(scriptIndex(k)), notes[3].push_back(scriptIndex(v));
            modelAssets.push_back(buildWeapon(w, hide, notes));
            if (options.warnings) options.warnings->push_back("weapon " + w.name + " compiled");
        }
        assets.insert(assets.begin(), modelAssets.begin(), modelAssets.end());  // models/anims load first
        if (!animNames.empty()) {
            std::string tree;
            for (const std::string& a : animNames) tree += a + "\n";
            assets.push_back(buildCompressedRawFile("animtrees/custom.atr", tree));
        }
    }
    std::vector<uint8_t> image = buildZoneImage(assets, scriptStrings);

    Fastfile ff;
    defaultPrefix(ff.prefix);
    ff.zone = std::move(image);
    return writeFastfile(ff, fastfileOut, error);
}
