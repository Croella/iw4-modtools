#include "model/xanimexport.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace xanimexport {
namespace {

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == ',' || c == '\r') {
            i++;
        } else if (c == '"') {
            size_t e = line.find('"', i + 1);
            if (e == std::string::npos) e = line.size();
            out.push_back(line.substr(i + 1, e - i - 1));
            i = e + 1;
        } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break;
        } else {
            size_t s = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != ',' && line[i] != '\r') i++;
            out.push_back(line.substr(s, i - s));
        }
    }
    return out;
}
float num(const std::vector<std::string>& t, size_t i) { return i < t.size() ? std::strtof(t[i].c_str(), nullptr) : 0.0f; }
int inum(const std::vector<std::string>& t, size_t i) { return i < t.size() ? std::atoi(t[i].c_str()) : 0; }

}  // namespace

bool load(const std::string& path, Anim& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "can't read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), out, error);
}

bool parse(const std::string& text, Anim& out, std::string& error) {
    out = Anim{};
    std::istringstream in(text);
    std::string line;
    bool sawHeader = false;
    int frame = -1, part = -1, numFrames = 0;
    std::vector<int> frameIndexOf;  // FRAME numbers may not start at 0
    while (std::getline(in, line)) {
        std::vector<std::string> t = tokens(line);
        if (t.empty()) continue;
        const std::string& k = t[0];
        if (k == "ANIMATION") {
            sawHeader = true;
        } else if (k == "VERSION") {
            out.version = inum(t, 1);
        } else if (k == "NUMPARTS") {
            out.parts.assign(static_cast<size_t>(std::max(0, inum(t, 1))), "");
        } else if (k == "PART" && frame < 0 && t.size() >= 3) {
            int i = inum(t, 1);
            if (i >= 0 && i < int(out.parts.size())) {
                out.parts[i] = t[2];
                for (char& c : out.parts[i]) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));  // like model bones
            }
        } else if (k == "FRAMERATE") {
            out.framerate = num(t, 1);
        } else if (k == "NUMFRAMES") {
            numFrames = std::max(0, inum(t, 1));
            out.frames.reserve(static_cast<size_t>(numFrames));
        } else if (k == "NOTETRACKS") {
            frame = -2;  // notetrack section: FRAME lines are notes from here on
        } else if (k == "FRAME" && frame == -2) {
            if (t.size() >= 3) out.notes.push_back({inum(t, 1), t[2]});
        } else if (k == "FRAME") {
            out.frames.emplace_back(out.parts.size());
            frame = static_cast<int>(out.frames.size()) - 1;
            part = -1;
        } else if (k == "PART" && frame >= 0) {
            part = inum(t, 1);
            if (part < 0 || part >= int(out.parts.size())) part = -1;
        } else if (k == "OFFSET" && frame >= 0 && part >= 0) {
            float* o = out.frames[frame][part].offset;
            o[0] = num(t, 1), o[1] = num(t, 2), o[2] = num(t, 3);
        } else if ((k == "X" || k == "Y" || k == "Z") && frame >= 0 && part >= 0) {
            int r = k == "X" ? 0 : k == "Y" ? 1 : 2;
            float* a = out.frames[frame][part].axis[r];
            a[0] = num(t, 1), a[1] = num(t, 2), a[2] = num(t, 3);
        }
    }
    (void)frameIndexOf;
    if (!sawHeader && out.version == 0) {
        error = "not an xanim_export file (no ANIMATION/VERSION header)";
        return false;
    }
    if (out.parts.empty() || out.frames.empty()) {
        error = "the animation has no parts or no frames";
        return false;
    }
    return true;
}

}  // namespace xanimexport
