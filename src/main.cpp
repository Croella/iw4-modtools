#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <miniz.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "app.h"
#include "uitest.h"
#include "uistyle.h"

using Microsoft::WRL::ComPtr;

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
const char* kDefaultGame = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty Modern Warfare 2";

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> swapChain;
ComPtr<ID3D11RenderTargetView> backbuffer;
App* gApp = nullptr;
UINT pendingW = 0, pendingH = 0;

void createBackbuffer() {
    ComPtr<ID3D11Texture2D> tex;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&tex));
    device->CreateRenderTargetView(tex.Get(), nullptr, &backbuffer);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                pendingW = LOWORD(lp);
                pendingH = HIWORD(lp);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt shouldn't open the system menu
            break;
        case WM_CLOSE:
            if (gApp && !gApp->confirmDiscard()) return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

bool writeScreenshot(const std::string& path) {
    ComPtr<ID3D11Texture2D> tex;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&tex));
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::vector<uint8_t> rgba(td.Width * td.Height * 4);
    for (UINT y = 0; y < td.Height; y++)
        std::memcpy(&rgba[y * td.Width * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, td.Width * 4);
    context->Unmap(staging.Get(), 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgba.data(), td.Width, td.Height, 4, &size);
    if (!png) return false;
    FILE* f = nullptr;
    bool ok = fopen_s(&f, path.c_str(), "wb") == 0;
    if (ok) {
        ok = fwrite(png, 1, size, f) == size;
        fclose(f);
    }
    mz_free(png);
    return ok;
}
}  // namespace

// Command line:
//   iw4radiant [map]
//   --game <folder>        game install to read assets from
//   --demo                 starts with a generated sample room
//   --select-all           selects everything after loading
//   --screenshot <png>     renders hidden, saves the window to a PNG, and exits
//   --frames <n>           frames to run before the screenshot (default 240)
//   --ui-test <report>     runs scripted input through the real UI and writes pass/fail
//   --import <map.ff>      imports a compiled map (File > Import Map from .ff)
//   --bake                 bakes lighting (F9) before showing / screenshotting
//   --cull                 back-face culling on in the 3D view (View > Back-Face Culling)
//   --focus <window>       brings a window/tab to the front (e.g. "Models"), for screenshots
//   --assets <name>        opens the Asset Manager with that asset selected ("" = none)
//   --hide-tools           tool brushes (caulk, clip...) hidden in the 3D view
//   --camera x y z yaw pitch   starts the 3D camera there (degrees)
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    std::string mapPath, assetSelect, screenshot, uiTestReport, importPath, focusWindow, game = kDefaultGame;
    bool demo = false, showAssets = false, selectAll = false, bakeLight = false, cull = false, hideTools = false;
    int frames = 240;
    bool setCam = false;
    double cam[5] = {};
    for (int i = 1; i < __argc; i++) {
        std::string a = __argv[i];
        bool hasNext = i + 1 < __argc;
        if (a == "--game" && hasNext) game = __argv[++i];
        else if (a == "--screenshot" && hasNext) screenshot = __argv[++i];
        else if (a == "--frames" && hasNext) frames = std::atoi(__argv[++i]);
        else if (a == "--ui-test" && hasNext) uiTestReport = __argv[++i];
        else if (a == "--import" && hasNext) importPath = __argv[++i];
        else if (a == "--demo") demo = true;
        else if (a == "--select-all") selectAll = true;
        else if (a == "--bake") bakeLight = true;
        else if (a == "--cull") cull = true;
        else if (a == "--focus" && hasNext) focusWindow = __argv[++i];
        else if (a == "--hide-tools") hideTools = true;
        else if (a == "--assets" && hasNext) showAssets = true, assetSelect = __argv[++i];
        else if (a == "--camera" && i + 5 < __argc) {
            for (double& c : cam) c = std::atof(__argv[++i]);
            setCam = true;
        }
        else mapPath = a;
    }

    WNDCLASSEXA wc{sizeof wc};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "iw4radiant";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "iw4-radiant", WS_OVERLAPPEDWINDOW, 80, 60, 1600, 960, nullptr, nullptr, instance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 1, D3D11_SDK_VERSION, &sd,
                                             &swapChain, &device, &level, &context))) {
        MessageBoxA(hwnd, "Couldn't create a Direct3D 11 device.", "iw4-radiant", MB_OK | MB_ICONERROR);
        return 1;
    }
    createBackbuffer();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // A screenshot run always uses the default layout and never overwrites the user's.
    if (!screenshot.empty() || !uiTestReport.empty()) io.IniFilename = nullptr;
    applyIw4rStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device.Get(), context.Get());

    App app;
    gApp = &app;
    app.hwnd = hwnd;
    app.init(device.Get(), context.Get(), game);
    if (!importPath.empty()) app.importFastfileMap(importPath);
    else if (!mapPath.empty()) app.openMap(mapPath);
    if (showAssets) app.openAssetManager(assetSelect);
    else if (demo) app.buildDemo();
    if (selectAll) app.selectAll();
    if (bakeLight) app.bakeLightingNow();
    app.renderer.cullBackFaces = cull;
    app.focusWindow = focusWindow;
    if (hideTools) app.renderer.showToolFaces = false;
    if (setCam) {
        app.camera.pos = {cam[0], cam[1], cam[2]};
        app.camera.yaw = cam[3];
        app.camera.pitch = cam[4];
    }

    ShowWindow(hwnd, screenshot.empty() && uiTestReport.empty() ? SW_SHOWDEFAULT : SW_HIDE);
    UiTest uiTest(app, uiTestReport);
    UpdateWindow(hwnd);

    int frame = 0;
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (pendingW && pendingH) {
            backbuffer.Reset();
            swapChain->ResizeBuffers(0, pendingW, pendingH, DXGI_FORMAT_UNKNOWN, 0);
            pendingW = pendingH = 0;
            createBackbuffer();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (!uiTestReport.empty() && !uiTest.tick()) {
            int code = uiTest.failures() ? 3 : 0;
            app.shutdown();
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return code;
        }
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        float clear[4] = {0.1f, 0.1f, 0.12f, 1};
        ID3D11RenderTargetView* rtv = backbuffer.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (!screenshot.empty() && ++frame >= frames) {
            bool ok = writeScreenshot(screenshot);
            app.shutdown();
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return ok ? 0 : 2;
        }
        swapChain->Present(1, 0);
    }

    app.shutdown();
    gApp = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
