#include "model/xmodelexport.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace xmodelexport {
namespace {

// Splits a line into tokens: quoted strings stay whole, commas and whitespace separate.
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

bool load(const std::string& path, Model& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "can't read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), out, error);
}

bool parse(const std::string& text, Model& out, std::string& error) {
    out = Model{};
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    Bone* bone = nullptr;      // BONE i block (offset/scale/axes)
    Vertex* vert = nullptr;    // VERT i block
    Face* face = nullptr;      // TRI block
    int faceVert = -1;         // which of the 3 face vertices is being read
    Material* mat = nullptr;
    bool sawModel = false;
    while (std::getline(in, line)) {
        lineNo++;
        std::vector<std::string> t = tokens(line);
        if (t.empty()) continue;
        const std::string& k = t[0];
        if (k == "MODEL") {
            sawModel = true;
        } else if (k == "VERSION") {
            out.version = inum(t, 1);
        } else if (k == "NUMBONES") {
            out.bones.resize(static_cast<size_t>(std::max(0, inum(t, 1))));
        } else if (k == "BONE" && t.size() >= 4 && !vert) {
            // BONE index parent "name"
            int i = inum(t, 1);
            if (i < 0 || i >= int(out.bones.size())) {
                error = "line " + std::to_string(lineNo) + ": bone index out of range";
                return false;
            }
            out.bones[i].parent = inum(t, 2);
            out.bones[i].name = t[3];
            bone = nullptr;
        } else if (k == "BONE" && t.size() == 2 && !vert) {
            int i = inum(t, 1);
            bone = (i >= 0 && i < int(out.bones.size())) ? &out.bones[i] : nullptr;
        } else if (k == "BONE" && vert && t.size() >= 3) {
            vert->weights.push_back({inum(t, 1), num(t, 2)});
        } else if (k == "OFFSET" && (bone || vert)) {
            float* p = vert ? vert->pos : bone->offset;
            p[0] = num(t, 1), p[1] = num(t, 2), p[2] = num(t, 3);
        } else if ((k == "X" || k == "Y" || k == "Z") && bone) {
            int r = k == "X" ? 0 : k == "Y" ? 1 : 2;
            bone->axis[r][0] = num(t, 1), bone->axis[r][1] = num(t, 2), bone->axis[r][2] = num(t, 3);
        } else if (k == "NUMVERTS" || k == "NUMVERTS32") {
            out.verts.resize(static_cast<size_t>(std::max(0, inum(t, 1))));
            bone = nullptr;
        } else if ((k == "VERT" || k == "VERT32") && !face) {
            int i = inum(t, 1);
            if (i < 0 || i >= int(out.verts.size())) {
                error = "line " + std::to_string(lineNo) + ": vertex index out of range";
                return false;
            }
            vert = &out.verts[i];
        } else if (k == "BONES" && vert) {
            // count line; the BONE lines follow
        } else if (k == "NUMFACES") {
            out.faces.reserve(static_cast<size_t>(std::max(0, inum(t, 1))));
            vert = nullptr;
        } else if (k == "TRI" || k == "TRI16") {
            vert = nullptr;
            out.faces.push_back(Face{});
            face = &out.faces.back();
            face->object = inum(t, 1);
            face->material = inum(t, 2);
            faceVert = -1;
        } else if ((k == "VERT" || k == "VERT32") && face) {
            if (++faceVert > 2) {
                error = "line " + std::to_string(lineNo) + ": more than 3 vertices in a TRI";
                return false;
            }
            face->v[faceVert].vert = inum(t, 1);
        } else if (k == "NORMAL" && face && faceVert >= 0) {
            float* n = face->v[faceVert].normal;
            n[0] = num(t, 1), n[1] = num(t, 2), n[2] = num(t, 3);
        } else if (k == "COLOR" && face && faceVert >= 0 && !mat) {
            float* c = face->v[faceVert].color;
            c[0] = num(t, 1), c[1] = num(t, 2), c[2] = num(t, 3), c[3] = t.size() > 4 ? num(t, 4) : 1.0f;
        } else if (k == "UV" && face && faceVert >= 0) {
            // UV <layers> u v [u v ...]: the first layer is the color UV
            face->v[faceVert].uv[0] = num(t, 2);
            face->v[faceVert].uv[1] = num(t, 3);
        } else if (k == "NUMOBJECTS") {
            face = nullptr;
        } else if (k == "OBJECT") {
            out.objects.push_back(t.size() > 2 ? t[2] : "");
        } else if (k == "NUMMATERIALS") {
            face = nullptr;
            out.materials.reserve(static_cast<size_t>(std::max(0, inum(t, 1))));
        } else if (k == "MATERIAL") {
            out.materials.push_back(Material{});
            mat = &out.materials.back();
            mat->name = t.size() > 2 ? t[2] : "";
            mat->type = t.size() > 3 ? t[3] : "";
            if (t.size() > 4) {
                std::string tex = t[4];
                if (tex.rfind("color:", 0) == 0) tex = tex.substr(6);
                mat->texture = tex;
            }
        }
    }
    if (!sawModel && out.version == 0) {
        error = "not an xmodel_export file (no MODEL/VERSION header)";
        return false;
    }
    if (out.bones.empty()) {
        out.bones.push_back(Bone{"tag_origin", -1});
    }
    for (const Face& f : out.faces)
        for (const FaceVertex& v : f.v)
            if (v.vert < 0 || v.vert >= int(out.verts.size())) {
                error = "a face references vertex " + std::to_string(v.vert) + " which doesn't exist";
                return false;
            }
    if (out.faces.empty()) {
        error = "the model has no faces";
        return false;
    }
    return true;
}

std::string format(const Model& m, const std::vector<std::string>& header) {
    std::string o;
    char b[256];
    for (const std::string& h : header) o += "// " + h + "\n";
    o += "MODEL\nVERSION 6\n\n";
    std::snprintf(b, sizeof b, "NUMBONES %zu\n", m.bones.size());
    o += b;
    for (size_t i = 0; i < m.bones.size(); i++) {
        std::snprintf(b, sizeof b, "BONE %zu %d \"%s\"\n", i, m.bones[i].parent, m.bones[i].name.c_str());
        o += b;
    }
    o += "\n";
    for (size_t i = 0; i < m.bones.size(); i++) {
        const Bone& bn = m.bones[i];
        std::snprintf(b, sizeof b, "BONE %zu\nOFFSET %f, %f, %f\nSCALE 1.000000, 1.000000, 1.000000\n", i, bn.offset[0], bn.offset[1],
                      bn.offset[2]);
        o += b;
        for (int r = 0; r < 3; r++) {
            std::snprintf(b, sizeof b, "%c %f, %f, %f\n", "XYZ"[r], bn.axis[r][0], bn.axis[r][1], bn.axis[r][2]);
            o += b;
        }
        o += "\n";
    }
    std::snprintf(b, sizeof b, "NUMVERTS %zu\n", m.verts.size());
    o += b;
    for (size_t i = 0; i < m.verts.size(); i++) {
        const Vertex& v = m.verts[i];
        std::snprintf(b, sizeof b, "VERT %zu\nOFFSET %f, %f, %f\nBONES %zu\n", i, v.pos[0], v.pos[1], v.pos[2], v.weights.size());
        o += b;
        for (const auto& [bone, w] : v.weights) {
            std::snprintf(b, sizeof b, "BONE %d %f\n", bone, w);
            o += b;
        }
        o += "\n";
    }
    std::snprintf(b, sizeof b, "NUMFACES %zu\n", m.faces.size());
    o += b;
    for (const Face& f : m.faces) {
        std::snprintf(b, sizeof b, "TRI %d %d 0 0\n", f.object, f.material);
        o += b;
        for (const FaceVertex& fv : f.v) {
            std::snprintf(b, sizeof b, "VERT %d\nNORMAL %f %f %f\nCOLOR %f %f %f %f\nUV 1 %f %f\n", fv.vert, fv.normal[0], fv.normal[1],
                          fv.normal[2], fv.color[0], fv.color[1], fv.color[2], fv.color[3], fv.uv[0], fv.uv[1]);
            o += b;
        }
        o += "\n";
    }
    std::snprintf(b, sizeof b, "NUMOBJECTS %zu\n", m.objects.size());
    o += b;
    for (size_t i = 0; i < m.objects.size(); i++) {
        std::snprintf(b, sizeof b, "OBJECT %zu \"%s\"\n", i, m.objects[i].c_str());
        o += b;
    }
    o += "\n";
    std::snprintf(b, sizeof b, "NUMMATERIALS %zu\n", m.materials.size());
    o += b;
    for (size_t i = 0; i < m.materials.size(); i++) {
        const Material& mt = m.materials[i];
        o += "MATERIAL " + std::to_string(i) + " \"" + mt.name + "\" \"" + (mt.type.empty() ? "Lambert" : mt.type) + "\" \"" +
             (mt.texture.empty() ? std::string() : "color:" + mt.texture) + "\"\n";
        o += "COLOR 0.000000 0.000000 0.000000 1.000000\nTRANSPARENCY 0.000000 0.000000 0.000000 1.000000\n"
             "AMBIENTCOLOR 0.000000 0.000000 0.000000 1.000000\nINCANDESCENCE 0.000000 0.000000 0.000000 1.000000\n"
             "COEFFS 0.800000 0.000000\nGLOW 0.000000 0\nREFRACTIVE 6 1.000000\nSPECULARCOLOR -1.000000 -1.000000 -1.000000 1.000000\n"
             "REFLECTIVECOLOR -1.000000 -1.000000 -1.000000 1.000000\nREFLECTIVE -1 -1.000000\nBLINN -1.000000 -1.000000\n"
             "PHONG -1.000000\n\n";
    }
    return o;
}

bool save(const Model& m, const std::string& path, std::string& error, const std::vector<std::string>& header) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "can't write " + path;
        return false;
    }
    const std::string text = format(m, header);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

}  // namespace xmodelexport
