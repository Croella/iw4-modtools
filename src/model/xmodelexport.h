#pragma once
#include <string>
#include <vector>

// Reader for xmodel_export files (the text format written by the Maya/Blender Call of Duty
// export plugins), versions 6 and 7.
namespace xmodelexport {

struct Bone {
    std::string name;
    int parent = -1;
    float offset[3] = {};
    float axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // X, Y, Z rows
};
struct Vertex {
    float pos[3] = {};
    std::vector<std::pair<int, float>> weights;  // bone, weight
};
struct FaceVertex {
    int vert = 0;
    float normal[3] = {0, 0, 1};
    float color[4] = {1, 1, 1, 1};
    float uv[2] = {};
};
struct Face {
    int object = 0, material = 0;
    FaceVertex v[3];
};
struct Material {
    std::string name;
    std::string type;     // "Lambert", "Phong", ...
    std::string texture;  // color texture path as exported ("" if none)
};

struct Model {
    int version = 0;
    std::vector<Bone> bones;
    std::vector<Vertex> verts;
    std::vector<Face> faces;
    std::vector<std::string> objects;
    std::vector<Material> materials;
};

bool load(const std::string& path, Model& out, std::string& error);
bool parse(const std::string& text, Model& out, std::string& error);

// Writes a model back out as a version 6 xmodel_export (what this reader and the stock tools
// read): bones (absolute bind pose), weighted vertices, faces, objects, materials. `header` lines
// are written first as // comments.
std::string format(const Model& m, const std::vector<std::string>& header = {});
bool save(const Model& m, const std::string& path, std::string& error, const std::vector<std::string>& header = {});

}  // namespace xmodelexport
