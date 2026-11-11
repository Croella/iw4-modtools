#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A zone stream writer that mirrors the x64 iw4mp loader's stream model exactly, so the
// XFile.blockSize[] table falls out of the same calls that emit the bytes.
//
// Reversed from iw4mp_2013.exe (image-relative addresses in the .i64):
//   DB_LoadXFile            0x117d40   reads XFile(40), XAssetList(32) directly, then:
//                                      push(VIRTUAL) ScriptStringList pop, push(VIRTUAL) assets pop
//   DB_PushStreamPos        0x132140   save {block, pos}; switch block
//   DB_PopStreamPos         0x132100   if the block being left is TEMP, rewind TEMP to the saved pos
//   DB_AllocStreamPos       0x131fd0   pos = (pos + mask) & ~mask      (mask 0 = byte, 3 = 4, 15 = 16)
//   Load_Stream             0x1322a0   read n file bytes to pos; pos += n   (no bounds check!)
//                                      in RUNTIME (block 2) it memsets instead: no file bytes
//   DB_InsertPointer        0x132080   in VIRTUAL: align 8, reserve an 8-byte slot (for -2 headers)
//   DB_ConvertOffsetToPtr   0x132270   block = (p-1) >> 28, offset = (p-1) & 0x0FFFFFFF
//   DB_AllocXZoneMemory     0x12a790   allocates each blockSize[i], page-rounded (0x1000)
//   Load_<Asset>Ptr         e.g. 0x118a20 (ComWorld): push(TEMP), align 4, [-2: InsertPointer],
//                           Load_<Asset> (struct into TEMP, sub-data under push(VIRTUAL)), DB_Add*
//
// So: each asset's top-level struct lives in TEMP and is copied out by DB_Add*; TEMP rewinds
// after every asset, making blockSize[TEMP] the high-water mark, not a sum. Sub-data goes to
// VIRTUAL with per-field alignment. Because Load_Stream never bounds-checks, an undersized
// blockSize corrupts memory silently, so these numbers must be exact or generous.
//
// Linker convention (matches stock zones byte-for-byte, e.g. patch_code_pre_gfx.ff
// blockSize = {TEMP 56, VIRTUAL 1154} for two rawfiles): the 32-byte XAssetList is counted in
// TEMP at the root, so blockSize[TEMP] = 32 + the largest per-asset TEMP use.
namespace zone {

enum Block : int {
    BLOCK_TEMP = 0,
    BLOCK_PHYSICAL = 1,
    BLOCK_RUNTIME = 2,
    BLOCK_VIRTUAL = 3,
    BLOCK_LARGE = 4,
    BLOCK_CALLBACK = 5,
    BLOCK_VERTEX = 6,
    BLOCK_INDEX = 7,
    BLOCK_COUNT = 8,
};

// On-disk pointer values.
constexpr uint64_t kPtrFollow = ~0ull;  // -1: pointee follows inline in the stream
constexpr uint64_t kPtrInsert = ~1ull;  // -2: like -1, and reserve a slot others can reference

// Alignment masks as passed to DB_AllocStreamPos.
constexpr uint32_t kAlign1 = 0, kAlign2 = 1, kAlign4 = 3, kAlign8 = 7, kAlign16 = 15;

// A back-reference to already-loaded data: the loader resolves it to blockBase[block] + offset.
constexpr uint64_t offsetPointer(int block, uint32_t offset) {
    return ((static_cast<uint64_t>(block) << 28) | (offset & 0x0FFFFFFFu)) + 1;
}

class ZoneWriter {
public:
    // Bytes that are read directly, outside any stream block (the XFile header).
    void writeUntracked(const void* p, size_t n);

    void push(int block);  // DB_PushStreamPos
    void pop();            // DB_PopStreamPos
    uint32_t align(uint32_t mask);  // DB_AllocStreamPos; returns the aligned position
    uint64_t insertPointer();       // DB_InsertPointer; returns a back-reference to the slot

    // Load_Stream: bytes land at the current block's position. In RUNTIME the loader zero-fills
    // instead of reading, so only the position advances (pass zeros).
    void write(const void* p, size_t n);
    template <class T>
    void writeStruct(const T& v) { write(&v, sizeof(T)); }
    void u8(uint8_t v) { write(&v, 1); }
    void u32(uint32_t v) { write(&v, 4); }
    void i32(int32_t v) { write(&v, 4); }
    void u64(uint64_t v) { write(&v, 8); }
    void zeros(size_t n);
    void cstr(const std::string& s);     // chars + NUL, no alignment
    void xstring(const std::string& s);  // Load_XString: align(byte) then cstr

    int block() const { return cur_; }
    uint32_t pos() const { return pos_[cur_]; }
    // Back-reference to the current position (call after align, before writing the data).
    uint64_t here() const { return offsetPointer(cur_, pos_[cur_]); }
    uint32_t blockSize(int b) const { return high_[b]; }
    size_t depth() const { return stack_.size(); }

    std::vector<uint8_t>& bytes() { return bytes_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    void setIndex(int block);  // DB_SetStreamIndex
    void advance(size_t n);

    struct Saved {
        int block;
        uint32_t pos;
    };
    std::vector<uint8_t> bytes_;
    int cur_ = BLOCK_TEMP;
    uint32_t pos_[BLOCK_COUNT] = {};
    uint32_t high_[BLOCK_COUNT] = {};
    std::vector<Saved> stack_;
};

}  // namespace zone
