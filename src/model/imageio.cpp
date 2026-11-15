#include "model/imageio.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <miniz.h>

namespace imageio {
namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool loadPng(const std::vector<uint8_t>& d, Rgba& out, std::string& error) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (d.size() < 8 || std::memcmp(d.data(), sig, 8) != 0) {
        error = "not a PNG";
        return false;
    }
    uint32_t w = 0, h = 0;
    uint8_t depth = 0, type = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    for (size_t p = 8; p + 12 <= d.size();) {
        uint32_t len = be32(&d[p]);
        if (p + 12 + len > d.size()) break;
        const char* tag = reinterpret_cast<const char*>(&d[p + 4]);
        const uint8_t* body = &d[p + 8];
        if (!std::memcmp(tag, "IHDR", 4) && len >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            type = body[9];
            interlace = body[12];
        } else if (!std::memcmp(tag, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!std::memcmp(tag, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!std::memcmp(tag, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(tag, "IEND", 4)) {
            break;
        }
        p += 12 + len;
    }
    if (!w || !h || w > 8192 || h > 8192) {
        error = "PNG: bad size";
        return false;
    }
    if (depth != 8 || interlace) {
        error = "PNG: only 8-bit, non-interlaced images are supported";
        return false;
    }
    int ch = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (!ch) {
        error = "PNG: unsupported color type";
        return false;
    }
    size_t rawLen = 0;
    void* raw = tinfl_decompress_mem_to_heap(idat.data(), idat.size(), &rawLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!raw) {
        error = "PNG: bad image data";
        return false;
    }
    const size_t stride = size_t(w) * ch;
    std::vector<uint8_t> img(stride * h);
    const uint8_t* src = static_cast<const uint8_t*>(raw);
    bool ok = rawLen >= (stride + 1) * h;
    for (uint32_t y = 0; ok && y < h; y++) {
        uint8_t filter = src[y * (stride + 1)];
        const uint8_t* in = src + y * (stride + 1) + 1;
        uint8_t* row = &img[y * stride];
        const uint8_t* prev = y ? &img[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= size_t(ch) ? row[x - ch] : 0, b = prev ? prev[x] : 0, c = (prev && x >= size_t(ch)) ? prev[x - ch] : 0;
            int v = in[x];
            switch (filter) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: {
                    int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            row[x] = static_cast<uint8_t>(v);
        }
    }
    mz_free(raw);
    if (!ok) {
        error = "PNG: truncated image data";
        return false;
    }
    out.width = int(w);
    out.height = int(h);
    out.pixels.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        uint8_t* o = &out.pixels[i * 4];
        const uint8_t* s = &img[i * ch];
        switch (type) {
            case 0: o[0] = o[1] = o[2] = s[0], o[3] = 255; break;
            case 4: o[0] = o[1] = o[2] = s[0], o[3] = s[1]; break;
            case 2: o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = 255; break;
            case 6: std::memcpy(o, s, 4); break;
            case 3: {
                size_t k = s[0];
                o[0] = k * 3 + 2 < palette.size() ? palette[k * 3] : 0;
                o[1] = k * 3 + 2 < palette.size() ? palette[k * 3 + 1] : 0;
                o[2] = k * 3 + 2 < palette.size() ? palette[k * 3 + 2] : 0;
                o[3] = k < trns.size() ? trns[k] : 255;
                break;
            }
        }
    }
    return true;
}

bool loadTga(const std::vector<uint8_t>& d, Rgba& out, std::string& error) {
    if (d.size() < 18) {
        error = "not a TGA";
        return false;
    }
    const uint8_t idLen = d[0], cmap = d[1], type = d[2], bpp = d[16], desc = d[17];
    const int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8);
    if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32) || !w || !h) {
        error = "TGA: only 24/32-bit truecolor (uncompressed or RLE) is supported";
        return false;
    }
    const int bytes = bpp / 8;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    size_t p = 18 + idLen, n = 0, total = size_t(w) * h;
    auto put = [&](const uint8_t* s) {
        uint8_t* o = &px[n * 4];
        o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = bytes == 4 ? s[3] : 255;
        n++;
    };
    while (n < total) {
        if (type == 2) {
            if (p + bytes > d.size()) break;
            put(&d[p]);
            p += bytes;
        } else {
            if (p >= d.size()) break;
            uint8_t hdr = d[p++];
            int count = (hdr & 0x7F) + 1;
            if (hdr & 0x80) {
                if (p + bytes > d.size()) break;
                for (int i = 0; i < count && n < total; i++) put(&d[p]);
                p += bytes;
            } else {
                for (int i = 0; i < count && n < total; i++) {
                    if (p + bytes > d.size()) break;
                    put(&d[p]);
                    p += bytes;
                }
            }
        }
    }
    if (n < total) {
        error = "TGA: truncated";
        return false;
    }
    out.width = w;
    out.height = h;
    out.pixels.resize(px.size());
    const bool topDown = (desc & 0x20) != 0;
    for (int y = 0; y < h; y++)
        std::memcpy(&out.pixels[size_t(y) * w * 4], &px[size_t(topDown ? y : h - 1 - y) * w * 4], size_t(w) * 4);
    return true;
}

int pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

}  // namespace

bool savePng(const std::string& path, const Rgba& img, std::string& error) {
    if (img.width <= 0 || img.height <= 0 || img.pixels.size() < 4ull * img.width * img.height) {
        error = "empty image";
        return false;
    }
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(img.pixels.data(), img.width, img.height, 4, &size);
    if (!png) {
        error = "PNG encoding failed";
        return false;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(png), static_cast<std::streamsize>(size));
    mz_free(png);
    if (!f) {
        error = "can't write " + path;
        return false;
    }
    return true;
}

bool load(const std::string& path, Rgba& out, std::string& error) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) {
        error = "can't read " + path;
        return false;
    }
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".tga") return loadTga(d, out, error);
    return loadPng(d, out, error);
}

void toPowerOfTwo(Rgba& img) {
    int w = pow2(img.width), h = pow2(img.height);
    if (w == img.width && h == img.height) return;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = x * img.width / w, sy = y * img.height / h;
            std::memcpy(&px[(size_t(y) * w + x) * 4], &img.pixels[(size_t(sy) * img.width + sx) * 4], 4);
        }
    img.width = w;
    img.height = h;
    img.pixels = std::move(px);
}

std::vector<uint8_t> bgraMipChain(const Rgba& img, int& levels) {
    std::vector<uint8_t> out;
    Rgba cur = img;
    levels = 0;
    for (;;) {
        for (size_t i = 0; i < size_t(cur.width) * cur.height; i++) {
            const uint8_t* s = &cur.pixels[i * 4];
            out.push_back(s[2]);
            out.push_back(s[1]);
            out.push_back(s[0]);
            out.push_back(s[3]);
        }
        levels++;
        if (cur.width == 1 && cur.height == 1) break;
        Rgba next;
        next.width = std::max(1, cur.width / 2);
        next.height = std::max(1, cur.height / 2);
        next.pixels.resize(size_t(next.width) * next.height * 4);
        for (int y = 0; y < next.height; y++)
            for (int x = 0; x < next.width; x++)
                for (int k = 0; k < 4; k++) {
                    int sum = 0, n = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int sx = std::min(cur.width - 1, x * 2 + dx), sy = std::min(cur.height - 1, y * 2 + dy);
                            sum += cur.pixels[(size_t(sy) * cur.width + sx) * 4 + k];
                            n++;
                        }
                    next.pixels[(size_t(y) * next.width + x) * 4 + k] = static_cast<uint8_t>(sum / n);
                }
        cur = std::move(next);
    }
    return out;
}

}  // namespace imageio
