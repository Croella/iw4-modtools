#include "uitest.h"
#include <cstdio>
#include <fstream>
#include "mapio.h"

UiTest::UiTest(App& a, std::string path) : app(a), reportPath(std::move(path)) {
    if (!reportPath.empty()) build();
}

void UiTest::wait(int frames) {
    for (int i = 0; i < frames; i++) steps.push_back([] {});
}

void UiTest::moveTo(ImVec2 p) {
    steps.push_back([p] { ImGui::GetIO().AddMousePosEvent(p.x, p.y); });
    wait(2);
}

void UiTest::press(int button) {
    steps.push_back([button] { ImGui::GetIO().AddMouseButtonEvent(button, true); });
    wait(2);
}

void UiTest::release(int button) {
    steps.push_back([button] { ImGui::GetIO().AddMouseButtonEvent(button, false); });
    wait(2);
}

void UiTest::click(std::function<ImVec2()> where, int button) {
    // Position is resolved late: it depends on layout from frames that haven't run yet.
    steps.push_back([where] {
        ImVec2 p = where();
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    });
    wait(3);
    press(button);
    release(button);
    wait(2);
}

void UiTest::drag(int view, double u0, double v0, double u1, double v1) {
    steps.push_back([this, view, u0, v0] {
        ImVec2 p = app.gridToScreen(view, u0, v0);
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    });
    wait(3);
    press(0);
    for (int i = 1; i <= 4; i++) {
        double t = i / 4.0;
        steps.push_back([this, view, u0, v0, u1, v1, t] {
            ImVec2 p = app.gridToScreen(view, u0 + (u1 - u0) * t, v0 + (v1 - v0) * t);
            ImGui::GetIO().AddMousePosEvent(p.x, p.y);
        });
        wait(2);
    }
    release(0);
    wait(2);
}

void UiTest::key(ImGuiKey k, bool ctrl) {
    steps.push_back([k, ctrl] {
        ImGuiIO& io = ImGui::GetIO();
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(k, true);
    });
    wait(2);
    steps.push_back([k, ctrl] {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(k, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
    });
    wait(2);
}

void UiTest::shift(bool down) {
    steps.push_back([down] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, down); });
    wait(2);
}

void UiTest::check(const std::string& name, std::function<bool()> test) {
    steps.push_back([this, name, test] {
        bool ok = test();
        report += (ok ? "PASS  " : "FAIL  ") + name + "\n";
        (ok ? passCount : failCount)++;
    });
}

void UiTest::build() {
    Document& doc = app.doc;
    auto brushes = [&doc] { return doc.ents[0].brushes.size(); };
    auto bounds = [&doc]() -> AABB { return doc.selectionBounds(); };
    const int top = 0, front = 1;

    drag(top, 0, 0, 128, 96);
    check("drag in grid creates a brush", [=] { return brushes() == 1; });
    check("new brush snaps to the drag area", [=] {
        AABB b = bounds();
        return b.mins.x == 0 && b.mins.y == 0 && b.maxs.x == 128 && b.maxs.y == 96;
    });
    check("new brush is selected and the doc is dirty", [&doc] { return doc.sel.size() == 1 && doc.dirty; });

    drag(top, 64, 48, 128, 80);
    check("drag inside selection moves it", [=] {
        AABB b = bounds();
        return b.mins.x == 64 && b.mins.y == 32 && b.maxs.x == 192 && brushes() == 1;
    });

    drag(top, 192, 80, 256, 80);
    check("drag on right edge resizes that side only", [=] {
        AABB b = bounds();
        return b.mins.x == 64 && b.maxs.x == 256;
    });

    drag(front, 160, 0, 160, -64);
    check("front view moves on the Z axis", [=] {
        AABB b = bounds();
        return b.mins.z == -64 && b.mins.x == 64;
    });

    key(ImGuiKey_Z, true);
    check("undo reverts the last move", [=] { return bounds().mins.z == 0; });
    key(ImGuiKey_Y, true);
    check("redo reapplies it", [=] { return bounds().mins.z == -64; });

    key(ImGuiKey_Escape);
    check("escape clears the selection", [&doc] { return doc.sel.empty(); });

    click([this] { return app.gridToScreen(top, 128, 64); });
    check("click on a brush selects it", [&doc] { return doc.sel.size() == 1; });
    check("click doesn't create a brush", [=] { return brushes() == 1; });

    click([this] { return app.gridToScreen(top, -400, 250); });
    check("click on empty space deselects", [&doc] { return doc.sel.empty(); });

    shift(true);
    click([this] { return app.gridToScreen(top, 128, 64); });
    shift(false);
    check("shift-click selects", [&doc] { return doc.sel.size() == 1; });

    click([this] { return app.thumbCenters[1]; });
    check("texture click applies to selection", [&doc] {
        return doc.ents[0].brushes[0].faces[0].material == "clip";
    });

    key(ImGuiKey_D, true);
    check("clone adds a selected copy", [=, &doc] { return brushes() == 2 && doc.sel.size() == 1 && doc.sel[0].brush == 1; });
    key(ImGuiKey_Delete);
    check("delete removes it", [=] { return brushes() == 1; });

    // Grid context menu: the first class in the list sits just below the header and separator.
    click([this] { return app.gridToScreen(top, -128, -128); }, 1);
    steps.push_back([this] {
        ImVec2 p = app.gridToScreen(top, -128, -128);
        ImGui::GetIO().AddMousePosEvent(p.x + 30, p.y + 42);
    });
    wait(3);
    press(0);
    release(0);
    wait(2);
    check("right-click menu adds a point entity", [&doc] {
        return doc.ents.size() == 2 && doc.ents[1].classname() == "mp_tdm_spawn";
    });
    check("new entity lands on the clicked point", [&doc] {
        Vec3 o;
        return doc.ents.size() == 2 && doc.ents[1].origin(o) && o.x == -128 && o.y == -128;
    });
    drag(top, -128, -120, 0, -120);
    check("point entity can be moved", [&doc] {
        Vec3 o;
        return doc.ents.size() == 2 && doc.ents[1].origin(o) && o.x == 0 && o.y == -128;
    });

    steps.push_back([this] {
        app.camera.pos = {128, -600, 0};
        app.camera.yaw = 90;
        app.camera.pitch = 0;
    });
    wait(2);
    key(ImGuiKey_Escape);
    click([this] {
        const App::ViewRect& r = app.cameraRect;
        return ImVec2{r.pos.x + r.size.x / 2, r.pos.y + r.size.y / 2};
    });
    check("camera click picks the brush in view", [&doc] { return doc.sel.size() == 1 && doc.sel[0].brush == 0; });

    // At this zoom a 16-unit brush is 8 pixels wide, smaller than the full 7px handles on each side.
    key(ImGuiKey_Escape);
    drag(top, 304, 208, 320, 224);
    check("tiny brush is created", [=] { return brushes() == 2; });
    drag(top, 312, 216, 376, 216);
    check("tiny brush moves instead of resizing", [=] {
        AABB b = bounds();
        return brushes() == 2 && b.mins.x == 368 && b.maxs.x == 384;
    });

    // Undo pressed in the middle of a drag must be ignored, not crash on stale indices.
    steps.push_back([this] {
        ImVec2 p = app.gridToScreen(top, 400, 150);
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    });
    wait(3);
    press(0);
    steps.push_back([this] {
        ImVec2 p = app.gridToScreen(top, 464, 214);
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    });
    wait(3);
    key(ImGuiKey_Z, true);
    steps.push_back([this] {
        ImVec2 p = app.gridToScreen(top, 528, 278);
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    });
    wait(3);
    release(0);
    check("undo during a drag is ignored", [=] { return brushes() == 3; });

    // Everything built by hand above must survive a save and reload.
    check("map survives a save and reload", [this, &doc] {
        std::string path = reportPath + ".map", err;
        if (!saveMap(path, doc, err)) return false;
        Document back;
        bool ok = loadMap(path, back, err) && writeMapText(back) == writeMapText(doc);
        std::remove(path.c_str());
        return ok;
    });
}

bool UiTest::tick() {
    if (warmup > 0) {  // lets the dock layout settle so widget positions are real
        warmup--;
        return true;
    }
    if (next < steps.size()) {
        steps[next++]();
        return true;
    }
    report += std::to_string(passCount) + " passed, " + std::to_string(failCount) + " failed\n";
    std::ofstream(reportPath) << report;
    return false;
}
