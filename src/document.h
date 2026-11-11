#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "brush.h"

struct KeyValue {
    std::string key, value;
};

struct Entity {
    std::vector<KeyValue> kv;
    std::vector<Brush> brushes;
    std::vector<std::string> rawBlocks;  // patches and anything else we don't edit yet

    const std::string* find(const std::string& key) const;
    void set(const std::string& key, const std::string& value);
    std::string classname() const;
    bool origin(Vec3& out) const;
    void setOrigin(const Vec3& o);
};

// brush < 0 selects the entity itself (point entities).
struct Sel {
    int ent = 0;
    int brush = -1;
    bool operator==(const Sel& o) const { return ent == o.ent && brush == o.brush; }
};

struct EntityClass {
    const char* name;
    float r, g, b;
    Vec3 mins, maxs;
    bool brush;
};
const std::vector<EntityClass>& entityClasses();
const EntityClass* findEntityClass(const std::string& name);
AABB pointEntityBounds(const Entity& e);

// Read-only render geometry imported from a compiled map (GfxWorld triangles: terrain, patches,
// detail meshes). Shown in the 3D view as reference; never edited, saved to .map, or compiled.
struct RefMesh {
    struct Batch {
        std::string material;
        uint32_t firstIndex = 0, indexCount = 0;
    };
    std::vector<float> positions;  // xyz per vertex
    std::vector<float> normals;    // xyz per vertex
    std::vector<float> uvs;        // uv per vertex
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    AABB bounds;
    // material (editor name) -> colorMap image, read from the zone's own Material structs
    std::map<std::string, std::string> materialImages;
    // Live-dump bookkeeping: geometry dumped from the running game is appended after this many
    // vertices/indices/batches, so a repeat dump can strip the previous one and replace it instead
    // of stacking. hasLiveDump is false when no dumped models are present.
    bool hasLiveDump = false;
    size_t liveVertStart = 0, liveIndexStart = 0, liveBatchStart = 0;
};

class Document {
public:
    std::vector<Entity> ents;
    std::vector<Sel> sel;
    std::vector<std::string> header;  // lines between "iwmap 4" and the first entity
    std::string path;
    bool dirty = false;
    uint64_t revision = 1;  // bumps on any change that affects drawing
    std::shared_ptr<const RefMesh> refMesh;  // imported reference geometry (optional)

    Document() { reset(); }
    void reset();

    void touch() { revision++; dirty = true; }
    void touchSelection() { revision++; }

    void pushUndo();
    bool undo();
    bool redo();
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    // For text fields: capture on focus, commit only if the value actually changed.
    void armUndo();
    void commitArmedUndo();

    bool isSelected(const Sel& s) const;
    void select(const Sel& s, bool additive);
    void clearSelection();
    AABB selectionBounds() const;

    Sel addBrush(Brush b);
    Sel addPointEntity(const std::string& classname, const Vec3& origin);
    void deleteSelected();
    void cloneSelected(const Vec3& offset);
    void translateSelected(const Vec3& d);
    bool hollowSelected(double wall);
    void setSelectedMaterial(const std::string& name);
    void selectedToEntity(const std::string& classname);

private:
    struct Snapshot {
        std::vector<Entity> ents;
        std::vector<Sel> sel;
    };
    static constexpr size_t kMaxUndo = 64;
    std::deque<Snapshot> undoStack, redoStack;
    Snapshot armed;
    bool hasArmed = false;
};
