#include "fxpreview.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fxpreview {
namespace {
constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr size_t kMaxPerEmitter = 3000;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool isSprite(const std::string& vt) {
    return vt == "billboardSprite" || vt == "orientedSprite" || vt == "tail" || vt == "trail" || vt == "cloud" || vt == "rotatedSprite" ||
           vt == "line";
}
float g1(const efx::Graph& g, int curve, float t) { return g.present ? g.curve[curve].eval(t)[0] * g.scale : 0.0f; }
// Curve 0, blended towards curve 1 by r when the element randomises this graph.
float value(const efx::Graph& g, bool rand, float r, float t) {
    const float a = g1(g, 0, t);
    return rand ? a + (g1(g, 1, t) - a) * r : a;
}
}  // namespace

void Preview::load(const std::string& zoneSourceDir, MaterialFn materials, std::vector<std::string>& warnings) {
    namespace fs = std::filesystem;
    effects_.clear();
    emitters_.clear();
    missing_.clear();
    loaded_ = true;
    const fs::path dir = fs::path(zoneSourceDir) / "fx";
    std::error_code ec;
    if (zoneSourceDir.empty() || !fs::is_directory(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".efx") continue;
        const std::string name = lower(fs::relative(it->path(), dir, ec).replace_extension().generic_string());
        if (name.rfind("materials/", 0) == 0) continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto src = std::make_unique<efx::Effect>();
        std::string err;
        if (!efx::parse(text, *src, err)) {
            warnings.push_back("fx preview: " + name + ": " + err);
            continue;
        }
        Effect fx;
        for (const efx::Element& e : src->elems) {
            if (e.editorFlags.count("playOnTouch") || e.editorFlags.count("playOnDeath") || e.editorFlags.count("playOnRun")) continue;
            if (!isSprite(e.visualType)) continue;
            Elem el;
            el.src = &e;
            for (const std::string& m : e.visuals) {
                MaterialInfo mi = materials ? materials(m) : MaterialInfo{};
                if (!mi.found) continue;
                el.materials.push_back(m);
                el.matInfo.push_back(mi);
            }
            if (el.materials.empty()) continue;
            el.looping = e.editorFlags.count("looping") != 0;
            el.interval = std::max(1, static_cast<int>(e.num("spawnLooping", 0, 200)));
            el.loopCount = static_cast<int>(e.num("spawnLooping", 1, 0));
            el.oneShotBase = static_cast<int>(e.num("spawnOneShot", 0, 1));
            el.oneShotRand = static_cast<int>(e.num("spawnOneShot", 1, 0));
            el.delayBase = e.num("spawnDelayMsec", 0), el.delayRand = e.num("spawnDelayMsec", 1);
            el.lifeBase = e.num("lifeSpanMsec", 0, 1000), el.lifeRand = e.num("lifeSpanMsec", 1);
            const char* org[3] = {"spawnOrgX", "spawnOrgY", "spawnOrgZ"};
            for (int k = 0; k < 3; k++) el.org[k][0] = e.num(org[k], 0), el.org[k][1] = e.num(org[k], 1);
            el.offRadius[0] = e.num("spawnOffsetRadius", 0), el.offRadius[1] = e.num("spawnOffsetRadius", 1);
            el.offHeight[0] = e.num("spawnOffsetHeight", 0), el.offHeight[1] = e.num("spawnOffsetHeight", 1);
            el.offsetShape = e.flags.count("spawnOffsetSphere") ? 1 : e.flags.count("spawnOffsetCylinder") ? 2 : 0;
            el.initRot[0] = e.num("initialRot", 0) * kDegToRad, el.initRot[1] = e.num("initialRot", 1) * kDegToRad;
            el.gravity[0] = e.num("gravity", 0) * 8.0f, el.gravity[1] = e.num("gravity", 1) * 8.0f;  // % of 800 u/s^2
            el.randVel0 = e.editorFlags.count("useRandVel0") != 0;
            el.randVel1 = e.editorFlags.count("useRandVel1") != 0;
            el.randSize0 = e.editorFlags.count("useRandSize0") != 0;
            el.randColor = e.editorFlags.count("useRandColor") != 0;
            el.randAlpha = e.editorFlags.count("useRandAlpha") != 0;
            el.randRot = e.editorFlags.count("useRandRotDelta") != 0;
            el.modColorByAlpha = e.editorFlags.count("modColorByAlpha") != 0;
            if (auto it2 = e.keys.find("atlasBehavior"); it2 != e.keys.end())
                for (const std::string& b : it2->second) {
                    if (b == "startRandom") el.atlasBehavior |= 1;
                    if (b == "startIndexed") el.atlasBehavior |= 2;
                    if (b == "playOverLife") el.atlasBehavior |= 4;
                }
            el.atlasIndex = static_cast<int>(e.num("atlasIndex"));
            el.atlasRange = static_cast<int>(e.num("atlasIndexRange"));
            el.atlasFps = static_cast<int>(e.num("atlasFps"));
            const MaterialInfo& m0 = el.matInfo[0];
            el.atlasEntries = e.num("atlasEntryCount") > 0 ? static_cast<int>(e.num("atlasEntryCount")) : m0.rows * m0.cols;
            el.atlasEntries = std::clamp(el.atlasEntries, 1, std::max(1, m0.rows * m0.cols));
            fx.hasLooping |= el.looping;
            fx.elems.push_back(std::move(el));
        }
        fx.src = std::move(src);
        effects_[name] = std::move(fx);
    }
}

void Preview::sync(const Document& doc) {
    std::vector<Emitter> next;
    missing_.clear();
    // triggers aimed at fx groups (the compiler's rules: trigger_radius only; others never fire)
    std::set<std::string> groupNames, targeted;
    for (const Entity& e : doc.ents)
        if (e.classname() == "fx_origin")
            if (const std::string* t = e.find("targetname"); t && !t->empty()) groupNames.insert(*t);
    std::vector<Trigger> trig;
    for (const Entity& e : doc.ents) {
        const std::string* t = e.find("target");
        if (!t || !groupNames.count(*t) || e.classname() == "fx_origin") continue;
        targeted.insert(*t);
        const std::string cls = e.classname();
        const bool brushTrigger = (cls.rfind("trigger_", 0) == 0 || cls == "info_volume") && !e.brushes.empty();
        if (cls != "trigger_radius" && !brushTrigger) continue;
        Trigger tr;
        e.origin(tr.origin);
        if (brushTrigger)
            for (const Brush& b : e.brushes) {
                std::vector<std::pair<Vec3, double>> planes;
                for (const Face& f : b.faces) planes.push_back({f.normal, f.dist});
                tr.brushes.push_back(std::move(planes));
            }
        if (const std::string* r = e.find("radius")) tr.radius = std::atof(r->c_str());
        if (const std::string* h = e.find("height")) tr.height = std::atof(h->c_str());
        tr.group = *t;
        const std::string* sn = e.find("script_noteworthy");
        tr.stop = sn && *sn == "stop";
        for (const Trigger& o : triggers_)  // keep "already inside" across edits
            if (o.group == tr.group && o.stop == tr.stop && length(o.origin - tr.origin) < 1e-3) tr.inside = o.inside;
        trig.push_back(tr);
    }
    triggers_ = std::move(trig);
    for (size_t i = 1; i < doc.ents.size(); i++) {
        const Entity& e = doc.ents[i];
        if (e.classname() != "fx_origin") continue;
        const std::string* fxName = e.find("fx");
        if (!fxName || fxName->empty()) continue;
        const std::string name = lower(*fxName);
        if (!effects_.count(name)) {
            missing_.insert(name);
            continue;
        }
        const std::string* tn = e.find("targetname");
        const std::string group = tn ? *tn : "";
        const std::string* so = e.find("start_off");
        const bool waitStart = !group.empty() && (targeted.count(group) || (so && std::atoi(so->c_str()) != 0));
        std::string key = name + (waitStart ? "|wait" : "");
        for (const char* k : {"origin", "angles", "angle", "delay", "targetname", "duration", "once"})
            if (const std::string* v = e.find(k)) key += std::string("|") + k + "=" + *v;
        auto old = std::find_if(emitters_.begin(), emitters_.end(), [&](const Emitter& m) { return m.key == key; });
        if (old != emitters_.end()) {
            next.push_back(std::move(*old));
            emitters_.erase(old);
            continue;
        }
        Emitter em;
        em.key = key;
        em.effect = name;
        e.origin(em.origin);
        double pitch = 270, yaw = 0, roll = 0;  // as the compiler: default pointing up
        if (const std::string* a = e.find("angles")) std::sscanf(a->c_str(), "%lf %lf %lf", &pitch, &yaw, &roll);
        else if (const std::string* a1 = e.find("angle")) pitch = 0, yaw = std::atof(a1->c_str());
        const double d = 3.14159265358979 / 180.0, sp = std::sin(pitch * d), cp = std::cos(pitch * d), sy = std::sin(yaw * d),
                     cy = std::cos(yaw * d), sr = std::sin(roll * d), cr = std::cos(roll * d);
        em.axis[0] = {cp * cy, cp * sy, -sp};
        em.axis[2] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
        em.axis[1] = cross(em.axis[2], em.axis[0]);  // left
        const Effect& fx = effects_[name];
        em.group = group;
        em.waitStart = waitStart;
        if (const std::string* du = e.find("duration")) em.duration = static_cast<float>(std::atof(du->c_str()));
        if (const std::string* on = e.find("once")) em.once = std::atoi(on->c_str()) != 0;
        em.dynamic = !group.empty() || em.duration > 0;
        // one-shot effects repeat, as the generated script does (a dynamic one: once per start)
        em.delay = fx.hasLooping || em.dynamic ? 0.0f : 2.0f;
        if (const std::string* dl = e.find("delay")) em.delay = static_cast<float>(std::atof(dl->c_str()));
        em.running = !em.waitStart;
        em.states.resize(fx.elems.size());
        next.push_back(std::move(em));
    }
    emitters_ = std::move(next);
}

void Preview::start(Emitter& em, const Effect& fx, bool loopingToo) {
    std::uniform_real_distribution<float> u(0, 1);
    for (size_t i = 0; i < fx.elems.size(); i++) {
        const Elem& el = fx.elems[i];
        ElemState& st = em.states[i];
        if (el.looping && !loopingToo) continue;
        st.nextSpawn = em.time + (el.delayBase + u(rng_) * el.delayRand) / 1000.0f;
        st.spawned = el.looping ? 0 : -(el.oneShotBase + static_cast<int>(u(rng_) * (el.oneShotRand + 1)));  // < 0: pending one-shots
    }
}

void Preview::spawn(Emitter& em, const Effect& fx, int elemIndex) {
    if (em.particles.size() >= kMaxPerEmitter) return;
    const Elem& el = fx.elems[static_cast<size_t>(elemIndex)];
    std::uniform_real_distribution<float> u(0, 1);
    Particle p;
    p.elem = elemIndex;
    p.life = std::max(0.01f, (el.lifeBase + u(rng_) * el.lifeRand) / 1000.0f);
    double o[3];
    for (int k = 0; k < 3; k++) o[k] = el.org[k][0] + u(rng_) * el.org[k][1];
    if (el.offsetShape == 1) {  // sphere
        const double z = u(rng_) * 2 - 1, a = u(rng_) * 6.2831853, s = std::sqrt(1 - z * z);
        const double r = el.offRadius[0] + u(rng_) * el.offRadius[1];
        o[0] += s * std::cos(a) * r, o[1] += s * std::sin(a) * r, o[2] += z * r;
    } else if (el.offsetShape == 2) {  // cylinder around the forward axis
        const double a = u(rng_) * 6.2831853, r = el.offRadius[0] + u(rng_) * el.offRadius[1];
        o[1] += std::cos(a) * r, o[2] += std::sin(a) * r;
        o[0] += el.offHeight[0] + u(rng_) * el.offHeight[1];
    }
    p.pos = em.origin + em.axis[0] * o[0] + em.axis[1] * o[1] + em.axis[2] * o[2];
    p.gravity = el.gravity[0] + u(rng_) * el.gravity[1];
    p.rot = el.initRot[0] + u(rng_) * el.initRot[1];
    for (float& r : p.r) r = u(rng_);
    p.material = static_cast<int>(u(rng_) * el.materials.size()) % static_cast<int>(el.materials.size());
    if (el.atlasBehavior & 1) p.frame = static_cast<int>(u(rng_) * el.atlasEntries);
    else p.frame = el.atlasIndex + (el.atlasRange > 0 ? static_cast<int>(u(rng_) * el.atlasRange) : 0);
    em.particles.push_back(p);
}

void Preview::step(Emitter& em, float dt) {
    auto fxIt = effects_.find(em.effect);
    if (fxIt == effects_.end()) return;
    const Effect& fx = fxIt->second;
    if (!em.started) {
        em.started = true;
        if (em.dynamic) {
            if (em.running) run(em);
        } else {
            start(em, fx, true);
            em.nextReplay = em.delay;
        }
    }
    em.time += dt;
    if (em.running && em.dynamic && em.duration > 0 && em.time >= em.stopAt) em.running = false;
    // replay the one-shot elements (a dynamic looping effect is spawnfx'd: no replays)
    if (em.running && em.delay > 0 && !(em.dynamic && fx.hasLooping) && em.time >= em.nextReplay) {
        start(em, fx, false);
        em.nextReplay += em.delay;
    }
    for (size_t i = 0; i < fx.elems.size(); i++) {
        const Elem& el = fx.elems[i];
        ElemState& st = em.states[i];
        if (el.looping) {
            if (!em.running) continue;
            for (int guard = 0; guard < 200 && em.time >= st.nextSpawn && (el.loopCount <= 0 || st.spawned < el.loopCount); guard++) {
                spawn(em, fx, static_cast<int>(i));
                st.spawned++;
                st.nextSpawn += el.interval / 1000.0f;
            }
        } else if (st.spawned < 0 && em.time >= st.nextSpawn) {
            for (int n = -st.spawned; n > 0; n--) spawn(em, fx, static_cast<int>(i));
            st.spawned = 0;
        }
    }
    for (Particle& p : em.particles) {
        p.age += dt;
        const Elem& el = fx.elems[static_cast<size_t>(p.elem)];
        const efx::Element& e = *el.src;
        const float t = std::min(1.0f, p.age / p.life);
        Vec3 v;
        for (int k = 0; k < 3; k++) {
            v += em.axis[k] * value(e.vel[0][k], el.randVel0, p.r[0], t);  // effect axes
            Vec3 w;
            w[k] = value(e.vel[1][k], el.randVel1, p.r[1], t);  // world axes
            v += w;
        }
        p.gravVel -= p.gravity * dt;
        v.z += p.gravVel;
        p.pos += v * dt;
        p.rot += value(e.rot, el.randRot, p.r[5], t) * kDegToRad * dt;
    }
    em.particles.erase(std::remove_if(em.particles.begin(), em.particles.end(), [](const Particle& p) { return p.age >= p.life; }),
                       em.particles.end());
}

void Preview::run(Emitter& em) {
    auto it = effects_.find(em.effect);
    if (it == effects_.end() || (em.once && em.used)) return;
    const Effect& fx = it->second;
    em.started = em.used = em.running = true;
    start(em, fx, true);
    em.nextReplay = em.time + em.delay;
    em.stopAt = em.time + em.duration;
    if (!fx.hasLooping && em.delay <= 0) em.running = false;  // a single playfx: back to waiting at once
}

void Preview::startGroup(const std::string& name) {
    for (Emitter& em : emitters_)
        if (em.dynamic && em.group == name && !(em.running && em.started)) run(em);
}

void Preview::stopGroup(const std::string& name) {
    for (Emitter& em : emitters_)
        if (em.dynamic && em.group == name) em.running = false;
}

void Preview::setViewer(const Vec3& v) {
    for (Trigger& tr : triggers_) {
        const double dx = v.x - tr.origin.x, dy = v.y - tr.origin.y;
        bool inside = false;
        if (tr.brushes.empty()) {
            inside = dx * dx + dy * dy <= tr.radius * tr.radius && v.z >= tr.origin.z && v.z <= tr.origin.z + tr.height;
        } else {
            for (const auto& planes : tr.brushes) {
                bool in = !planes.empty();
                for (const auto& [n, d] : planes) in &= dot(n, v) <= d;
                inside |= in;
            }
        }
        if (inside && !tr.inside) {
            if (tr.stop) stopGroup(tr.group);
            else startGroup(tr.group);
        }
        tr.inside = inside;
    }
}

std::set<std::string> Preview::groups() const {
    std::set<std::string> out;
    for (const Emitter& em : emitters_)
        if (!em.group.empty()) out.insert(em.group);
    return out;
}

size_t Preview::runningCount() const {
    size_t n = 0;
    for (const Emitter& em : emitters_) n += em.running;
    return n;
}

void Preview::update(double dt) {
    const float d = static_cast<float>(std::clamp(dt, 0.0, 0.1));
    for (Emitter& em : emitters_) step(em, d);
}

void Preview::warmup(double seconds) {
    for (double t = 0; t < seconds; t += 1.0 / 30) update(1.0 / 30);
}

void Preview::sprites(const Vec3& eye, const Vec3& forward, const Vec3& right, const Vec3& up, std::vector<Sprite>& out) const {
    for (const Emitter& em : emitters_) {
        auto fxIt = effects_.find(em.effect);
        if (fxIt == effects_.end()) continue;
        for (const Particle& p : em.particles) {
            const Elem& el = fxIt->second.elems[static_cast<size_t>(p.elem)];
            const efx::Element& e = *el.src;
            const float t = std::min(1.0f, p.age / p.life);
            const float depth = static_cast<float>(dot(p.pos - eye, forward));
            if (depth < 2) continue;
            const float size = value(e.size[0], el.randSize0, p.r[2], t);
            if (size <= 0.01f) continue;
            float rgb[3] = {1, 1, 1}, a = 1;
            if (e.color.present) {
                const std::vector<float> c0 = e.color.curve[0].eval(t), c1 = e.color.curve[1].eval(t);
                for (int k = 0; k < 3 && k < static_cast<int>(c0.size()); k++)
                    rgb[k] = (el.randColor && k < static_cast<int>(c1.size()) ? c0[k] + (c1[k] - c0[k]) * p.r[3] : c0[k]) * e.color.scale;
            }
            if (e.alpha.present) a = value(e.alpha, el.randAlpha, p.r[4], t);
            if (el.modColorByAlpha) {
                for (float& c : rgb) c *= a;
                a = 1;
            }
            if (a <= 0.003f) continue;
            Sprite s;
            s.material = el.materials[static_cast<size_t>(p.material)];
            const MaterialInfo& mi = el.matInfo[static_cast<size_t>(p.material)];
            s.additive = mi.additive;
            s.pos = p.pos;
            s.depth = depth;
            const double half = size * 0.5, c = std::cos(p.rot), sn = std::sin(p.rot);
            s.right = (right * c + up * sn) * half;
            s.up = (up * c - right * sn) * half;
            int frame = p.frame;
            if (el.atlasBehavior & 4) frame += static_cast<int>(t * el.atlasEntries);
            else if (el.atlasFps > 0) frame += static_cast<int>(p.age * el.atlasFps);
            frame = ((frame % el.atlasEntries) + el.atlasEntries) % el.atlasEntries;
            const int col = frame % mi.cols, row = (frame / mi.cols) % mi.rows;
            s.uv[0] = static_cast<float>(col) / mi.cols;
            s.uv[1] = static_cast<float>(row) / mi.rows;
            s.uv[2] = static_cast<float>(col + 1) / mi.cols;
            s.uv[3] = static_cast<float>(row + 1) / mi.rows;
            for (int k = 0; k < 3; k++) s.color[k] = std::clamp(rgb[k], 0.0f, 4.0f);
            s.color[3] = std::clamp(a, 0.0f, 1.0f);
            out.push_back(std::move(s));
        }
    }
}

size_t Preview::particleCount() const {
    size_t n = 0;
    for (const Emitter& em : emitters_) n += em.particles.size();
    return n;
}

std::set<std::string> Preview::materials() const {
    std::set<std::string> out;
    for (const auto& [n, fx] : effects_)
        for (const Elem& el : fx.elems) out.insert(el.materials.begin(), el.materials.end());
    return out;
}

}  // namespace fxpreview
