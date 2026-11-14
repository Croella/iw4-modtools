#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Loads source textures (zone_source/images) for embedding in compiled maps: PNG (8-bit gray,
// gray+alpha, RGB, RGBA, palette; non-interlaced) and TGA (uncompressed or RLE, 24/32 bit).
namespace imageio {

struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGBA8, rows top to bottom
};

bool load(const std::string& path, Rgba& out, std::string& error);

// Nearest-neighbour resize to the next power of two on each side (no-op if already).
void toPowerOfTwo(Rgba& img);

// A8R8G8B8 (B, G, R, A bytes) mip chain, largest first, down to 1x1.
std::vector<uint8_t> bgraMipChain(const Rgba& img, int& levels);

}  // namespace imageio
