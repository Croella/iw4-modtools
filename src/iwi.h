#pragma once
#include <cstdint>
#include <string>
#include <vector>

// IWI version 8 textures. Layout confirmed against this install's IWD files:
//   0  'I' 'W' 'i' 8
//   4  u32 flags
//   8  u8  format
//   10 u16 width, height, depth
//   16 u32 fileSizeForPicmip[4]
//   32 mip data, smallest level first, so the largest level ends the file
enum class IwiFormat : uint8_t { Rgba = 1, Rgb = 2, LumAlpha = 3, Lum = 4, Alpha = 5, Dxt1 = 11, Dxt3 = 12, Dxt5 = 13 };

struct IwiLevel {
    int width = 0, height = 0;
    int rowPitch = 0;
    std::vector<uint8_t> data;
};

struct IwiImage {
    IwiFormat format = IwiFormat::Rgba;  // uncompressed sources are expanded to Rgba (BGRA byte order)
    std::vector<IwiLevel> levels;        // largest first
};

// maxDim drops top levels until the image fits, keeping GPU memory bounded.
bool parseIwi(const uint8_t* data, size_t size, int maxDim, IwiImage& out, std::string& error);
