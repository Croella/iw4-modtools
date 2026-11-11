#include "mapio.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parseFace(const std::string& line, Face& f) {
    std::istringstream in(line);
    for (int i = 0; i < 3; i++) {
        std::string open, close;
        if (!(in >> open >> f.p[i].x >> f.p[i].y >> f.p[i].z >> close)) return false;
        if (open != "(" || close != ")") return false;
    }
    double reserved = 0;
    if (!(in >> f.material >> f.tex.w >> f.tex.h >> f.tex.sx >> f.tex.sy >> f.tex.rot >> reserved)) return false;
    std::string rest;
    std::getline(in, rest);
    f.tail = trim(rest);
    return true;
}

bool parseKeyValue(const std::string& line, KeyValue& kv) {
    size_t a = line.find('"');
    if (a == std::string::npos) return false;
    size_t b = line.find('"', a + 1);
    if (b == std::string::npos) return false;
    size_t c = line.find('"', b + 1);
    size_t d = line.rfind('"');
    if (c == std::string::npos || d <= c) return false;
    kv.key = line.substr(a + 1, b - a - 1);
    kv.value = line.substr(c + 1, d - c - 1);
    return true;
}

std::string fmt(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v == 0 ? 0.0 : v);  // avoids "-0"
    return buf;
}

std::string faceLine(const Face& f) {
    std::string s = " ";
    for (const Vec3& p : f.p) s += "( " + fmt(p.x) + " " + fmt(p.y) + " " + fmt(p.z) + " ) ";
    s += f.material + " " + fmt(f.tex.w) + " " + fmt(f.tex.h) + " " + fmt(f.tex.sx) + " " + fmt(f.tex.sy) +
         " " + fmt(f.tex.rot) + " 0";
    if (!f.tail.empty()) s += " " + f.tail;
    return s + "\n";
}
}  // namespace

bool parseMapText(const std::string& text, Document& doc, std::string& error) {
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    Document out;
    out.ents.clear();
    out.header.clear();
    Entity* ent = nullptr;
    Brush* brush = nullptr;
    int depth = 0;
    std::string blockText;  // collects an unsupported block inside an entity
    int blockDepth = 0;
    bool sawVersion = false;

    while (std::getline(in, raw)) {
        lineNo++;
        std::string line = trim(raw);
        if (blockDepth > 0) {
            blockText += raw + "\n";
            if (line == "{") {
                blockDepth++;
            } else if (line == "}" && --blockDepth == 0) {
                ent->rawBlocks.push_back(blockText);
                blockText.clear();
                depth = 1;
            }
            continue;
        }
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        if (depth == 0) {
            if (line == "{") {
                out.ents.emplace_back();
                ent = &out.ents.back();
                depth = 1;
            } else if (line.rfind("iwmap", 0) == 0) {
                sawVersion = true;
            } else {
                // Uses the raw line: header lines can have meaningful trailing spaces.
                if (!raw.empty() && raw.back() == '\r') raw.pop_back();
                out.header.push_back(raw);
            }
        } else if (depth == 1) {
            if (line == "{") {
                ent->brushes.emplace_back();
                brush = &ent->brushes.back();
                depth = 2;
            } else if (line == "}") {
                ent = nullptr;
                depth = 0;
            } else {
                KeyValue kv;
                if (!parseKeyValue(line, kv)) {
                    error = "line " + std::to_string(lineNo) + ": expected a key/value pair";
                    return false;
                }
                ent->kv.push_back(std::move(kv));
            }
        } else if (line == "}") {
            if (!brush->rebuild()) {
                // Keeps the source of a solid we can't build instead of dropping it.
                std::string kept = "{\n";
                for (const std::string& a : brush->attrs) kept += a + "\n";
                for (const Face& f : brush->faces) kept += faceLine(f);
                ent->rawBlocks.push_back(kept + "}\n");
                ent->brushes.pop_back();
            }
            brush = nullptr;
            depth = 1;
        } else if (line[0] == '(') {
            Face f;
            if (!parseFace(line, f)) {
                error = "line " + std::to_string(lineNo) + ": malformed brush face";
                return false;
            }
            brush->faces.push_back(std::move(f));
        } else if (brush->faces.empty() && (line.rfind("mesh", 0) == 0 || line.rfind("curve", 0) == 0)) {
            // A patch, not a brush: its braces are captured as one raw block.
            ent->brushes.pop_back();
            brush = nullptr;
            blockText = "{\n" + raw + "\n";
            blockDepth = 1;
        } else {
            brush->attrs.push_back(line);
        }
    }
    if (depth != 0 || blockDepth != 0) {
        error = "unexpected end of file inside a block";
        return false;
    }
    if (!sawVersion) {
        error = "missing iwmap version line";
        return false;
    }
    if (out.ents.empty()) {
        error = "map has no entities";
        return false;
    }
    doc.ents = std::move(out.ents);
    doc.header = std::move(out.header);
    doc.sel.clear();
    doc.dirty = false;
    doc.revision++;
    return true;
}

std::string writeMapText(const Document& doc) {
    std::string s = "iwmap 4\n";
    for (const std::string& h : doc.header) s += h + "\n";
    for (size_t e = 0; e < doc.ents.size(); e++) {
        const Entity& ent = doc.ents[e];
        s += "// entity " + std::to_string(e) + "\n{\n";
        for (const KeyValue& kv : ent.kv) s += "\"" + kv.key + "\" \"" + kv.value + "\"\n";
        for (size_t b = 0; b < ent.brushes.size(); b++) {
            const Brush& br = ent.brushes[b];
            s += "// brush " + std::to_string(b) + "\n{\n";
            for (const std::string& a : br.attrs) s += a + "\n";
            for (const Face& f : br.faces) s += faceLine(f);
            s += "}\n";
        }
        for (const std::string& r : ent.rawBlocks) s += r;
        s += "}\n";
    }
    return s;
}

bool loadMap(const std::string& path, Document& doc, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "can't open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    Document next;
    if (!parseMapText(ss.str(), next, error)) return false;
    doc.reset();
    doc.ents = std::move(next.ents);
    doc.header = std::move(next.header);
    doc.path = path;
    doc.revision++;
    return true;
}

bool saveMap(const std::string& path, const Document& doc, std::string& error) {
    // Writes to a temp file first so a failed save can't truncate the user's map.
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "can't write " + tmp;
            return false;
        }
        f << writeMapText(doc);
        f.flush();
        if (!f.good()) {
            error = "write failed for " + tmp;
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "can't replace " + path + " (new version is at " + tmp + ")";
        return false;
    }
    return true;
}
