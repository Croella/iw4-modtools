#pragma once
#include <imgui.h>

// The look shared by Mod Tools and Radiant: ImGui's dark theme with every saturated blue-ish
// colour turned pink (brightness and alpha kept, so hover/active/selected stay distinct).
inline void applyIw4rStyle() {
    ImGui::StyleColorsDark();
    for (ImVec4& c : ImGui::GetStyle().Colors) {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
        if (s > 0.15f && h > 0.45f && h < 0.78f) ImGui::ColorConvertHSVtoRGB(0.925f, s, v, c.x, c.y, c.z);
    }
    ImGui::GetStyle().WindowRounding = 0;
    ImGui::GetStyle().FrameRounding = 2;
}
