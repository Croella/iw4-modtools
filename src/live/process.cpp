#include "live/process.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace live {
namespace {
// Case-insensitive match of a module/process image name.
bool sameName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

DWORD findPid(const char* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (sameName(pe.szExeFile, exeName)) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Main-module base and size for the process (the .exe image itself, the first module listed).
bool mainModule(DWORD pid, const char* exeName, uintptr_t& base, size_t& size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32First(snap, &me)) {
        do {
            if (sameName(me.szModule, exeName)) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                size = me.modBaseSize;
                found = true;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}
}  // namespace

bool ProcessReader::attach(const char* exeName, std::string& error) {
    detach();
    DWORD pid = findPid(exeName);
    if (!pid) {
        error = std::string(exeName) + " isn't running";
        return false;
    }
    if (!mainModule(pid, exeName, base_, size_)) {
        error = "couldn't find the main module of " + std::string(exeName) +
                " (try running the editor as administrator)";
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        error = "couldn't open " + std::string(exeName) +
                " for reading (code " + std::to_string(GetLastError()) +
                "; try running the editor as administrator)";
        base_ = size_ = 0;
        return false;
    }
    proc_ = h;
    return true;
}

void ProcessReader::detach() {
    if (proc_) CloseHandle(static_cast<HANDLE>(proc_));
    proc_ = nullptr;
    base_ = 0;
    size_ = 0;
}

bool ProcessReader::readRaw(uintptr_t addr, void* out, size_t n) const {
    if (!proc_ || !addr) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(static_cast<HANDLE>(proc_), reinterpret_cast<LPCVOID>(addr), out, n, &got) &&
           got == n;
}

std::string ProcessReader::readString(uintptr_t addr, size_t maxLen) const {
    if (!proc_ || !addr) return {};
    std::string s;
    char buf[64];
    while (s.size() < maxLen) {
        size_t want = std::min(sizeof(buf), maxLen - s.size());
        if (!readRaw(addr + s.size(), buf, want)) break;  // hit an unmapped page
        for (size_t i = 0; i < want; i++) {
            if (buf[i] == 0) return s;
            s.push_back(buf[i]);
        }
    }
    return s;
}

}  // namespace live
