#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal read-only access to another process's memory, for dumping assets straight out of the
// running game (iw4mp.exe) with ReadProcessMemory. Nothing here writes to the target or injects
// code: it opens the process for VM read only, finds the main module, and reads structs/arrays
// out of its address space. Pointer fields in those structs are addresses in the *target's*
// space, so follow them with another read, never by dereferencing locally.
namespace live {

class ProcessReader {
public:
    ~ProcessReader() { detach(); }
    // Attaches to the first process whose image name matches exeName (e.g. "iw4mp.exe").
    bool attach(const char* exeName, std::string& error);
    void detach();
    bool attached() const { return proc_ != nullptr; }

    uintptr_t base() const { return base_; }         // runtime load address of the main module
    size_t moduleSize() const { return size_; }
    // A global's runtime address from its image-relative address (RVA) in the IDA database:
    // the .i64 is based at 0, so runtime = moduleBase + rva. Verified against IW4Controller VAs.
    uintptr_t rva(uintptr_t imageRva) const { return base_ + imageRva; }

    bool readRaw(uintptr_t addr, void* out, size_t n) const;
    template <class T>
    bool read(uintptr_t addr, T& out) const {
        return readRaw(addr, &out, sizeof(T));
    }
    template <class T>
    T readOr(uintptr_t addr, T def = T{}) const {
        T v{};
        return read(addr, v) ? v : def;
    }
    uint64_t readPtr(uintptr_t addr) const { return readOr<uint64_t>(addr); }
    // Reads count Ts into out (resized). Returns false if the whole block couldn't be read.
    template <class T>
    bool readVec(uintptr_t addr, size_t count, std::vector<T>& out) const {
        out.resize(count);
        return count == 0 || readRaw(addr, out.data(), count * sizeof(T));
    }
    // Reads a NUL-terminated string (bounded); empty on a bad pointer.
    std::string readString(uintptr_t addr, size_t maxLen = 512) const;

private:
    void* proc_ = nullptr;  // HANDLE
    uintptr_t base_ = 0;
    size_t size_ = 0;
};

}  // namespace live
