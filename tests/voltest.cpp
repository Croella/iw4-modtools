// Offline check of iw4r_mapload.dll's volumetric pass: a D3D9 app that looks like the game to the
// DLL (a redirected "<map>.ff" with a .volumetric next to it, a GfxViewParms in its image, a
// full-screen R32F "floatz" target, a sky-over-ground frame) and checks that the frame it presents
// gained light below the horizon under the sun, and that a "HUD" drawn after a 2D matrix upload
// is left alone. Usage: iw4r_voltest <iw4r_mapload.dll> [fog|occ]
// (occ: an occluder triangle - a full_clip face - covering the view must stop the shafts).
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

struct ViewParms {
    float v[16], p[16], vp[16], ivp[16];
    float origin[4];
};
volatile ViewParms g_view;  // in the exe image, where the DLL's scan should find it

static void makeView(float w, float h) {
    const float r[3] = {0, -1, 0}, u[3] = {0, 0, 1}, f[3] = {1, 0, 0}, o[3] = {10, 20, 30};
    float V[16] = {}, P[16] = {}, VP[16] = {};
    for (int i = 0; i < 3; i++) V[i * 4 + 0] = r[i], V[i * 4 + 1] = u[i], V[i * 4 + 2] = f[i];
    V[12] = -(o[0] * r[0] + o[1] * r[1] + o[2] * r[2]);
    V[13] = -(o[0] * u[0] + o[1] * u[1] + o[2] * u[2]);
    V[14] = -(o[0] * f[0] + o[1] * f[1] + o[2] * f[2]);
    V[15] = 1;
    const float ty = std::tan(0.5f * 50 * 3.14159265f / 180);
    P[5] = 1 / ty;
    P[0] = P[5] * h / w;
    P[10] = 1;
    P[11] = 1;
    P[14] = -4;
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += V[a * 4 + k] * P[k * 4 + b];
            VP[a * 4 + b] = s;
        }
    for (int i = 0; i < 16; i++) g_view.v[i] = V[i], g_view.p[i] = P[i], g_view.vp[i] = VP[i];
    for (int i = 0; i < 3; i++) g_view.origin[i] = o[i];
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: iw4r_voltest <iw4r_mapload.dll> [fog|occ]\n");
        return 2;
    }
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "iw4r_voltest";
    fs::create_directories(dir);
    std::ofstream(dir / "voltest.ff") << "x";
    const bool fogMode = argc > 2 && std::string(argv[2]) == "fog";
    const bool occMode = argc > 2 && std::string(argv[2]) == "occ";
    if (fogMode)  // red fog only: the ground (far) should turn redder than the pillar (near)
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\nfog 1\nfogdensity 0.004\nfogfalloff 0\n"
                                                     "fogstart 0\nfogmax 1\nfogcolor 1 0 0\nfogsun 0\nfogsky 0\n";
    else if (occMode)  // a wall at x = 100 in front of the camera (at x = 10, looking down +x)
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\ncolor 1 0.9 0.7\nintensity 1\n"
                                                     "occluder 100 -5000 -5000 100 5000 -5000 100 5000 5000\n"
                                                     "occluder 100 -5000 -5000 100 5000 5000 100 -5000 5000\n";
    else
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\ncolor 1 0.9 0.7\nintensity 1\n";
    const std::string logPath = (dir / "mapload.log").string();
    fs::remove(logPath);
    SetEnvironmentVariableA("IW4R_DEPLOY_DIR", dir.string().c_str());
    SetEnvironmentVariableA("IW4R_MAPLOAD_LOG", logPath.c_str());

    const UINT W = 640, H = 360;
    makeView(float(W), float(H));
    if (!LoadLibraryA(argv[1])) {
        std::printf("FAIL: can't load %s\n", argv[1]);
        return 1;
    }
    Sleep(1500);  // the DLL hooks from its own thread
    HANDLE fh = CreateFileA("C:\\nowhere\\zone\\english\\voltest.ff", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        std::printf("FAIL: the .ff open wasn't redirected\n");
        return 1;
    }
    CloseHandle(fh);

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "voltest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, LONG(W), LONG(H)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowA("voltest", "voltest", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                            nullptr, wc.hInstance, nullptr);
    IDirect3D9* d9 = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.hDeviceWindow = hw;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        std::printf("FAIL: no D3D9 device\n");
        return 1;
    }
    IDirect3DTexture9* fz = nullptr;
    IDirect3DSurface9 *fzs = nullptr, *bb = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &fz, nullptr);
    fz->GetSurfaceLevel(0, &fzs);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    // Like the game: its own depth buffer (the DLL should turn it into INTZ).
    IDirect3DSurface9* ds = nullptr;
    dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, nullptr);
    dev->SetDepthStencilSurface(ds);
    // Like the game's post effects: the finished world is copied to a texture, then drawn back over
    // the screen in 2D (the effect must already be in the copy), then the HUD.
    IDirect3DTexture9* copy = nullptr;
    IDirect3DSurface9* copyS = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &copy, nullptr);
    copy->GetSurfaceLevel(0, &copyS);
    struct TV {
        float x, y, z, rhw, u, v;
    };
    const TV fullQuad[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {W - 0.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, H - 0.5f, 0, 1, 0, 1}, {W - 0.5f, H - 0.5f, 0, 1, 1, 1}};
    const D3DRECT ground = {0, LONG(H / 2), LONG(W), LONG(H)};
    const D3DRECT pillar = {LONG(W / 2 - 20), 0, LONG(W / 2 + 20), LONG(H)};
    const DWORD kGround = 0xFF101010, kHud = 0xFF404040;
    // The HUD: a box near the lit ground, drawn after the game's 2D (pixel) matrix goes up.
    const D3DRECT hud = {LONG(W / 2 - 110), LONG(H / 2 + 10), LONG(W / 2 - 80), LONG(H / 2 + 40)};
    const float ortho[16] = {2.0f / W, 0, 0, 0, 0, -2.0f / H, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    for (int frame = 0; frame < 240; frame++) {
        dev->BeginScene();
        {  // like the game: the view-projection goes to the vertex shader transposed
            float t[16];
            for (int i = 0; i < 16; i++) t[i] = g_view.vp[(i & 3) * 4 + (i >> 2)];
            dev->SetVertexShaderConstantF(4, t, 4);
        }
        dev->SetRenderTarget(0, fzs);  // "floatz": 0 = sky, 1.0 = geometry
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1, 0);
        dev->Clear(1, &ground, D3DCLEAR_TARGET, 0xFFFFFFFF, 1, 0);
        dev->Clear(1, &pillar, D3DCLEAR_TARGET, 0xFFFFFFFF, 1, 0);
        dev->SetRenderTarget(0, bb);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFE0E8FF, 1, 0);  // bright sky
        dev->Clear(1, &ground, D3DCLEAR_TARGET, kGround, 1, 0);
        dev->Clear(1, &pillar, D3DCLEAR_TARGET, kGround, 1, 0);
        // depth: sky 1, ground at view z 400 (z = 1 - 4/400), pillar at view z 40 (z = 1 - 4/40)
        dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        dev->Clear(1, &ground, D3DCLEAR_ZBUFFER, 0, 1 - 4.0f / 400, 0);
        dev->Clear(1, &pillar, D3DCLEAR_ZBUFFER, 0, 1 - 4.0f / 40, 0);
        dev->StretchRect(bb, nullptr, copyS, nullptr, D3DTEXF_NONE);
        dev->SetVertexShaderConstantF(0, ortho, 4);  // 2D from here on
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetTexture(0, copy);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fullQuad, sizeof(TV));
        dev->SetTexture(0, nullptr);
        dev->SetRenderState(D3DRS_ZENABLE, TRUE);
        dev->Clear(1, &hud, D3DCLEAR_TARGET, kHud, 1, 0);
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(16);
    }
    // The presented frame (COPY swap keeps it): a ground pixel under the sun must have gained light.
    IDirect3DSurface9* sys = nullptr;
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr;
    sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    auto px = [&](UINT x, UINT y) { return reinterpret_cast<const DWORD*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch)[x]; };
    const DWORD under = px(W / 2 - 60, H / 2 + 20), corner = px(5, H - 5), shadow = px(W / 2, H / 2 + 20);
    const DWORD pil = px(W / 2, H - 10), hudPx = px(W / 2 - 95, H / 2 + 25);
    sys->UnlockRect();
    std::printf("ground under the sun %08lX, far corner %08lX, behind the pillar %08lX (cleared %08lX)\n", under, corner, shadow, kGround);
    std::printf("--- %s ---\n", logPath.c_str());
    if (FILE* f = std::fopen(logPath.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) std::fputs(line, stdout);
        std::fclose(f);
    }
    const bool hudOk = (hudPx & 0xFFFFFF) == (kHud & 0xFFFFFF);
    std::printf("hud %08lX (drawn %08lX): %s\n", hudPx, kHud, hudOk ? "untouched" : "CHANGED by the effect");
    if (!hudOk) {
        std::printf("FAIL: the effect was drawn over the HUD\n");
        return 1;
    }
    if (occMode) {
        const bool dark = (under & 0xFF) <= (kGround & 0xFF) + 2;
        std::printf(dark ? "PASS: occluder stopped the shafts\n" : "FAIL: light got past the occluder\n");
        return dark ? 0 : 1;
    }
    if (fogMode) {
        std::printf("fog: far ground %08lX, near pillar %08lX\n", under, pil);
        const int gr = (under >> 16) & 0xFF, gg = (under >> 8) & 0xFF, pr = (pil >> 16) & 0xFF;
        const bool ok = gr > gg + 40 && gr > pr + 20;
        std::printf(ok ? "PASS: fog\n" : "FAIL: fog\n");
        return ok ? 0 : 1;
    }
    const bool lit = (under & 0xFF) > (kGround & 0xFF) + 8;
    std::printf(lit ? "PASS: shafts drawn\n" : "FAIL: no light added below the horizon\n");
    return lit ? 0 : 1;
}
