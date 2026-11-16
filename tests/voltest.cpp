// Offline check of iw4r_mapload.dll's volumetric pass: a D3D9 app that looks like the game to the
// DLL (a redirected "<map>.ff" with a .volumetric next to it, a GfxViewParms in its image, a
// full-screen R32F "floatz" target, a sky-over-ground frame) and checks that the frame it presents
// gained light below the horizon under the sun, and that a "HUD" drawn after a 2D matrix upload
// is left alone. Usage: iw4r_voltest <iw4r_mapload.dll> [fog|occ|ssao|grade|gradevol|dof|dofauto|flare|flareocc|glare|rain|snow|wxshelter|lights|lightsocc|lightspot]
// (occ: an occluder triangle - a full_clip face - covering the view must stop the shafts;
// ssao: a floor meeting a wall must darken along the crease only; grade: an all-red table must
// turn the frame red; gradevol: a grade volume around the camera (red) applies, one elsewhere (blue)
// doesn't; dof: striped ground far away blurs, the striped near pillar stays sharp; flare: a sun in
// open sky puts a lens ghost on the ground, flareocc: the same sun behind the pillar doesn't; glare:
// the bright sky bleeds below the horizon; rain/snow: drops over the far ground, fewer over the near
// pillar (only the nearest layer is in front of it); wxshelter: a trigger_weather around the camera
// stops the rain; lights: a point light over the dark ground glows there, not on the near pillar or
// the far corner; lightsocc: the same light straight behind the pillar adds nothing on it;
// lightspot: a spotlight aimed up lights the air above it, not below).
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
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
        std::printf("usage: iw4r_voltest <iw4r_mapload.dll> [fog|occ|ssao|grade|gradevol|dof|dofauto|flare|flareocc|glare|rain|snow|wxshelter|lights|lightsocc|lightspot]\n");
        return 2;
    }
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "iw4r_voltest";
    fs::create_directories(dir);
    std::ofstream(dir / "voltest.ff") << "x";
    const bool fogMode = argc > 2 && std::string(argv[2]) == "fog";
    const bool occMode = argc > 2 && std::string(argv[2]) == "occ";
    const bool aoMode = argc > 2 && std::string(argv[2]) == "ssao";
    const bool gradeMode = argc > 2 && std::string(argv[2]) == "grade";
    const bool gradeVolMode = argc > 2 && std::string(argv[2]) == "gradevol";
    const bool dofAutoMode = argc > 2 && std::string(argv[2]) == "dofauto";
    const bool dofMode = dofAutoMode || (argc > 2 && std::string(argv[2]) == "dof");
    const std::string mode = argc > 2 ? argv[2] : "";
    const bool flareMode = mode == "flare" || mode == "flareocc", glareMode = mode == "glare";
    const bool wxMode = mode == "rain" || mode == "snow" || mode == "wxshelter";
    const bool lightMode = mode == "lights" || mode == "lightsocc" || mode == "lightspot";
    // A box volume (outward planes) of half size h around c.
    auto box = [](int vi, float cx, float cy, float cz, float h) {
        std::string o = "volbrush " + std::to_string(vi) + "\n";
        const float c[3] = {cx, cy, cz};
        for (int k = 0; k < 3; k++)
            for (int sgn : {1, -1}) {
                float n[3] = {0, 0, 0};
                n[k] = float(sgn);
                char line[128];
                std::snprintf(line, sizeof line, "volplane %d %g %g %g %g\n", vi, n[0], n[1], n[2], sgn * c[k] + h);
                o += line;
            }
        return o;
    };
    if (lightMode) {  // camera (10 20 30) looking +x; the light 200 ahead, 60 to the left, 20 down (or behind the pillar)
        const char* light = mode == "lightsocc" ? "llight 210 20 20 1 1 1 100 0 0 0 -2 -2 0\n"
                            : mode == "lightspot" ? "llight 210 80 10 1 1 1 100 0 0 1 0.866 0.95 0\n"
                                                  : "llight 210 80 10 1 1 1 100 0 0 0 -2 -2 0\n";
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\nlights 1\nlightsparm 3 0.8 8 4000\n" << light;
    } else if (flareMode)  // sun up and to the left in open sky (flare), or behind the pillar (flareocc)
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun " << (mode == "flare" ? "0.9 0.3 0.25" : "0.9 0 0.25")
                                                  << "\nshafts 0\nflare 1\nflareparm 1 1 2 0.5\nflarecolor 1 0.9 0.7\n";
    else if (glareMode)
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\nglare 1\nglareparm 0.7 1 64 0\n";
    else if (wxMode) {
        std::string v = std::string("enabled 1\nsun 0.9 0 0.25\nshafts 0\nweather ") + (mode == "snow" ? "2" : "1") +
                        "\nweatherparm 3 1 300\nweatherwind 0 0\nweathercolor 1 1 1 1\n";
        if (mode == "wxshelter") {
            std::string b = box(0, 10, 20, 30, 200);
            for (const char* from : {"volbrush", "volplane"}) {
                const std::string to = std::string(from) == "volbrush" ? "wxbrush" : "wxplane";
                for (size_t at; (at = b.find(from)) != std::string::npos;) b.replace(at, std::string(from).size(), to);
            }
            v += "wxvolume 0 64 0 0\n" + b;
        }
        std::ofstream(dir / "voltest.volumetric") << v;
    } else if (gradeVolMode) {  // identity base; volume 0 (all red) around the camera at (10 20 30), volume 1 (all blue) far away
        std::string v = "enabled 1\nsun 0.9 0 0.25\nshafts 0\ngrade 1\nvignette 0\nlut 2\n"
                        "lutrow 0 0 000000ff0000\nlutrow 0 1 00ff00ffff00\nlutrow 1 0 0000ffff00ff\nlutrow 1 1 00ffffffffff\n";
        v += "gradevolume 0 0 0 0\n" + box(0, 10, 20, 30, 100);
        for (int b = 0; b < 2; b++)
            for (int g = 0; g < 2; g++) v += "vollutrow 0 " + std::to_string(b) + " " + std::to_string(g) + " ff0000ff0000\n";
        v += "gradevolume 1 0 0 0\n" + box(1, 5000, 5000, 30, 100);
        for (int b = 0; b < 2; b++)
            for (int g = 0; g < 2; g++) v += "vollutrow 1 " + std::to_string(b) + " " + std::to_string(g) + " 0000ff0000ff\n";
        std::ofstream(dir / "voltest.volumetric") << v;
    } else if (dofAutoMode)  // focus on the pillar under the crosshair (view z 40): far blur from 50 to 60 past it
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\ndof 1\ndofauto 1\ndofautospeed 20\ndofnear 0 0\n"
                                                     "doffar 10 20\ndofblur 0 32\n";
    else if (dofMode)
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\ndof 1\ndofnear 0 0\ndoffar 100 200\ndofblur 0 32\n";
    else if (aoMode)
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\nssao 1\nssaoradius 32\nssaointensity 1.5\n"
                                                     "ssaonear 20\nssaofar 3000\n";
    else if (gradeMode)  // 2x2x2 table, every entry red; plus fog off, shafts off
        std::ofstream(dir / "voltest.volumetric") << "enabled 1\nsun 0.9 0 0.25\nshafts 0\ngrade 1\nvignette 0\nlut 2\n"
                                                     "lutrow 0 0 ff0000ff0000\nlutrow 0 1 ff0000ff0000\nlutrow 1 0 ff0000ff0000\nlutrow 1 1 ff0000ff0000\n";
    else if (fogMode)  // red fog only: the ground (far) should turn redder than the pillar (near)
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
    // ssao scene: the floor (world z 0, 30 below the eye) meets the wall (view z 150) at this row.
    const float kP5 = 1 / std::tan(0.5f * 50 * 3.14159265f / 180);
    const float kWallZ = 150, kCrease = (1 + kP5 * 30 / kWallZ) * 0.5f * H, kFloorNear = kP5 * 30;
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
        if (dofMode) {  // white stripes, 4 px every 8, over the ground and the pillar (below the horizon)
            D3DRECT stripes[W / 8];
            for (UINT i = 0; i < W / 8; i++) stripes[i] = {LONG(i * 8), LONG(H / 2), LONG(i * 8 + 4), LONG(H)};
            dev->Clear(W / 8, stripes, D3DCLEAR_TARGET, 0xFFFFFFFF, 1, 0);
        }
        if (aoMode) {  // wall at view z 150 above the crease, floor (30 below the eye) down to the bottom edge
            struct CV {
                float x, y, z, rhw;
                DWORD c;
            };
            const float zw = 1 - 4.0f / kWallZ, zb = 1 - 4.0f / kFloorNear;
            const DWORD grey = 0xFF808080;
            const CV wall[4] = {{-0.5f, -0.5f, zw, 1, grey}, {W - 0.5f, -0.5f, zw, 1, grey}, {-0.5f, kCrease - 0.5f, zw, 1, grey},
                                {W - 0.5f, kCrease - 0.5f, zw, 1, grey}};
            const CV floor[4] = {{-0.5f, kCrease - 0.5f, zw, 1, grey}, {W - 0.5f, kCrease - 0.5f, zw, 1, grey},
                                 {-0.5f, H - 0.5f, zb, 1, grey}, {W - 0.5f, H - 0.5f, zb, 1, grey}};
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, wall, sizeof(CV));
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, floor, sizeof(CV));
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        }
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
    const DWORD aoCrease = px(W - 40, UINT(kCrease) - 3), aoCreaseFloor = px(W - 40, UINT(kCrease) + 2), aoHigh = px(W - 40, H / 8), aoFloor = px(W - 40, H - 30);
    const DWORD sky = px(W - 5, 5);
    // contrast (max - min of the blue channel) over 16 px of a row
    auto contrast = [&](UINT x0, UINT y) {
        int lo = 255, hi = 0;
        for (UINT x = x0; x < x0 + 16; x++) lo = (std::min)(lo, int(px(x, y) & 0xFF)), hi = (std::max)(hi, int(px(x, y) & 0xFF));
        return hi - lo;
    };
    const int groundContrast = contrast(40, H * 3 / 4), pillarContrast = contrast(W / 2 - 8, H * 3 / 4);
    // Share of pixels well above the ground colour (weather drops), over the ground left of the HUD box and on the pillar.
    auto changed = [&](UINT x0, UINT x1, UINT y0, UINT y1) {
        unsigned n = 0, all = 0;
        for (UINT y = y0; y < y1; y++)
            for (UINT x = x0; x < x1; x++, all++) n += (px(x, y) & 0xFF) > (kGround & 0xFF) + 40;
        return all ? double(n) / all : 0.0;
    };
    const double wxGround = changed(10, W / 2 - 120, H / 2 + 10, H - 10), wxPillar = changed(W / 2 - 18, W / 2 + 18, H / 2 + 10, H - 10);
    const DWORD lightAt = px(205, 218), lightAbove = px(205, 195), lightBelow = px(205, 241), pillarMid = px(W / 2, H / 2 + 40);
    const DWORD ghost = px(UINT(0.58f * W), UINT(0.62f * H)), belowHorizon = px(100, H / 2 + 3), deepGround = px(100, H - 5);
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
    if (lightMode) {
        const int g0 = kGround & 0xFF;
        std::printf("lights: at the light %08lX, above %08lX, below %08lX, pillar %08lX, corner %08lX (cleared %08lX)\n", lightAt, lightAbove,
                    lightBelow, pillarMid, corner, kGround);
        bool ok;
        if (mode == "lights") ok = int(lightAt & 0xFF) > g0 + 40 && int(pillarMid & 0xFF) <= g0 + 2 && int(corner & 0xFF) <= g0 + 2;
        else if (mode == "lightsocc") ok = int(pillarMid & 0xFF) <= g0 + 2;
        else ok = int(lightAbove & 0xFF) > g0 + 15 && int(lightBelow & 0xFF) < int(lightAbove & 0xFF) - 15;
        std::printf(ok ? "PASS: %s\n" : "FAIL: %s\n", mode.c_str());
        return ok ? 0 : 1;
    }
    if (flareMode) {
        std::printf("flare: ground at the ghost %08lX (cleared %08lX)\n", ghost, kGround);
        const bool bright = (ghost & 0xFF) > (kGround & 0xFF) + 10;
        const bool ok = mode == "flare" ? bright : (ghost & 0xFF) <= (kGround & 0xFF) + 3;
        std::printf(ok ? "PASS: %s\n" : "FAIL: %s\n", mode == "flare" ? "flare drawn for a visible sun" : "no flare for a hidden sun");
        return ok ? 0 : 1;
    }
    if (glareMode) {
        std::printf("glare: just below the horizon %08lX, deep ground %08lX (cleared %08lX)\n", belowHorizon, deepGround, kGround);
        const bool ok = (belowHorizon & 0xFF) > (kGround & 0xFF) + 15 && (deepGround & 0xFF) <= (kGround & 0xFF) + 3;
        std::printf(ok ? "PASS: glare bled the bright sky only nearby\n" : "FAIL: glare\n");
        return ok ? 0 : 1;
    }
    if (wxMode) {
        std::printf("weather: %.2f%% of the far ground, %.2f%% of the near pillar covered by drops\n", wxGround * 100, wxPillar * 100);
        const bool ok = mode == "wxshelter" ? wxGround < 0.001 : wxGround > (mode == "snow" ? 0.012 : 0.03) && wxPillar < wxGround * 0.7;
        std::printf(ok ? "PASS: %s\n" : "FAIL: %s\n", mode == "wxshelter" ? "sheltered from the weather" : "weather drawn, behind the near pillar too");
        return ok ? 0 : 1;
    }
    if (aoMode) {
        const int c = (std::min)(aoCrease & 0xFF, aoCreaseFloor & 0xFF), hi = aoHigh & 0xFF, fl = aoFloor & 0xFF;
        std::printf("ssao: floor at the crease %08lX, wall at the crease %08lX, high wall %08lX, near floor %08lX (drawn 808080)\n", aoCreaseFloor, aoCrease, aoHigh, aoFloor);
        const bool ok = c + 24 < hi && hi >= 0x80 - 3 && fl >= 0x80 - 6;
        std::printf(ok ? "PASS: ssao darkened the crease only\n" : "FAIL: ssao\n");
        return ok ? 0 : 1;
    }
    if (dofMode) {
        std::printf("dof: stripe contrast on the far ground %d, on the near pillar %d (drawn 239)\n", groundContrast, pillarContrast);
        const bool ok = groundContrast < 80 && pillarContrast > 200;
        std::printf(ok ? "PASS: dof blurred the far ground only\n" : "FAIL: dof\n");
        return ok ? 0 : 1;
    }
    if (gradeMode || gradeVolMode) {
        const int r = (sky >> 16) & 0xFF, gch = (sky >> 8) & 0xFF, b = sky & 0xFF;
        std::printf("grade: sky %08lX (drawn E0E8FF)\n", sky);
        const bool ok = r > 240 && gch < 10 && b < 10;
        std::printf(ok ? "PASS: grade table applied\n" : "FAIL: grade\n");
        return ok ? 0 : 1;
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
