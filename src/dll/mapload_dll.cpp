// iw4r_mapload.dll — a tiny injectable loader so the editor's compiled maps can be devmap'd.
//
// The game's fastfile loader already accepts the editor's unsigned IWffu100 zones; the only thing
// missing for a custom map is that the game opens "<zone path>\mp_<name>.ff" from its own install,
// not from wherever the editor deployed it. This DLL redirects exactly those opens: it hooks
// CreateFileA/W and, when the game opens an "*.ff" whose basename also exists in the editor's
// deploy folder, serves it from there instead. Stock zones (not present in the deploy folder) are
// left completely alone, so nothing else about the game changes. No signatures, memory, or game
// code are touched — only file opens are redirected, and only for files the editor put in place.
//
// The deploy folder is read from the same file the editor writes it to:
//   %LOCALAPPDATA%\iw4-radiant\launch.cfg   (first line = the custom-zone folder)
// Activity is logged to %LOCALAPPDATA%\iw4-radiant\mapload.log.

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string g_deployDir;
std::string g_logPath;

using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileA_t g_realCreateFileA = nullptr;
CreateFileW_t g_realCreateFileW = nullptr;

std::string localAppData() {
    char buf[MAX_PATH] = ".";
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    return buf;
}

void logLine(const std::string& s) {
    if (g_logPath.empty()) return;
    if (FILE* f = std::fopen(g_logPath.c_str(), "a")) {
        std::fputs(s.c_str(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

bool endsWithFf(const char* name, size_t len) {
    return len >= 3 && (name[len - 3] == '.') && (name[len - 2] | 0x20) == 'f' && (name[len - 1] | 0x20) == 'f';
}

const char* baseName(const char* p) {
    const char* b = p;
    for (const char* c = p; *c; c++)
        if (*c == '\\' || *c == '/') b = c + 1;
    return b;
}

// If `path` opens an *.ff whose basename also exists in the deploy folder, returns that redirected
// path; otherwise returns "" (open unchanged). Never redirects a file already under the deploy dir.
std::string redirectPath(const char* path) {
    if (g_deployDir.empty() || !path) return {};
    size_t len = std::strlen(path);
    if (!endsWithFf(path, len)) return {};
    const char* base = baseName(path);
    std::string candidate = g_deployDir + "\\" + base;
    if (candidate == path) return {};  // already the deploy copy
    if (GetFileAttributesA(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return {};  // not a custom map
    return candidate;
}

HANDLE WINAPI hookedCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                DWORD flags, HANDLE tmpl) {
    std::string redir = name ? redirectPath(name) : std::string();
    if (!redir.empty()) {
        logLine(std::string("redirect ") + name + " -> " + redir);
        return g_realCreateFileA(redir.c_str(), access, share, sa, disp, flags, tmpl);
    }
    return g_realCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI hookedCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                DWORD flags, HANDLE tmpl) {
    if (name) {
        char narrow[MAX_PATH * 2] = "";
        int n = WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr);
        if (n > 0) {
            std::string redir = redirectPath(narrow);
            if (!redir.empty()) {
                logLine(std::string("redirect(W) ") + narrow + " -> " + redir);
                wchar_t wide[MAX_PATH * 2] = L"";
                MultiByteToWideChar(CP_ACP, 0, redir.c_str(), -1, wide, MAX_PATH * 2);
                return g_realCreateFileW(wide, access, share, sa, disp, flags, tmpl);
            }
        }
    }
    return g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

// Replaces, in one module's import table, the IAT slots pointing at kernel32's CreateFileA/W with
// our hooks. Returns how many slots were patched. Walks all imported modules (the thunk may be
// listed under kernel32.dll, KERNELBASE.dll or an api-ms-* forwarder).
int hookModuleIat(HMODULE module) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;

    int patched = 0;
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto* orig = imp->OriginalFirstThunk
                         ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk)
                         : thunk;
        for (; orig->u1.AddressOfData; orig++, thunk++) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            void* repl = nullptr;
            if (std::strcmp(name->Name, "CreateFileA") == 0) repl = reinterpret_cast<void*>(hookedCreateFileA);
            else if (std::strcmp(name->Name, "CreateFileW") == 0) repl = reinterpret_cast<void*>(hookedCreateFileW);
            if (!repl) continue;
            if (reinterpret_cast<void*>(thunk->u1.Function) == repl) continue;  // already hooked
            DWORD old;
            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                thunk->u1.Function = reinterpret_cast<ULONGLONG>(repl);
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                patched++;
            }
        }
    }
    return patched;
}

// --- Backup inline hook on KernelBase itself -------------------------------------------------
// A packed game may resolve CreateFile straight to KernelBase, bypassing every import table. So we
// also patch the KernelBase entry points with a 14-byte absolute jump. Only done when the first 15
// bytes match the prologue seen on this Windows build (all position-independent, rsp/register-
// relative), so the relocated copy in the trampoline behaves identically; otherwise we skip it and
// rely on the import-table hooks. Other threads are suspended while patching, and any that were
// parked inside the patched bytes are moved to the matching spot in the trampoline.
constexpr size_t kPatchLen = 15;
const uint8_t kPrologueCreateFileA[kPatchLen] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18};
const uint8_t kPrologueCreateFileW[kPatchLen] = {0x48, 0x83, 0xec, 0x58, 0x44, 0x8b, 0x94, 0x24,
                                                 0x88, 0x00, 0x00, 0x00, 0x41, 0x8b, 0xc2};

void writeAbsJump(uint8_t* at, const void* to) {
    at[0] = 0xFF;
    at[1] = 0x25;  // jmp qword ptr [rip+0]
    std::memset(at + 2, 0, 4);
    std::memcpy(at + 6, &to, 8);
}

std::vector<HANDLE> suspendOtherThreads() {
    std::vector<HANDLE> out;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) CloseHandle(h);
            else out.push_back(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out;
}

// Hooks target; on success *realSlot becomes the trampoline (call it to run the original). The
// slot is updated BEFORE the jump is written: the hook may fire immediately (even from our own
// logging), and calling the patched original from it would recurse forever.
bool inlineHook(void* target, const uint8_t (&prologue)[kPatchLen], void* hook, void** realSlot, const char* what) {
    auto* fn = static_cast<uint8_t*>(target);
    if (!fn || std::memcmp(fn, prologue, kPatchLen) != 0) {
        logLine(std::string("inline hook skipped for ") + what + " (prologue differs on this Windows build)");
        return false;
    }
    auto* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, fn, kPatchLen);
    writeAbsJump(tramp + kPatchLen, fn + kPatchLen);  // continue after the copied prologue
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    void* previous = *realSlot;
    *realSlot = tramp;  // the trampoline runs the original identically, patched or not

    std::vector<HANDLE> threads = suspendOtherThreads();
    DWORD old;
    bool ok = VirtualProtect(fn, kPatchLen, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (ok) {
        for (HANDLE h : threads) {  // a thread parked mid-prologue resumes in the trampoline copy
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Rip >= reinterpret_cast<DWORD64>(fn) &&
                ctx.Rip < reinterpret_cast<DWORD64>(fn + kPatchLen)) {
                ctx.Rip = reinterpret_cast<DWORD64>(tramp) + (ctx.Rip - reinterpret_cast<DWORD64>(fn));
                SetThreadContext(h, &ctx);
            }
        }
        writeAbsJump(fn, hook);
        fn[14] = 0x90;  // pad the 15th byte so the patched prologue disassembles cleanly
        VirtualProtect(fn, kPatchLen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, kPatchLen);
    }
    for (HANDLE h : threads) {
        ResumeThread(h);
        CloseHandle(h);
    }
    if (!ok) {
        *realSlot = previous;
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    logLine(std::string("inline-hooked ") + what);
    return true;
}

int hookAllModules() {
    HMODULE mods[512];
    DWORD needed = 0;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&hookAllModules), &self);
    int total = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        int count = static_cast<int>(needed / sizeof(HMODULE));
        for (int i = 0; i < count && i < 512; i++)
            if (mods[i] != self) total += hookModuleIat(mods[i]);
    }
    return total;
}

std::string envVar(const char* name) {
    char buf[MAX_PATH] = "";
    DWORD n = GetEnvironmentVariableA(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::string(buf) : std::string();
}

DWORD WINAPI initThread(LPVOID) {
    std::string dir = localAppData() + "\\iw4-radiant";
    // Overrides (used by the offline test): IW4R_MAPLOAD_LOG, IW4R_DEPLOY_DIR.
    g_logPath = envVar("IW4R_MAPLOAD_LOG");
    if (g_logPath.empty()) g_logPath = dir + "\\mapload.log";
    g_deployDir = envVar("IW4R_DEPLOY_DIR");
    if (g_deployDir.empty()) {
        std::string cfg = dir + "\\launch.cfg";
        if (FILE* f = std::fopen(cfg.c_str(), "r")) {
            char line[MAX_PATH] = "";
            if (std::fgets(line, sizeof line, f)) {
                size_t n = std::strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
                g_deployDir = line;
            }
            std::fclose(f);
        }
    }
    logLine("--- iw4r_mapload loaded; deployDir=\"" + g_deployDir + "\" ---");
    if (g_deployDir.empty()) {
        logLine("no deploy folder in launch.cfg; nothing hooked. Set it in the editor (Settings) and re-run.");
        return 0;
    }
    // The real functions must be KernelBase's: kernel32's CreateFileA/W are `jmp [slot]` stubs whose
    // slot sits in kernel32's own import table, which the IAT pass below patches — calling the
    // kernel32 export from the hook would re-enter the hook forever.
    HMODULE kb = GetModuleHandleA("KernelBase.dll");
    if (!kb) {
        logLine("KernelBase.dll not loaded; nothing hooked");
        return 0;
    }
    void* kbA = reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileA"));
    void* kbW = reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW"));
    g_realCreateFileA = reinterpret_cast<CreateFileA_t>(kbA);
    g_realCreateFileW = reinterpret_cast<CreateFileW_t>(kbW);
    if (!g_realCreateFileA || !g_realCreateFileW) {
        logLine("CreateFileA/W not found in KernelBase; nothing hooked");
        return 0;
    }
    // Backup: hook KernelBase itself; when it succeeds the "real" function becomes its trampoline.
    inlineHook(kbA, kPrologueCreateFileA, reinterpret_cast<void*>(hookedCreateFileA),
               reinterpret_cast<void**>(&g_realCreateFileA), "CreateFileA");
    inlineHook(kbW, kPrologueCreateFileW, reinterpret_cast<void*>(hookedCreateFileW),
               reinterpret_cast<void**>(&g_realCreateFileW), "CreateFileW");

    // The game loads modules after we inject, and the user may run "devmap" minutes later, so keep
    // re-applying the hooks for a while to catch any module that appears after injection. Hooks
    // already applied persist regardless, so this only needs to outlast new module loads.
    int firstHooked = 0;
    for (int i = 0; i < 1200; i++) {  // ~5 minutes at 250ms
        int n = hookAllModules();
        if (n > 0 && firstHooked == 0) {
            firstHooked = n;
            logLine("hooked " + std::to_string(n) + " import slot(s) on pass " + std::to_string(i));
        }
        Sleep(250);
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
