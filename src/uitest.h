#pragma once
#include <functional>
#include <string>
#include <vector>
#include "app.h"

// Feeds scripted mouse and keyboard events through ImGui, so tools run through the same code
// path as real input, then checks the resulting document.
class UiTest {
public:
    UiTest(App& app, std::string reportPath);
    bool tick();  // call before ImGui::NewFrame; false when the script has finished
    int failures() const { return failCount; }

private:
    void build();
    void wait(int frames);
    void moveTo(ImVec2 p);
    void press(int button);
    void release(int button);
    void click(std::function<ImVec2()> where, int button = 0);
    void drag(int view, double u0, double v0, double u1, double v1);
    void key(ImGuiKey k, bool ctrl = false);
    void shift(bool down);
    void check(const std::string& name, std::function<bool()> test);

    App& app;
    std::string reportPath, report;
    std::vector<std::function<void()>> steps;
    size_t next = 0;
    int failCount = 0, passCount = 0;
    int warmup = 10;
};
