#include "zone/efx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace efx {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;

// ---- tokenizer / statement tree ----
struct Tok {
    std::string s;
    bool quoted = false;
};
std::vector<Tok> tokenize(const std::string& text) {
    std::vector<Tok> out;
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            i++;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') i++;
        } else if (c == '"') {
            size_t e = text.find('"', i + 1);
            if (e == std::string::npos) e = text.size();
            out.push_back({text.substr(i + 1, e - i - 1), true});
            i = e + 1;
        } else if (c == '{' || c == '}' || c == ';') {
            out.push_back({std::string(1, c), false});
            i++;
        } else {
            size_t s = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '{' && text[i] != '}' &&
                   text[i] != ';' && text[i] != '"')
                i++;
            out.push_back({text.substr(s, i - s), false});
        }
    }
    return out;
}

// A brace block: plain tokens and nested blocks, in order.
struct Block {
    std::vector<Tok> toks;
    std::vector<Block> blocks;
};
// A statement: key, leading tokens, blocks (all of them until ';').
struct Stmt {
    std::string key;
    std::vector<Tok> args;
    std::vector<Block> blocks;
};

struct Parser {
    const std::vector<Tok>& t;
    size_t i = 0;
    std::string err;
    bool at(const char* s) const { return i < t.size() && !t[i].quoted && t[i].s == s; }
    bool block(Block& b) {  // at '{'
        i++;
        while (i < t.size() && !at("}")) {
            if (at("{")) {
                b.blocks.emplace_back();
                if (!block(b.blocks.back())) return false;
            } else {
                b.toks.push_back(t[i++]);
            }
        }
        if (i >= t.size()) {
            err = "unterminated '{'";
            return false;
        }
        i++;
        return true;
    }
    // Statements until the closing '}' of an element (or end).
    bool statements(std::vector<Stmt>& out) {
        while (i < t.size() && !at("}")) {
            if (at(";")) {
                i++;
                continue;
            }
            Stmt s;
            s.key = t[i++].s;
            while (i < t.size() && !at(";") && !at("}")) {
                if (at("{")) {
                    s.blocks.emplace_back();
                    if (!block(s.blocks.back())) return false;
                } else {
                    s.args.push_back(t[i++]);
                }
            }
            if (at(";")) i++;
            out.push_back(std::move(s));
        }
        return true;
    }
};

float toF(const std::string& s) { return static_cast<float>(std::strtod(s.c_str(), nullptr)); }

// "{ {t v ...} {t v ...} }" with `width` values per key.
bool readGraph(const Stmt& s, int width, Graph& g) {
    g.present = true;
    g.scale = s.args.empty() ? 1.0f : toF(s.args[0].s);
    if (s.blocks.empty()) return false;
    const Block& outer = s.blocks[0];
    for (int c = 0; c < 2 && c < static_cast<int>(outer.blocks.size()); c++) {
        const auto& toks = outer.blocks[c].toks;
        Curve& cv = g.curve[c];
        for (size_t k = 0; k + 1 + width <= toks.size(); k += 1 + width) {
            cv.t.push_back(toF(toks[k].s));
            std::vector<float> v;
            for (int j = 0; j < width; j++) v.push_back(toF(toks[k + 1 + j].s));
            cv.v.push_back(v);
        }
    }
    if (g.curve[1].t.empty()) g.curve[1] = g.curve[0];
    return !g.curve[0].t.empty();
}

const char* kVisualTypes[] = {"billboardSprite", "orientedSprite", "tail",  "trail",  "cloud",  "sparkCloud", "sparkFountain",
                              "model",           "light",          "spotLight", "sound", "decal", "runner"};

}  // namespace

std::vector<float> Curve::eval(float time) const {
    if (t.empty()) return {0, 0, 0};
    if (time <= t.front()) return v.front();
    if (time >= t.back()) return v.back();
    size_t k = 1;
    while (k < t.size() && t[k] < time) k++;
    const float span = t[k] - t[k - 1];
    const float f = span > 1e-9f ? (time - t[k - 1]) / span : 0.0f;
    std::vector<float> r(v[k].size());
    for (size_t j = 0; j < r.size(); j++) r[j] = v[k - 1][j] + (v[k][j] - v[k - 1][j]) * f;
    return r;
}

float Element::num(const std::string& key, int i, float def) const {
    auto it = keys.find(key);
    if (it == keys.end() || static_cast<int>(it->second.size()) <= i) return def;
    return toF(it->second[i]);
}

bool parse(const std::string& text, Effect& out, std::string& error) {
    out = Effect{};
    std::vector<Tok> toks = tokenize(text);
    Parser p{toks};
    if (toks.size() >= 2 && toks[0].s == "iwfx") {
        out.version = std::atoi(toks[1].s.c_str());
        p.i = 2;
    } else {
        error = "not an effect file (expected 'iwfx <version>')";
        return false;
    }
    while (p.i < toks.size()) {
        if (!p.at("{")) {  // effect-level settings (efFlags, efPriority, ...): skip to ';'
            while (p.i < toks.size() && !p.at(";") && !p.at("{")) p.i++;
            if (p.at(";")) p.i++;
            continue;
        }
        p.i++;
        std::vector<Stmt> st;
        if (!p.statements(st)) {
            error = p.err;
            return false;
        }
        if (p.at("}")) p.i++;
        Element e;
        for (const Stmt& s : st) {
            const std::string& k = s.key;
            if (k == "name") {
                if (!s.args.empty()) e.name = s.args[0].s;
            } else if (k == "editorFlags") {
                for (const Tok& t : s.args) e.editorFlags.insert(t.s);
            } else if (k == "flags") {
                for (const Tok& t : s.args) e.flags.insert(t.s);
            } else if (k.size() == 10 && k.compare(0, 8, "velGraph") == 0 && (k[8] == '0' || k[8] == '1') && k[9] >= 'X' && k[9] <= 'Z') {
                readGraph(s, 1, e.vel[k[8] - '0'][k[9] - 'X']);  // velGraph0X .. velGraph1Z
            } else if (k == "rotGraph") {
                readGraph(s, 1, e.rot);
            } else if (k == "sizeGraph0") {
                readGraph(s, 1, e.size[0]);
            } else if (k == "sizeGraph1") {
                readGraph(s, 1, e.size[1]);
            } else if (k == "scaleGraph") {
                readGraph(s, 1, e.scale);
            } else if (k == "colorGraph") {
                readGraph(s, 3, e.color);
            } else if (k == "alphaGraph") {
                readGraph(s, 1, e.alpha);
            } else if (k == "trailDef") {
                if (s.blocks.size() >= 1)
                    for (const Tok& t : s.blocks[0].toks) e.trailVerts.push_back(toF(t.s));
                if (s.blocks.size() >= 2)
                    for (const Tok& t : s.blocks[1].toks) e.trailInds.push_back(std::atoi(t.s.c_str()));
            } else if (std::find(std::begin(kVisualTypes), std::end(kVisualTypes), k) != std::end(kVisualTypes) ||
                       k == "rotatedSprite" || k == "line") {
                e.visualType = k;
                for (const Block& b : s.blocks)
                    for (const Tok& t : b.toks)
                        if (!t.s.empty()) e.visuals.push_back(t.s);
            } else if (!s.blocks.empty()) {
                // other blocks (elemSpawnSound, ...): not used
            } else {
                std::vector<std::string> v;
                for (const Tok& t : s.args) v.push_back(t.s);
                e.keys[k] = std::move(v);
            }
        }
        out.elems.push_back(std::move(e));
    }
    return true;
}

namespace {

void putF(uint8_t* p, float v) { std::memcpy(p, &v, 4); }
void putI(uint8_t* p, int32_t v) { std::memcpy(p, &v, 4); }
void putRange(uint8_t* p, float base, float amp) {
    putF(p, base);
    putF(p + 4, amp);
}

// Graph value at time t for one curve (scale applied).
float g1(const Graph& g, int curve, float t) { return g.present ? g.curve[curve].eval(t)[0] * g.scale : 0.0f; }

// Intervals needed so linear interpolation between uniform samples follows every graph to within
// a small error (relative to each graph's range), 1..maxN.
int intervalsFor(const std::vector<std::function<float(float)>>& fns, int maxN) {
    std::vector<float> lo(fns.size(), 1e30f), hi(fns.size(), -1e30f);
    constexpr int kProbe = 256;
    std::vector<std::vector<float>> truth(fns.size(), std::vector<float>(kProbe + 1));
    for (size_t f = 0; f < fns.size(); f++)
        for (int k = 0; k <= kProbe; k++) {
            float v = fns[f](static_cast<float>(k) / kProbe);
            truth[f][k] = v;
            lo[f] = std::min(lo[f], v);
            hi[f] = std::max(hi[f], v);
        }
    for (int n = 1; n < maxN; n++) {
        bool ok = true;
        for (size_t f = 0; f < fns.size() && ok; f++) {
            const float tol = std::max(1e-4f, 0.02f * (hi[f] - lo[f]));
            std::vector<float> s(n + 1);
            for (int i = 0; i <= n; i++) s[i] = fns[f](static_cast<float>(i) / n);
            for (int k = 0; k <= kProbe && ok; k++) {
                float x = static_cast<float>(k) / kProbe * n;
                int i = std::min(static_cast<int>(x), n - 1);
                float approx = s[i] + (s[i + 1] - s[i]) * (x - i);
                if (std::fabs(approx - truth[f][k]) > tol) ok = false;
            }
        }
        if (ok) return n;
    }
    return maxN;
}

uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L)); }

bool graphNonZero(const Graph& g) {
    if (!g.present || g.scale == 0) return false;
    for (int c = 0; c < 2; c++)
        for (const auto& v : g.curve[c].v)
            if (v[0] != 0) return true;
    return false;
}

}  // namespace

bool compile(const Effect& fx, const std::string& name, const MaterialLookup& materials, Compiled& out,
             std::vector<std::string>& warnings, std::string& error) {
    out = Compiled{};
    out.name = name;
    std::vector<const Element*> looping, oneShot;
    for (const Element& e : fx.elems) {
        if (e.editorFlags.count("playOnTouch") || e.editorFlags.count("playOnDeath") || e.editorFlags.count("playOnRun")) {
            warnings.push_back(name + ": element '" + e.name + "' plays on touch/death/run, which isn't supported yet; left out");
            continue;
        }
        if (e.visualType.empty()) {
            warnings.push_back(name + ": element '" + e.name + "' has no visual; left out");
            continue;
        }
        (e.editorFlags.count("looping") ? looping : oneShot).push_back(&e);
    }
    std::vector<const Element*> order = looping;
    order.insert(order.end(), oneShot.begin(), oneShot.end());
    out.countLooping = static_cast<int32_t>(looping.size());
    out.countOneShot = static_cast<int32_t>(oneShot.size());
    if (order.empty()) {
        error = name + ": no elements this compiler can build";
        return false;
    }
    bool needsLighting = false, infiniteLoop = false;
    int loopLife = 0, keptLooping = 0;
    size_t samplesBytes = 0, visualBytes = 0;
    for (const Element* ep : order) {
        const Element& e = *ep;
        ElemOut o;
        uint8_t* d = o.def.data();
        // type
        std::string vt = e.visualType;
        if (vt == "rotatedSprite") {
            warnings.push_back(name + ": '" + e.name + "' is a BO1 rotatedSprite; built as a billboard");
            vt = "billboardSprite";
        } else if (vt == "line") {
            warnings.push_back(name + ": '" + e.name + "' is a BO1 line; built as a tail");
            vt = "tail";
        }
        int type = 0;
        while (type < 13 && vt != kVisualTypes[type]) type++;
        if (type == 13) {
            error = name + ": unknown visual type " + vt;
            return false;
        }
        if (type == 5 || type == 6) {
            warnings.push_back(name + ": '" + e.name + "' spark cloud/fountain isn't supported; left out");
            continue;
        }
        o.type = static_cast<uint8_t>(type);
        const bool isLight = type == 8 || type == 9;
        const bool hasMaterial = type <= 4 || type == 11;
        // flags (FX_ELEM_*, iw4 enum)
        uint32_t flags = 0;
        static const std::pair<const char*, uint32_t> kFlags[] = {
            {"spawnRelative", 0x2},      {"spawnFrustumCull", 0x4},     {"runnerUsesRandRot", 0x8}, {"spawnOffsetSphere", 0x10},
            {"spawnOffsetCylinder", 0x20}, {"runRelToSpawn", 0x40},     {"runRelToEffect", 0x80},   {"runRelToOffset", 0xC0},
            {"useCollision", 0x100},     {"dieOnTouch", 0x200},         {"drawPastFog", 0x400},     {"drawWithViewModel", 0x800},
            {"blocksSight", 0x1000},     {"drawInThermalViewOnly", 0x2000}, {"trailOrientByVelocity", 0x4000},
            {"emitOrientByElem", 0x8000}, {"modelUsesPhysics", 0x8000000}, {"nonUniformScale", 0x10000000},
            {"cloudShapeSphereLarge", 0x20000000}, {"cloudShapeSphereMedium", 0x40000000}, {"cloudShapeSphereSmall", 0x60000000},
            {"fountainDisableCollision", 0x80000000u},
        };
        for (const auto& [n, bit] : kFlags)
            if (e.flags.count(n)) flags |= bit;
        bool localVel = false, worldVel = false;
        for (int ax = 0; ax < 3; ax++) {
            localVel |= graphNonZero(e.vel[0][ax]);
            worldVel |= graphNonZero(e.vel[1][ax]);
        }
        if (localVel) flags |= 0x1000000;
        if (worldVel) flags |= 0x2000000;
        const float gravBase = e.num("gravity", 0) / 100.0f, gravAmp = e.num("gravity", 1) / 100.0f;
        if (gravBase != 0 || gravAmp != 0) flags |= 0x4000000;
        putI(d + 0, static_cast<int32_t>(flags));
        // spawn
        if (e.editorFlags.count("looping")) {
            const int interval = static_cast<int>(e.num("spawnLooping", 0, 200)), count = static_cast<int>(e.num("spawnLooping", 1, 0));
            putI(d + 4, interval);
            // count 0 in the .efx = forever, which the game stores as INT_MAX (stock
            // fire/propane_small_fire, fire/firelp_med_pm...): a 0 count spawns nothing more
            putI(d + 8, count <= 0 ? 0x7FFFFFFF : count);
            if (count <= 0)
                infiniteLoop = true;
            else
                loopLife = std::max(loopLife, interval * (count - 1));
        } else {
            putI(d + 4, static_cast<int>(e.num("spawnOneShot", 0, 1)));
            putI(d + 8, static_cast<int>(e.num("spawnOneShot", 1, 0)));
        }
        putRange(d + 12, e.num("spawnRange", 0), e.num("spawnRange", 1));
        putRange(d + 20, e.num("fadeInRange", 0), e.num("fadeInRange", 1));
        putRange(d + 28, e.num("fadeOutRange", 0), e.num("fadeOutRange", 1));
        putF(d + 36, e.num("spawnFrustumCullRadius", 0));
        putI(d + 40, static_cast<int>(e.num("spawnDelayMsec", 0)));
        putI(d + 44, static_cast<int>(e.num("spawnDelayMsec", 1)));
        const int lifeBase = static_cast<int>(e.num("lifeSpanMsec", 0, 1000)), lifeAmp = static_cast<int>(e.num("lifeSpanMsec", 1));
        putI(d + 48, lifeBase);
        putI(d + 52, lifeAmp);
        const char* org[3] = {"spawnOrgX", "spawnOrgY", "spawnOrgZ"};
        for (int k = 0; k < 3; k++) putRange(d + 56 + 8 * k, e.num(org[k], 0), e.num(org[k], 1));
        putRange(d + 80, e.num("spawnOffsetRadius", 0), e.num("spawnOffsetRadius", 1));
        putRange(d + 88, e.num("spawnOffsetHeight", 0), e.num("spawnOffsetHeight", 1));
        const char* ang[3] = {"spawnAnglePitch", "spawnAngleYaw", "spawnAngleRoll"};
        for (int k = 0; k < 3; k++) putRange(d + 96 + 8 * k, e.num(ang[k], 0) * kDegToRad, e.num(ang[k], 1) * kDegToRad);
        const char* avel[3] = {"angleVelPitch", "angleVelYaw", "angleVelRoll"};
        for (int k = 0; k < 3; k++)
            putRange(d + 120 + 8 * k, e.num(avel[k], 0) * kDegToRad / 1000.0f, e.num(avel[k], 1) * kDegToRad / 1000.0f);
        putRange(d + 144, e.num("initialRot", 0) * kDegToRad, e.num("initialRot", 1) * kDegToRad);
        putRange(d + 152, gravBase, gravAmp);
        putRange(d + 160, e.num("elasticity", 0), e.num("elasticity", 1));
        // visuals (+ the material's atlas)
        MaterialInfo mi;
        for (const std::string& v : e.visuals) {
            Visual vis;
            vis.name = v;
            if (hasMaterial) {
                vis.kind = Visual::Material;
                MaterialInfo m = materials ? materials(v) : MaterialInfo{};
                if (!m.known) {
                    warnings.push_back(name + ": material '" + v + "' not found (zone_source, the game's common zone, Black Ops); left out");
                    continue;
                }
                if (!m.replacement.empty()) vis.name = m.replacement;
                if (o.visuals.empty()) mi = m;
            } else if (type == 7) {
                vis.kind = Visual::Model;
            } else if (type == 10 || type == 12) {
                vis.kind = Visual::Name;
                if (type == 12) out.dependencies.push_back(v);
            }
            o.visuals.push_back(vis);
            if (type == 11) o.visuals.push_back(vis);  // decal: world + model material (same)
        }
        if (!isLight && o.visuals.empty()) {
            warnings.push_back(name + ": element '" + e.name + "' has an empty visual list; left out");
            continue;
        }
        // atlas (FxElemAtlas at 168)
        if (hasMaterial) {
            uint8_t behavior = 0;
            const auto it = e.keys.find("atlasBehavior");
            if (it != e.keys.end())
                for (const std::string& b : it->second) {
                    if (b == "startRandom") behavior |= 1;
                    if (b == "startIndexed") behavior |= 2;
                    if (b == "playOverLife") behavior |= 4;
                    if (b == "loopOnlyNTimes") behavior |= 8;
                }
            int cols = mi.cols, rows = mi.rows;
            int colBits = 0, rowBits = 0;
            while ((1 << colBits) < cols) colBits++;
            while ((1 << rowBits) < rows) rowBits++;
            if (e.num("atlasColIndexBits") > 0) colBits = static_cast<int>(e.num("atlasColIndexBits"));
            if (e.num("atlasRowIndexBits") > 0) rowBits = static_cast<int>(e.num("atlasRowIndexBits"));
            int entries = e.num("atlasEntryCount") > 0 ? static_cast<int>(e.num("atlasEntryCount")) : cols * rows;
            d[168] = behavior;
            d[169] = static_cast<uint8_t>(e.num("atlasIndex"));
            d[170] = static_cast<uint8_t>(std::clamp(static_cast<int>(e.num("atlasFps")), 0, 255));
            d[171] = static_cast<uint8_t>(std::max(1, static_cast<int>(e.num("atlasLoopCount"))));
            d[172] = static_cast<uint8_t>(colBits);
            d[173] = static_cast<uint8_t>(rowBits);
            d[174] = static_cast<uint8_t>(entries & 0xFF);
            d[175] = static_cast<uint8_t>(entries >> 8);
        }
        d[176] = o.type;
        const int visualCount = type == 11 ? static_cast<int>(o.visuals.size() / 2) : static_cast<int>(o.visuals.size());
        d[177] = static_cast<uint8_t>(isLight ? 1 : visualCount);
        if (visualCount > 1 && type != 11) visualBytes += 8ull * visualCount;
        if (type == 11) visualBytes += 16ull * visualCount;

        // velocity samples
        const bool rv[2] = {e.editorFlags.count("useRandVel0") != 0, e.editorFlags.count("useRandVel1") != 0};
        std::vector<std::function<float(float)>> velFns;
        for (int g = 0; g < 2; g++)
            for (int ax = 0; ax < 3; ax++) {
                const Graph& gr = e.vel[g][ax];
                velFns.push_back([&gr](float t) { return g1(gr, 0, t); });
                if (rv[g]) velFns.push_back([&gr](float t) { return g1(gr, 1, t); });
            }
        const int velN = (localVel || worldVel) ? intervalsFor(velFns, 32) : 1;
        d[178] = static_cast<uint8_t>(velN);
        o.velSamples.assign(96ull * (velN + 1), 0);
        {
            const float k = 1.0f / (1000.0f * velN);
            std::vector<float> prev(24, 0.0f);
            for (int i = 0; i <= velN; i++) {
                const float t = static_cast<float>(i) / velN;
                float s[24] = {};
                for (int g = 0; g < 2; g++)
                    for (int ax = 0; ax < 3; ax++) {
                        const Graph& gr = e.vel[g][ax];
                        const float base = g1(gr, 0, t) * k;
                        const float amp = rv[g] ? g1(gr, 1, t) * k - base : 0.0f;
                        s[12 * g + ax] = base;
                        s[12 * g + 3 + ax] = amp;
                    }
                for (int g = 0; g < 2; g++)
                    for (int j = 0; j < 6; j++) {
                        const int vi = 12 * g + j, di = 12 * g + 6 + j;
                        s[di] = i == 0 ? 0.0f : prev[di] + (prev[vi] + s[vi]) * 0.5f;
                    }
                std::memcpy(o.velSamples.data() + 96 * i, s, 96);
                prev.assign(s, s + 24);
            }
        }
        // visual-state samples
        const bool rc = e.editorFlags.count("useRandColor") != 0, ra = e.editorFlags.count("useRandAlpha") != 0;
        const bool rs0 = e.editorFlags.count("useRandSize0") != 0, rs1 = e.editorFlags.count("useRandSize1") != 0;
        const bool rsc = e.editorFlags.count("useRandScale") != 0, rr = e.editorFlags.count("useRandRotDelta") != 0;
        const bool modByAlpha = e.editorFlags.count("modColorByAlpha") != 0;
        const bool nonUniform = (flags & 0x10000000) != 0;
        std::vector<std::function<float(float)>> visFns;
        for (int c = 0; c < 2; c++) {
            for (int j = 0; j < 3; j++)
                visFns.push_back([&e, c, j](float t) { return e.color.present ? e.color.curve[c].eval(t)[j] : 1.0f; });
            visFns.push_back([&e, c](float t) { return e.alpha.present ? e.alpha.curve[c].eval(t)[0] : 1.0f; });
            visFns.push_back([&e, c](float t) { return g1(e.size[0], c, t); });
            visFns.push_back([&e, c](float t) { return g1(e.size[1], c, t); });
            visFns.push_back([&e, c](float t) { return g1(e.scale, c, t); });
            visFns.push_back([&e, c](float t) { return g1(e.rot, c, t); });
        }
        const int visN = intervalsFor(visFns, 32);
        d[179] = static_cast<uint8_t>(visN);
        o.visSamples.assign(48ull * (visN + 1), 0);
        {
            const float rk = kDegToRad / (1000.0f * visN);
            float prevRot[2] = {0, 0}, prevTot[2] = {0, 0};
            for (int i = 0; i <= visN; i++) {
                const float t = static_cast<float>(i) / visN;
                uint8_t* s = o.visSamples.data() + 48 * i;
                for (int c = 0; c < 2; c++) {  // c 0: base state, 1: amplitude state
                    uint8_t* st = s + 24 * c;
                    const int cc = c == 1 && rc ? 1 : 0, ac = c == 1 && ra ? 1 : 0;
                    std::vector<float> rgb = e.color.present ? e.color.curve[cc].eval(t) : std::vector<float>{1, 1, 1};
                    const float scaleC = e.color.present ? e.color.scale : 1.0f;
                    float a = e.alpha.present ? e.alpha.curve[ac].eval(t)[0] * e.alpha.scale : 1.0f;
                    for (float& x : rgb) x *= scaleC;
                    if (modByAlpha) {  // stock: rgb premultiplied, alpha byte 255 (saw_flash_wv_a flares)
                        for (float& x : rgb) x *= a;
                        a = 1.0f;
                    }
                    st[0] = toByte(rgb[2]);  // B G R A
                    st[1] = toByte(rgb[1]);
                    st[2] = toByte(rgb[0]);
                    st[3] = toByte(a);
                }
                // floats: rotationDelta, rotationTotal, size[2], scale (base at +4, amplitude at +28)
                const float rot0 = g1(e.rot, 0, t) * rk, rot1 = rr ? g1(e.rot, 1, t) * rk - rot0 : 0.0f;
                const float rotIn[2] = {rot0, rot1};
                float tot[2];
                for (int c = 0; c < 2; c++) {
                    tot[c] = i == 0 ? 0.0f : prevTot[c] + (prevRot[c] + rotIn[c]) * 0.5f;
                    prevRot[c] = rotIn[c];
                    prevTot[c] = tot[c];
                }
                const float sz0 = g1(e.size[0], 0, t) * 0.5f, sz0a = rs0 ? g1(e.size[0], 1, t) * 0.5f - sz0 : 0.0f;
                float sz1 = sz0, sz1a = sz0a;
                if (nonUniform) {
                    sz1 = g1(e.size[1], 0, t) * 0.5f;
                    sz1a = rs1 ? g1(e.size[1], 1, t) * 0.5f - sz1 : 0.0f;
                }
                float sc = g1(e.scale, 0, t), sca = rsc ? g1(e.scale, 1, t) - sc : 0.0f;
                if (isLight && sc == 0 && !graphNonZero(e.scale)) sc = 1.5f;  // stock lights (no scale graph)
                if (isLight) sz1 = sz1a = 0;
                const float base[5] = {rot0, tot[0], sz0, sz1, sc}, amp[5] = {rot1, tot[1], sz0a, sz1a, sca};
                std::memcpy(s + 4, base, 20);
                std::memcpy(s + 28, amp, 20);
            }
        }
        samplesBytes += o.velSamples.size() + o.visSamples.size();
        // collision bounds, effect references, emission, sort order
        float cb[6] = {e.num("collOffset", 0), e.num("collOffset", 1), e.num("collOffset", 2), 0, 0, 0};
        const float cr = e.num("collRadius");
        cb[3] = cb[4] = cb[5] = cr;
        std::memcpy(d + 208, cb, 24);
        auto strKey = [&](const char* k) {
            auto it = e.keys.find(k);
            return it == e.keys.end() || it->second.empty() ? std::string() : it->second[0];
        };
        o.onImpact = strKey("fxOnImpact");
        o.onDeath = strKey("fxOnDeath");
        o.emitted = strKey("emission");
        for (const std::string* r : {&o.onImpact, &o.onDeath, &o.emitted})
            if (!r->empty()) out.dependencies.push_back(*r);
        putRange(d + 256, e.num("emitDist", 0), e.num("emitDist", 1));
        putRange(d + 264, e.num("emitDistVariance", 0), e.num("emitDistVariance", 1));
        d[280] = static_cast<uint8_t>(std::clamp(static_cast<int>(e.num("sortOrder", 0, 5)), 0, 255));
        const float lf = e.num("lightingFrac");
        d[281] = static_cast<uint8_t>(std::clamp(static_cast<int>(lf * 255.0f), 0, 255));
        if (lf > 0) needsLighting = true;
        d[282] = e.flags.count("useItemClip") || e.editorFlags.count("useItemClip") ? 1 : 0;
        // trail (type 3): FxTrailDef + vertices (x y texCoord -> pos, normal, texCoord) + indices
        if (type == 3) {
            const size_t nv = e.trailVerts.size() / 3;
            if (nv < 2 || e.trailInds.size() < 2) {
                warnings.push_back(name + ": trail '" + e.name + "' has no usable trailDef; left out");
                continue;
            }
            o.trail.assign(0x30, 0);
            const float splitDist = e.num("trailSplitDist"), repeatDist = e.num("trailRepeatDist");
            putI(o.trail.data() + 0, static_cast<int>(e.num("trailScrollTime")));
            putI(o.trail.data() + 4, static_cast<int>(repeatDist));
            putF(o.trail.data() + 8, splitDist > 0 ? 1.0f / splitDist : 0.0f);
            const float splitTime = e.num("trailRepeatTime");
            putF(o.trail.data() + 16, splitTime > 0 ? 1.0f / splitTime : 0.0f);
            putI(o.trail.data() + 20, static_cast<int>(nv));
            putI(o.trail.data() + 32, static_cast<int>(e.trailInds.size()));
            o.trailVerts.assign(20 * nv, 0);
            for (size_t v = 0; v < nv; v++) {
                const float x = e.trailVerts[3 * v], y = e.trailVerts[3 * v + 1], tc = e.trailVerts[3 * v + 2];
                const float l = std::sqrt(x * x + y * y);
                const float f[5] = {x, y, l > 1e-6f ? x / l : 0.0f, l > 1e-6f ? y / l : 1.0f, tc};
                std::memcpy(o.trailVerts.data() + 20 * v, f, 20);
            }
            for (int ix : e.trailInds) o.trailInds.push_back(static_cast<uint16_t>(std::clamp(ix, 0, static_cast<int>(nv) - 1)));
            visualBytes += 0x30 + o.trailVerts.size() + 2 * o.trailInds.size();
        }
        if (e.editorFlags.count("looping")) keptLooping++;
        out.elems.push_back(std::move(o));
    }
    out.countLooping = keptLooping;
    out.countOneShot = static_cast<int32_t>(out.elems.size()) - keptLooping;
    if (out.elems.empty()) {
        error = name + ": no elements this compiler can build";
        return false;
    }
    out.flags = needsLighting ? 1 : 0;
    out.msecLoopingLife = infiniteLoop ? 0x7FFFFFFF : loopLife;  // stock: INT_MAX for effects that loop forever
    out.totalSize = static_cast<int32_t>(40 + 288 * out.elems.size() + name.size() + 1 + samplesBytes + visualBytes);
    return true;
}

}  // namespace efx
