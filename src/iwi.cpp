#include "iwi.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kHeaderSize = 32;

bool isBlock(uint8_t f) { return f == 11 || f == 12 || f == 13; }

size_t levelSize(uint8_t format, int w, int h) {
    if (isBlock(format)) {
        size_t blocks = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4);
        return blocks * (format == 11 ? 8 : 16);
    }
    int bpp = format == 1 ? 4 : format == 2 ? 3 : format == 3 ? 2 : 1;
    return static_cast<size_t>(w) * h * bpp;
}

void expandToBgra(uint8_t format, const uint8_t* src, int w, int h, std::vector<uint8_t>& dst) {
    size_t n = static_cast<size_t>(w) * h;
    dst.resize(n * 4);
    for (size_t i = 0; i < n; i++) {
        uint8_t* d = &dst[i * 4];
        switch (format) {
            case 1: std::memcpy(d, src + i * 4, 4); break;
            case 2: std::memcpy(d, src + i * 3, 3); d[3] = 255; break;
            case 3: d[0] = d[1] = d[2] = src[i * 2]; d[3] = src[i * 2 + 1]; break;
            case 4: d[0] = d[1] = d[2] = src[i]; d[3] = 255; break;
            default: d[0] = d[1] = d[2] = 255; d[3] = src[i]; break;
        }
    }
}
}  // namespace

bool parseIwi(const uint8_t* data, size_t size, int maxDim, IwiImage& out, std::string& error) {
    if (size < kHeaderSize || std::memcmp(data, "IWi", 3) != 0) {
        error = "not an IWI file";
        return false;
    }
    // Version 8 (MW2): format at 8, dims at 10, 32-byte header. Version 13 (Black Ops): format at 4,
    // flags at 5, dims at 6, 8 picmip sizes, 48-byte header. Both store the smallest level first.
    const uint8_t version = data[3];
    if (version != 8 && version != 13) {
        error = "unsupported IWI version " + std::to_string(version);
        return false;
    }
    const size_t headerSize = version == 13 ? 48 : kHeaderSize;
    if (size < headerSize) {
        error = "not an IWI file";
        return false;
    }
    uint8_t format = version == 13 ? data[4] : data[8];
    if (!(format >= 1 && format <= 5) && !isBlock(format)) {
        error = "unsupported IWI format " + std::to_string(format);
        return false;
    }
    uint16_t dims[3];
    std::memcpy(dims, data + (version == 13 ? 6 : 10), sizeof dims);
    int w = dims[0], h = dims[1];
    if (w <= 0 || h <= 0 || dims[2] != 1) {
        error = "unsupported IWI dimensions";
        return false;
    }

    size_t payload = size - headerSize;
    std::vector<std::pair<int, int>> chain;
    size_t fullChain = 0;
    for (int mw = w, mh = h;;) {
        chain.push_back({mw, mh});
        fullChain += levelSize(format, mw, mh);
        if (mw == 1 && mh == 1) break;
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
    }
    // Anything that isn't a complete single chain (no mips, cubemaps) uses only the top level.
    if (fullChain != payload) chain.resize(1);
    if (levelSize(format, w, h) > payload) {
        error = "IWI data is truncated";
        return false;
    }

    out = {};
    out.format = isBlock(format) ? static_cast<IwiFormat>(format) : IwiFormat::Rgba;
    size_t end = size;
    for (auto [mw, mh] : chain) {
        size_t bytes = levelSize(format, mw, mh);
        const uint8_t* src = data + end - bytes;
        end -= bytes;
        bool tooBig = std::max(mw, mh) > maxDim;
        if (tooBig && (mw != chain.back().first || mh != chain.back().second)) continue;
        IwiLevel level;
        level.width = mw;
        level.height = mh;
        if (isBlock(format)) {
            level.rowPitch = static_cast<int>(((mw + 3) / 4) * (format == 11 ? 8 : 16));
            level.data.assign(src, src + bytes);
        } else {
            level.rowPitch = mw * 4;
            expandToBgra(format, src, mw, mh, level.data);
        }
        out.levels.push_back(std::move(level));
    }
    return !out.levels.empty();
}
