#include "document.h"
#include <algorithm>
#include <cstdio>

const std::string* Entity::find(const std::string& key) const {
    for (const KeyValue& k : kv)
        if (k.key == key) return &k.value;
    return nullptr;
}

void Entity::set(const std::string& key, const std::string& value) {
    for (KeyValue& k : kv)
        if (k.key == key) { k.value = value; return; }
    kv.push_back({key, value});
}

std::string Entity::classname() const {
    const std::string* s = find("classname");
    return s ? *s : std::string();
}

bool Entity::origin(Vec3& out) const {
    const std::string* s = find("origin");
    return s && std::sscanf(s->c_str(), "%lf %lf %lf", &out.x, &out.y, &out.z) == 3;
}

void Entity::setOrigin(const Vec3& o) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.6g %.6g %.6g", o.x, o.y, o.z);
    set("origin", buf);
}

const std::vector<EntityClass>& entityClasses() {
    static const Vec3 pm{-16, -16, 0}, px{16, 16, 72}, sm{-8, -8, -8}, sx{8, 8, 8};
    static const std::vector<EntityClass> list = {
        {"mp_tdm_spawn", 0.2f, 0.9f, 0.3f, pm, px, false},
        {"mp_tdm_spawn_allies_start", 0.2f, 0.6f, 1.0f, pm, px, false},
        {"mp_tdm_spawn_axis_start", 1.0f, 0.4f, 0.2f, pm, px, false},
        {"mp_dm_spawn", 0.9f, 0.9f, 0.2f, pm, px, false},
        {"mp_dom_spawn", 0.3f, 0.9f, 0.8f, pm, px, false},
        {"mp_sd_spawn_attacker", 1.0f, 0.3f, 0.3f, pm, px, false},
        {"mp_sd_spawn_defender", 0.3f, 0.5f, 1.0f, pm, px, false},
        {"mp_global_intermission", 0.9f, 0.3f, 0.9f, pm, px, false},
        {"light", 1.0f, 0.9f, 0.5f, sm, sx, false},
        {"script_origin", 0.6f, 0.6f, 1.0f, sm, sx, false},
        {"script_struct", 0.6f, 0.8f, 1.0f, sm, sx, false},
        {"script_model", 0.9f, 0.6f, 0.2f, {-16, -16, -16}, {16, 16, 16}, false},
        {"misc_model", 0.8f, 0.6f, 0.3f, {-16, -16, -16}, {16, 16, 16}, false},
        {"trigger_multiple", 0.9f, 0.5f, 0.1f, {}, {}, true},
        {"trigger_radius", 0.9f, 0.5f, 0.1f, {-16, -16, 0}, {16, 16, 64}, false},
        {"script_brushmodel", 0.3f, 0.7f, 0.9f, {}, {}, true},
    };
    return list;
}

const EntityClass* findEntityClass(const std::string& name) {
    for (const EntityClass& c : entityClasses())
        if (name == c.name) return &c;
    return nullptr;
}

AABB pointEntityBounds(const Entity& e) {
    Vec3 o;
    e.origin(o);
    Vec3 mn{-8, -8, -8}, mx{8, 8, 8};
    if (const EntityClass* c = findEntityClass(e.classname()); c && !c->brush) {
        mn = c->mins;
        mx = c->maxs;
    }
    AABB b;
    b.add(o + mn);
    b.add(o + mx);
    return b;
}

void Document::reset() {
    ents.clear();
    Entity world;
    world.set("classname", "worldspawn");
    ents.push_back(std::move(world));
    sel.clear();
    header = {"\"000_Global\" flags  active", "\"The Map\" flags  "};
    path.clear();
    refMesh.reset();
    undoStack.clear();
    redoStack.clear();
    hasArmed = false;
    dirty = false;
    revision++;
}

void Document::pushUndo() {
    undoStack.push_back({ents, sel});
    if (undoStack.size() > kMaxUndo) undoStack.pop_front();
    redoStack.clear();
}

bool Document::undo() {
    if (undoStack.empty()) return false;
    redoStack.push_back({std::move(ents), std::move(sel)});
    ents = std::move(undoStack.back().ents);
    sel = std::move(undoStack.back().sel);
    undoStack.pop_back();
    touch();
    return true;
}

bool Document::redo() {
    if (redoStack.empty()) return false;
    undoStack.push_back({std::move(ents), std::move(sel)});
    ents = std::move(redoStack.back().ents);
    sel = std::move(redoStack.back().sel);
    redoStack.pop_back();
    touch();
    return true;
}

void Document::armUndo() {
    armed = {ents, sel};
    hasArmed = true;
}

void Document::commitArmedUndo() {
    if (!hasArmed) return;
    undoStack.push_back(std::move(armed));
    if (undoStack.size() > kMaxUndo) undoStack.pop_front();
    redoStack.clear();
    hasArmed = false;
}

bool Document::isSelected(const Sel& s) const {
    return std::find(sel.begin(), sel.end(), s) != sel.end();
}

void Document::select(const Sel& s, bool additive) {
    if (!additive) sel.clear();
    auto it = std::find(sel.begin(), sel.end(), s);
    if (it != sel.end()) sel.erase(it);
    else sel.push_back(s);
    touchSelection();
}

void Document::clearSelection() {
    if (sel.empty()) return;
    sel.clear();
    touchSelection();
}

AABB Document::selectionBounds() const {
    AABB b;
    for (const Sel& s : sel) {
        if (s.brush >= 0) b.add(ents[s.ent].brushes[s.brush].bounds);
        else b.add(pointEntityBounds(ents[s.ent]));
    }
    return b;
}

Sel Document::addBrush(Brush b) {
    ents[0].brushes.push_back(std::move(b));
    touch();
    return {0, static_cast<int>(ents[0].brushes.size()) - 1};
}

Sel Document::addPointEntity(const std::string& classname, const Vec3& origin) {
    Entity e;
    e.set("classname", classname);
    e.setOrigin(origin);
    ents.push_back(std::move(e));
    touch();
    return {static_cast<int>(ents.size()) - 1, -1};
}

void Document::deleteSelected() {
    if (sel.empty()) return;
    // Erases from the back so earlier indices stay valid.
    std::vector<Sel> order = sel;
    std::sort(order.begin(), order.end(), [](const Sel& a, const Sel& b) {
        return a.ent != b.ent ? a.ent > b.ent : a.brush > b.brush;
    });
    for (const Sel& s : order) {
        Entity& e = ents[s.ent];
        if (s.brush >= 0) {
            e.brushes.erase(e.brushes.begin() + s.brush);
            if (s.ent != 0 && e.brushes.empty() && e.rawBlocks.empty()) ents.erase(ents.begin() + s.ent);
        } else if (s.ent != 0) {
            ents.erase(ents.begin() + s.ent);
        }
    }
    sel.clear();
    touch();
}

void Document::cloneSelected(const Vec3& offset) {
    std::vector<Sel> copies;
    for (const Sel& s : sel) {
        if (s.brush >= 0) {
            Brush b = ents[s.ent].brushes[s.brush];
            b.translate(offset);
            ents[s.ent].brushes.push_back(std::move(b));
            copies.push_back({s.ent, static_cast<int>(ents[s.ent].brushes.size()) - 1});
        } else if (s.ent != 0) {
            Entity e = ents[s.ent];
            Vec3 o;
            e.origin(o);
            e.setOrigin(o + offset);
            ents.push_back(std::move(e));
            copies.push_back({static_cast<int>(ents.size()) - 1, -1});
        }
    }
    sel = std::move(copies);
    touch();
}

void Document::translateSelected(const Vec3& d) {
    for (const Sel& s : sel) {
        if (s.brush >= 0) {
            ents[s.ent].brushes[s.brush].translate(d);
        } else {
            Vec3 o;
            ents[s.ent].origin(o);
            ents[s.ent].setOrigin(o + d);
        }
    }
    touch();
}

bool Document::hollowSelected(double wall) {
    std::vector<Sel> brushes;
    for (const Sel& s : sel) {
        if (s.brush < 0) continue;
        const AABB& b = ents[s.ent].brushes[s.brush].bounds;
        for (int i = 0; i < 3; i++)
            if (b.maxs[i] - b.mins[i] <= wall * 2) return false;  // no room left inside
        brushes.push_back(s);
    }
    if (brushes.empty()) return false;
    std::sort(brushes.begin(), brushes.end(), [](const Sel& a, const Sel& b) {
        return a.ent != b.ent ? a.ent > b.ent : a.brush > b.brush;
    });
    std::vector<Sel> created;
    for (const Sel& s : brushes) {
        Entity& e = ents[s.ent];
        AABB box = e.brushes[s.brush].bounds;
        std::string mat = e.brushes[s.brush].faces[0].material;
        e.brushes.erase(e.brushes.begin() + s.brush);
        for (Sel& c : created)
            if (c.ent == s.ent && c.brush > s.brush) c.brush--;
        for (int axis = 0; axis < 3; axis++) {
            for (int side = 0; side < 2; side++) {
                Vec3 mn = box.mins, mx = box.maxs;
                if (side == 0) mx[axis] = mn[axis] + wall;
                else mn[axis] = mx[axis] - wall;
                e.brushes.push_back(makeBox(mn, mx, mat));
                created.push_back({s.ent, static_cast<int>(e.brushes.size()) - 1});
            }
        }
    }
    sel = std::move(created);
    touch();
    return true;
}

void Document::setSelectedMaterial(const std::string& name) {
    for (const Sel& s : sel)
        if (s.brush >= 0) ents[s.ent].brushes[s.brush].setMaterial(name);
    touch();
}

void Document::selectedToEntity(const std::string& classname) {
    std::vector<Sel> order;
    for (const Sel& s : sel)
        if (s.brush >= 0 && s.ent == 0) order.push_back(s);
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [](const Sel& a, const Sel& b) { return a.brush > b.brush; });
    Entity e;
    e.set("classname", classname);
    for (const Sel& s : order) {
        e.brushes.push_back(std::move(ents[0].brushes[s.brush]));
        ents[0].brushes.erase(ents[0].brushes.begin() + s.brush);
    }
    ents.push_back(std::move(e));
    sel.clear();
    int idx = static_cast<int>(ents.size()) - 1;
    for (int i = 0; i < static_cast<int>(ents[idx].brushes.size()); i++) sel.push_back({idx, i});
    touch();
}
