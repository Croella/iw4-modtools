#pragma once
#include <string>
#include <vector>

// Reader for xanim_export files (the text animation format of the Maya/Blender Call of Duty
// export plugins, VERSION 3): parts (bone names), framerate, and per frame each part's
// model-space OFFSET and X/Y/Z axes.
namespace xanimexport {

struct PartPose {
    float offset[3] = {};
    float axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // X, Y, Z rows
};

struct Anim {
    int version = 0;
    float framerate = 30;
    std::vector<std::string> parts;
    std::vector<std::vector<PartPose>> frames;  // [frame][part]
    struct Note {
        int frame;
        std::string name;
    };
    std::vector<Note> notes;  // NOTETRACKS (kept for later)
};

bool load(const std::string& path, Anim& out, std::string& error);
bool parse(const std::string& text, Anim& out, std::string& error);

}  // namespace xanimexport
