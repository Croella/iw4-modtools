#include "assetmanager.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include "model/imageio.h"
#include "model/xanimexport.h"
#include "model/xmodelexport.h"
#include "zone/zonebuild.h"

namespace fs = std::filesystem;

namespace assetman {
namespace {

std::string lower(std::string s) { return zsrc::lower(std::move(s)); }

bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& p, const std::string& text, std::string& error) {
    std::ofstream out(p, std::ios::binary);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        error = "couldn't write " + p.string();
        return false;
    }
    return true;
}

uintmax_t fileBytes(const fs::path& p) {
    std::error_code ec;
    uintmax_t n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

// Path of f under dir with '/' separators, without the extension when dropExt.
std::string relName(const fs::path& dir, const fs::path& f, bool dropExt) {
    fs::path r = fs::relative(f, dir);
    if (dropExt) r.replace_extension();
    std::string s = r.generic_string();
    return lower(s);
}

std::vector<fs::path> listRecursive(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (ext && lower(it->path().extension().string()) != ext) continue;
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool isImageExt(const fs::path& p) {
    const std::string e = lower(p.extension().string());
    return e == ".png" || e == ".tga";
}

// Keys the compiler needs for a working automatic rifle (from the AK-74u weapon file: every
// non-zero value except its models, anims and notetrack maps). newWeapon fills the rest.
const std::pair<const char*, const char*> kRifleTemplate[] = {
    {"playerAnimType", "autorifle"}, {"weaponType", "bullet"}, {"weaponClass", "rifle"}, {"penetrateType", "medium"},
    {"penetrateMultiplier", "1"}, {"impactType", "bullet_large"}, {"inventoryType", "primary"},
    {"fireType", "Full Auto"}, {"offhandClass", "None"}, {"viewFlashEffect", "muzzleflashes/ak47_flash_view"},
    {"worldFlashEffect", "muzzleflashes/ak47_flash_wv"}, {"pickupSound", "weap_pickup"},
    {"pickupSoundPlayer", "weap_pickup"}, {"ammoPickupSound", "weap_ammo_pickup"},
    {"fireSound", "weap_ak47_fire_npc"}, {"fireSoundPlayer", "weap_ak47_fire_plr"},
    {"fireSoundPlayerAkimbo", "weap_ak47_fire_aki"}, {"emptyFireSound", "weap_dryfire_smg_npc"},
    {"emptyFireSoundPlayer", "weap_dryfire_smg_plr"}, {"meleeSwipeSound", "melee_swing_small"},
    {"meleeSwipeSoundPlayer", "melee_swing_large"}, {"reloadSound", "weap_ak47_reload_npc"},
    {"reloadEmptySound", "weap_ak47_reload_npc"}, {"raiseSound", "weap_raise"},
    {"raiseSoundPlayer", "weap_raise_plr"}, {"putawaySound", "weap_putaway"},
    {"viewShellEjectEffect", "shellejects/rifle_view"}, {"worldShellEjectEffect", "shellejects/rifle"},
    {"viewLastShotEjectEffect", "shellejects/rifle_view"}, {"worldLastShotEjectEffect", "shellejects/rifle"},
    {"reticleSide", "reticle_side_small"}, {"reticleCenterSize", "4"}, {"reticleSideSize", "8"},
    {"activeReticleType", "None"}, {"standMoveU", "-0.5"}, {"strafeMoveR", "0.5"}, {"strafeRotR", "5"},
    {"duckedOfsR", "0.8"}, {"duckedMoveF", "-0.5"}, {"proneOfsF", "-2"}, {"proneOfsR", "1.5"},
    {"proneMoveF", "-160"}, {"proneMoveR", "3"}, {"proneMoveU", "-120"}, {"proneRotY", "300"}, {"proneRotR", "-300"},
    {"posMoveRate", "6"}, {"posProneMoveRate", "25"}, {"posRotRate", "6"}, {"posProneRotRate", "30"},
    {"hudIcon", "hud_icon_ak47"}, {"hudIconRatio", "4:1"}, {"pickupIconRatio", "4:1"},
    {"ammoCounterIcon", "hud_icon_ak47"}, {"ammoCounterIconRatio", "4:1"}, {"ammoCounterClip", "Magazine"},
    {"startAmmo", "90"}, {"maxAmmo", "180"}, {"clipSize", "30"}, {"shotCount", "1"}, {"damage", "40"},
    {"playerDamage", "100"}, {"meleeDamage", "135"}, {"minDamage", "30"}, {"minPlayerDamage", "30"},
    {"maxDamageRange", "1500"}, {"minDamageRange", "2000"}, {"meleeDelay", "0.079"}, {"meleeChargeDelay", "0.1"},
    {"fireTime", "0.085"}, {"rechamberTime", "0.1"}, {"rechamberTimeOneHanded", "0.1"}, {"meleeTime", "0.8"},
    {"meleeChargeTime", "1.159"}, {"reloadTime", "2.5"}, {"reloadEmptyTime", "3.25"}, {"reloadAddTime", "1.75"},
    {"dropTime", "0.6"}, {"raiseTime", "0.949"}, {"quickDropTime", "0.25"}, {"quickRaiseTime", "0.75"},
    {"firstRaiseTime", "1.399"}, {"breachRaiseTime", "0.5"}, {"emptyRaiseTime", "0.949"}, {"emptyDropTime", "0.6"},
    {"sprintInTime", "0.3"}, {"sprintLoopTime", "0.699"}, {"sprintOutTime", "0.3"}, {"stunnedTimeBegin", "0.5"},
    {"stunnedTimeLoop", "0.5"}, {"stunnedTimeEnd", "0.5"}, {"inheritsPerks", "1"}, {"autoAimRange", "1000"},
    {"aimAssistRange", "1000"}, {"aimAssistRangeAds", "1500"}, {"enemyCrosshairRange", "1000"},
    {"crosshairColorChange", "1"}, {"moveSpeedScale", "1"}, {"adsMoveSpeedScale", "1"}, {"sprintDurationScale", "1"},
    {"idleCrouchFactor", "1"}, {"idleProneFactor", "0.4"}, {"gunMaxPitch", "8"}, {"gunMaxYaw", "25"},
    {"swayMaxAngle", "20"}, {"swayLerpSpeed", "4"}, {"swayPitchScale", "-1"}, {"swayYawScale", "-1"},
    {"swayHorizScale", "0.3"}, {"swayVertScale", "0.1"}, {"swayShellShockScale", "5"}, {"adsSwayMaxAngle", "2"},
    {"adsSwayLerpSpeed", "6"}, {"adsSwayPitchScale", "0.1"}, {"adsSwayYawScale", "0.1"},
    {"adsSwayHorizScale", "0.14"}, {"adsSwayVertScale", "0.25"}, {"rifleBullet", "1"}, {"aimDownSight", "1"},
    {"rechamberWhileAds", "1"}, {"noDualWield", "1"}, {"dualWieldViewModelOffset", "7"},
    {"killIcon", "hud_icon_ak47"}, {"killIconRatio", "4:1"}, {"flipKillIcon", "1"}, {"dpadIconRatio", "4:1"},
    {"dpadIconShowsAmmo", "1"}, {"dropAmmoMin", "1"}, {"dropAmmoMax", "30"}, {"ammoDropClipPercentMin", "30"},
    {"ammoDropClipPercentMax", "100"}, {"bulletExplDmgMult", "1"}, {"bulletExplRadiusMult", "1"},
    {"projExplosionType", "grenade"}, {"stickiness", "Don't stick"}, {"lowAmmoWarningThreshold", "0.33"},
    {"ricochetChance", "1"}, {"guidedMissileType", "None"}, {"adsTransInTime", "0.25"}, {"adsTransOutTime", "0.25"},
    {"adsIdleAmount", "2"}, {"adsIdleSpeed", "0.8"}, {"adsZoomFov", "50"}, {"adsZoomInFrac", "0.5"},
    {"adsZoomOutFrac", "0.4"}, {"adsOverlayReticle", "none"}, {"adsOverlayInterface", "None"},
    {"adsOverlayWidth", "150"}, {"adsOverlayHeight", "150"}, {"adsOverlayWidthSplitscreen", "480"},
    {"adsOverlayHeightSplitscreen", "480"}, {"adsBobFactor", "1"}, {"adsViewBobMult", "0.2"},
    {"adsCrosshairInFrac", "1"}, {"adsCrosshairOutFrac", "0.2"}, {"adsReloadTransTime", "0.449"},
    {"adsGunKickReducedKickPercent", "75"}, {"adsGunKickPitchMin", "5"}, {"adsGunKickPitchMax", "15"},
    {"adsGunKickYawMin", "-5"}, {"adsGunKickYawMax", "10"}, {"adsGunKickAccel", "800"},
    {"adsGunKickSpeedMax", "2000"}, {"adsGunKickSpeedDecay", "32"}, {"adsGunKickStaticDecay", "40"},
    {"adsViewKickPitchMin", "-30"}, {"adsViewKickPitchMax", "60"}, {"adsViewKickYawMin", "60"},
    {"adsViewKickYawMax", "-60"}, {"adsViewKickCenterSpeed", "1500"}, {"hipSpreadStandMin", "3"},
    {"hipSpreadDuckedMin", "2.5"}, {"hipSpreadProneMin", "2"}, {"hipSpreadMax", "7"}, {"hipSpreadDuckedMax", "6"},
    {"hipSpreadProneMax", "5"}, {"hipSpreadDecayRate", "4"}, {"hipSpreadFireAdd", "0.6"}, {"hipSpreadMoveAdd", "5"},
    {"hipSpreadDuckedDecay", "1.05"}, {"hipSpreadProneDecay", "1.1"}, {"hipIdleAmount", "15"}, {"hipIdleSpeed", "5"},
    {"hipGunKickPitchMin", "5"}, {"hipGunKickPitchMax", "-15"}, {"hipGunKickYawMin", "5"},
    {"hipGunKickYawMax", "-5"}, {"hipGunKickAccel", "800"}, {"hipGunKickSpeedMax", "2000"},
    {"hipGunKickSpeedDecay", "16"}, {"hipGunKickStaticDecay", "20"}, {"hipViewKickPitchMin", "-30"},
    {"hipViewKickPitchMax", "60"}, {"hipViewKickYawMin", "60"}, {"hipViewKickYawMax", "-60"},
    {"hipViewKickCenterSpeed", "1500"}, {"stance", "stand"}, {"fightDist", "720"}, {"maxDist", "590"},
    {"locNone", "1"}, {"locHelmet", "1.4"}, {"locHead", "1.4"}, {"locNeck", "1"}, {"locTorsoUpper", "1"},
    {"locTorsoLower", "1"}, {"locRightArmUpper", "1"}, {"locRightArmLower", "1"}, {"locRightHand", "1"},
    {"locLeftArmUpper", "1"}, {"locLeftArmLower", "1"}, {"locLeftHand", "1"}, {"locRightLegUpper", "1"},
    {"locRightLegLower", "1"}, {"locRightFoot", "1"}, {"locLeftLegUpper", "1"}, {"locLeftLegLower", "1"},
    {"locLeftFoot", "1"}, {"fireRumble", "assault_fire"}, {"meleeImpactRumble", "defaultweapon_melee"},
    {"tracerType", "assaultrifle"}, {"adsDofEnd", "15"},
};

}  // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Weapon: return "Weapons";
        case Kind::XModel: return "XModels";
        case Kind::XAnim: return "XAnims";
        case Kind::Image: return "Images";
        case Kind::Effect: return "Effects";
        case Kind::Sky: return "Skies";
        case Kind::Script: return "Scripts";
        default: return "Other";
    }
}

const char* kindSubdir(Kind k) {
    switch (k) {
        case Kind::Weapon: return "weapons";
        case Kind::XModel: return "xmodels";
        case Kind::XAnim: return "xanims";
        case Kind::Image: return "images";
        case Kind::Effect: return "fx";
        case Kind::Sky: return "skies";
        case Kind::Script: return "scripts";
        default: return "";
    }
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 60) return false;
    for (char c : name)
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// ---- weapon files ----

bool WeaponFile::parse(const std::string& text, std::string& error) {
    kv.clear();
    std::vector<std::string> parts;
    std::string cur;
    for (char c : text) {
        if (c == '\\') parts.push_back(cur), cur.clear();
        else cur += c;
    }
    parts.push_back(cur);
    if (parts.empty() || parts[0] != "WEAPONFILE") {
        error = "not a weapon file (expected WEAPONFILE\\key\\value...)";
        return false;
    }
    for (size_t i = 1; i + 1 < parts.size(); i += 2) {
        std::string v = parts[i + 1];
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        kv.push_back({parts[i], v});
    }
    return true;
}

std::string WeaponFile::format() const {
    std::string s = "WEAPONFILE";
    for (const auto& [k, v] : kv) s += "\\" + k + "\\" + v;
    return s;
}

const std::string* WeaponFile::get(const std::string& key) const {
    for (const auto& [k, v] : kv)
        if (iequals(k, key)) return &v;
    return nullptr;
}

void WeaponFile::set(const std::string& key, const std::string& value) {
    for (auto& [k, v] : kv)
        if (iequals(k, key)) {
            v = value;
            return;
        }
    kv.push_back({key, value});
}

bool WeaponFile::erase(const std::string& key) {
    auto it = std::find_if(kv.begin(), kv.end(), [&](const auto& p) { return iequals(p.first, key); });
    if (it == kv.end()) return false;
    kv.erase(it);
    return true;
}

// ---- index ----

void Index::scan(const std::string& zoneSourceDir) {
    root = zoneSourceDir;
    assets.clear();
    const fs::path r(root);
    std::error_code ec;
    if (root.empty() || !fs::is_directory(r, ec)) return;
    auto add = [&](Kind k, std::string name, const fs::path& p) {
        Asset a;
        a.kind = k;
        a.name = std::move(name);
        a.path = p;
        a.bytes = fileBytes(p);
        assets.push_back(std::move(a));
    };
    for (const fs::path& f : zsrc::list(root, "weapons", nullptr)) add(Kind::Weapon, lower(f.filename().string()), f);
    for (const fs::path& f : zsrc::list(root, "xmodels", ".xmodel_export")) {
        add(Kind::XModel, zsrc::assetName(f), f);
        Asset& a = assets.back();
        for (const auto& e : fs::directory_iterator(f.parent_path(), ec)) {
            const std::string stem = lower(e.path().stem().string());
            if (lower(e.path().extension().string()) == ".xmodel_export" && zsrc::isExtraLod(stem) &&
                stem.compare(0, stem.size() - 5, a.name) == 0 && stem.size() == a.name.size() + 5)
                a.lods.push_back(e.path());
        }
        std::sort(a.lods.begin(), a.lods.end());
    }
    for (const fs::path& f : zsrc::list(root, "xanims", ".xanim_export")) add(Kind::XAnim, zsrc::assetName(f), f);
    for (const char* sub : {"images", "xmodels"})
        for (const fs::path& f : zsrc::list(root, sub, nullptr))
            if (isImageExt(f)) add(Kind::Image, lower(f.filename().string()), f);
    for (const fs::path& f : listRecursive(r / "fx", ".efx")) add(Kind::Effect, relName(r / "fx", f, true), f);
    for (const fs::path& f : zsrc::list(root, "skies", nullptr))
        if (isImageExt(f)) add(Kind::Sky, lower(f.stem().string()), f);
    for (const fs::path& f : listRecursive(r / "scripts", nullptr)) add(Kind::Script, relName(r / "scripts", f, false), f);
    // Anything else (stray files, unknown folders) so nothing in zone_source is invisible.
    static const std::set<std::string> known = {"weapons", "xmodels", "xanims", "images", "fx", "skies", "scripts"};
    for (const auto& e : fs::directory_iterator(r, ec)) {
        if (e.is_directory(ec) && known.count(lower(e.path().filename().string()))) {
            // files in known folders that the categories above don't take
            if (lower(e.path().filename().string()) == "xmodels" || lower(e.path().filename().string()) == "xanims") {
                for (const auto& f : fs::directory_iterator(e.path(), ec)) {
                    if (!f.is_regular_file(ec)) continue;
                    const std::string ext = lower(f.path().extension().string());
                    const bool taken = ext == ".xmodel_export" || ext == ".xanim_export" || isImageExt(f.path());
                    if (!taken) add(Kind::Other, relName(r, f.path(), false), f.path());
                }
            }
            continue;
        }
        if (e.is_regular_file(ec)) add(Kind::Other, relName(r, e.path(), false), e.path());
        else if (e.is_directory(ec))
            for (const fs::path& f : listRecursive(e.path(), nullptr)) add(Kind::Other, relName(r, f, false), f);
    }
}

const Asset* Index::find(Kind k, const std::string& name) const {
    for (const Asset& a : assets)
        if (a.kind == k && a.name == name) return &a;
    return nullptr;
}

std::vector<std::string> Index::names(Kind k) const {
    std::vector<std::string> out;
    for (const Asset& a : assets)
        if (a.kind == k) out.push_back(a.name);
    return out;
}

size_t Index::count(Kind k) const {
    return static_cast<size_t>(std::count_if(assets.begin(), assets.end(), [&](const Asset& a) { return a.kind == k; }));
}

// ---- analysis ----

fs::path findTexture(const std::string& zoneSourceDir, const std::string& texture, const std::string& material) {
    std::string stem = fs::path(texture).stem().string();
    if (stem.empty()) stem = material;
    std::string bare = stem;
    for (const char* pre : {"t5_", "t6_", "t7_", "T5_", "T6_", "T7_"})
        if (bare.rfind(pre, 0) == 0) bare = bare.substr(3);
    std::vector<std::string> cands;
    if (!texture.empty()) cands.push_back(fs::path(texture).filename().string());
    for (const std::string& st : {stem, bare})
        for (const char* ext : {".png", ".tga", ".PNG", ".TGA"}) cands.push_back(st + ext);
    for (const char* sub : {"images", "xmodels"})
        for (const std::string& c : cands) {
            if (c.empty()) continue;
            const fs::path p = fs::path(zoneSourceDir) / sub / c;
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p;
        }
    return {};
}

size_t Analysis::issuesFor(Kind k, const std::string& name, Issue::Level atLeast) const {
    size_t n = 0;
    for (const Issue& i : issues)
        if (i.kind == k && i.asset == name && i.level >= atLeast) n++;
    return n;
}

Analysis analyse(const Index& index) {
    Analysis an;
    using L = Issue::Level;
    auto issue = [&](L level, Kind k, const std::string& asset, const std::string& text) {
        an.issues.push_back({level, k, asset, text});
    };
    auto use = [&](Kind k, const std::string& name, const std::string& by) {
        auto& v = an.usedBy[{k, name}];
        if (std::find(v.begin(), v.end(), by) == v.end()) v.push_back(by);
    };
    const std::vector<std::string> modelNames = index.names(Kind::XModel), animNames = index.names(Kind::XAnim),
                                   fxNames = index.names(Kind::Effect);

    // Images first (models point at them).
    for (const Asset& a : index.assets) {
        if (a.kind != Kind::Image && a.kind != Kind::Sky) continue;
        ImageInfo ii;
        imageio::Rgba img;
        if (imageio::load(a.path.string(), img, ii.error)) {
            ii.ok = true;
            ii.width = img.width;
            ii.height = img.height;
            for (size_t p = 3; p < img.pixels.size(); p += 4)
                if (img.pixels[p] != 255) {
                    ii.alpha = true;
                    break;
                }
            auto pow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
            if (!pow2(ii.width) || !pow2(ii.height))
                issue(L::Info, a.kind, a.name,
                      std::to_string(ii.width) + "x" + std::to_string(ii.height) + " isn't a power of two; resized when compiled");
        } else {
            issue(L::Error, a.kind, a.name, "can't read: " + ii.error);
        }
        if (a.kind == Kind::Image) an.images[a.name] = ii;
    }
    // Same file name in images/ and xmodels/: images/ wins.
    {
        std::map<std::string, int> seen;
        for (const Asset& a : index.assets)
            if (a.kind == Kind::Image && ++seen[a.name] == 2)
                issue(L::Warning, Kind::Image, a.name, "two files named " + a.name + " (images/ and xmodels/); images/ is used");
    }

    // Models.
    for (const Asset& a : index.assets) {
        if (a.kind != Kind::XModel) continue;
        ModelInfo mi;
        xmodelexport::Model m;
        if (!xmodelexport::load(a.path.string(), m, mi.error)) {
            issue(L::Error, Kind::XModel, a.name, "can't read: " + mi.error);
            an.models[a.name] = mi;
            continue;
        }
        mi.ok = true;
        mi.verts = m.verts.size();
        mi.tris = m.faces.size();
        for (const xmodelexport::Bone& b : m.bones) mi.bones.push_back(lower(b.name));
        for (int k = 0; k < 3; k++) mi.mins[k] = 1e30f, mi.maxs[k] = -1e30f;
        for (const xmodelexport::Vertex& v : m.verts)
            for (int k = 0; k < 3; k++) mi.mins[k] = std::min(mi.mins[k], v.pos[k]), mi.maxs[k] = std::max(mi.maxs[k], v.pos[k]);
        if (m.verts.empty())
            for (int k = 0; k < 3; k++) mi.mins[k] = mi.maxs[k] = 0;
        std::vector<bool> usedMat(m.materials.size());
        for (const xmodelexport::Face& f : m.faces)
            if (f.material >= 0 && f.material < static_cast<int>(usedMat.size())) usedMat[f.material] = true;
        for (size_t i = 0; i < m.materials.size(); i++) {
            ModelInfo::Mat mm;
            mm.name = m.materials[i].name;
            mm.texture = m.materials[i].texture;
            mm.image = findTexture(index.root, mm.texture, mm.name);
            if (!mm.image.empty()) use(Kind::Image, lower(mm.image.filename().string()), "model " + a.name + " (material " + mm.name + ")");
            else if (usedMat[i])
                mi.missingTextures.push_back(mm.name + " (" + (mm.texture.empty() ? mm.name : fs::path(mm.texture).filename().string()) + ")");
            mi.materials.push_back(std::move(mm));
        }
        if (m.faces.empty()) issue(L::Warning, Kind::XModel, a.name, "no triangles");
        if (m.bones.size() > 128) issue(L::Warning, Kind::XModel, a.name, std::to_string(m.bones.size()) + " bones (IW4 limit is 128)");
        an.models[a.name] = std::move(mi);
    }

    // Anims (+ the model whose skeleton they fit best, as the compiler picks the local pose).
    for (const Asset& a : index.assets) {
        if (a.kind != Kind::XAnim) continue;
        AnimInfo ai;
        xanimexport::Anim x;
        if (!xanimexport::load(a.path.string(), x, ai.error)) {
            issue(L::Error, Kind::XAnim, a.name, "can't read: " + ai.error);
            an.anims[a.name] = ai;
            continue;
        }
        ai.ok = true;
        ai.frames = static_cast<int>(x.frames.size());
        ai.framerate = x.framerate;
        for (const std::string& p : x.parts) ai.parts.push_back(lower(p));
        for (const auto& n : x.notes) ai.notes.push_back({n.frame, n.name});
        const std::string& n = a.name;
        ai.looping = n.size() > 5 && (n.compare(n.size() - 5, 5, "_loop") == 0 || n.compare(n.size() - 5, 5, "_idle") == 0);
        std::set<std::string> parts(ai.parts.begin(), ai.parts.end());
        for (const auto& [mn, mi] : an.models) {
            size_t shared = 0;
            for (const std::string& b : mi.bones) shared += parts.count(b);
            if (shared > ai.modelBones) ai.modelBones = shared, ai.model = mn;
        }
        if (ai.model.empty()) issue(L::Warning, Kind::XAnim, a.name, "no zone_source model has any of its bones (compiled against an identity pose)");
        else use(Kind::XModel, ai.model, "anim " + a.name + " (skeleton)");
        if (ai.frames > 256) issue(L::Info, Kind::XAnim, a.name, std::to_string(ai.frames) + " frames; resampled to 256 when compiled");
        if (ai.frames == 0) issue(L::Error, Kind::XAnim, a.name, "no frames");
        an.anims[a.name] = std::move(ai);
    }

    // Weapons.
    for (const Asset& a : index.assets) {
        if (a.kind != Kind::Weapon) continue;
        WeaponInfo wi;
        std::string text;
        if (!readFile(a.path, text)) {
            wi.error = "can't read the file";
        } else if (wi.file.parse(text, wi.error)) {
            weapondef::Weapon w;
            std::string err;
            if (weapondef::parse(text, a.name, w, wi.warnings, err)) wi.ok = true;
            else wi.error = err;
        }
        if (!wi.ok) {
            issue(L::Error, Kind::Weapon, a.name, wi.error);
            an.weapons[a.name] = std::move(wi);
            continue;
        }
        const std::string prefix = "weapon " + a.name + ": ";
        for (const std::string& w : wi.warnings)
            issue(L::Info, Kind::Weapon, a.name, w.rfind(prefix, 0) == 0 ? w.substr(prefix.size()) : w);
        bool hasGun = false;
        for (const auto& [k, v] : wi.file.kv) {
            const weapondef::RefKind rk = weapondef::refKind(k);
            if (rk == weapondef::RefKind::None || v.empty()) continue;
            WeaponRef r{k, v, rk, {}};
            const std::string by = "weapon " + a.name + " (" + k + ")";
            if (rk == weapondef::RefKind::Model) {
                r.resolved = zsrc::resolve(v, modelNames);
                if (r.resolved.empty() && iequals(k, "handModel"))
                    for (const std::string& m : modelNames)
                        if (m.rfind("viewhands", 0) == 0) {
                            r.resolved = m;
                            break;
                        }
                if (iequals(k, "gunModel")) hasGun = true;
                if (r.resolved.empty())
                    issue(L::Warning, Kind::Weapon, a.name, k + " '" + v + "' isn't in zone_source (by name: stock model or the default model)");
                else use(Kind::XModel, r.resolved, by);
            } else if (rk == weapondef::RefKind::Anim) {
                r.resolved = zsrc::resolve(v, animNames);
                if (r.resolved.empty()) issue(L::Warning, Kind::Weapon, a.name, k + " '" + v + "' isn't in zone_source/xanims (left empty)");
                else use(Kind::XAnim, r.resolved, by);
            } else if (rk == weapondef::RefKind::Fx) {
                const std::string lv = lower(v);
                if (std::find(fxNames.begin(), fxNames.end(), lv) != fxNames.end()) {
                    r.resolved = lv;
                    use(Kind::Effect, lv, by);
                }
            }
            wi.refs.push_back(std::move(r));
        }
        if (!hasGun) issue(L::Warning, Kind::Weapon, a.name, "no gunModel (nothing in first person)");
        an.weapons[a.name] = std::move(wi);
    }

    // Models with missing textures: one issue each. Weapon gun/world models drop untextured parts
    // when compiled (usually attachments hidden by hideTags anyway), so those are only info.
    for (const auto& [name, mi] : an.models) {
        if (mi.missingTextures.empty()) continue;
        bool weaponModel = false;
        if (auto it = an.usedBy.find({Kind::XModel, name}); it != an.usedBy.end())
            for (const std::string& u : it->second)
                weaponModel |= u.find("(gunModel") != std::string::npos || u.find("(worldModel") != std::string::npos;
        std::string list;
        for (size_t i = 0; i < mi.missingTextures.size() && i < 4; i++) list += (i ? ", " : "") + mi.missingTextures[i];
        if (mi.missingTextures.size() > 4) list += ", +" + std::to_string(mi.missingTextures.size() - 4) + " more";
        issue(weaponModel ? L::Info : L::Warning, Kind::XModel, name,
              std::to_string(mi.missingTextures.size()) + " material(s) without a texture in images/ or xmodels/" +
                  (weaponModel ? " (left out of the weapon model when compiled): " : " (draw untextured): ") + list);
    }

    // Effects name their textures; count those as uses so they aren't flagged unused.
    for (const Asset& a : index.assets) {
        if (a.kind != Kind::Effect) continue;
        std::string text;
        if (!readFile(a.path, text)) continue;
        const std::string lt = lower(text);
        for (const Asset& img : index.assets)
            if (img.kind == Kind::Image && lt.find(lower(img.path.stem().string())) != std::string::npos)
                use(Kind::Image, img.name, "effect " + a.name);
    }
    for (const Asset& a : index.assets)
        if (a.kind == Kind::Image && !an.usedBy.count({Kind::Image, a.name}))
            issue(L::Info, Kind::Image, a.name, "not used by any model or effect");
    std::stable_sort(an.issues.begin(), an.issues.end(), [](const Issue& x, const Issue& y) { return x.level > y.level; });
    return an;
}

// ---- file operations ----

Kind importKind(const fs::path& file) {
    const std::string e = lower(file.extension().string());
    if (e == ".xmodel_export") return Kind::XModel;
    if (e == ".xanim_export") return Kind::XAnim;
    if (e == ".png" || e == ".tga") return Kind::Image;
    if (e == ".efx") return Kind::Effect;
    if (e == ".gsc" || e == ".csc" || e == ".atr") return Kind::Script;
    if (e.empty()) return Kind::Weapon;
    return Kind::Other;
}

bool importFile(const std::string& zoneSourceDir, const fs::path& file, std::vector<fs::path>& copied, std::string& error) {
    const Kind k = importKind(file);
    if (k == Kind::Other) {
        error = file.filename().string() + ": unknown asset type (xmodel_export, xanim_export, png, tga, efx, gsc, or a weapon file)";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        error = file.string() + " doesn't exist";
        return false;
    }
    if (k == Kind::Weapon) {
        std::string text;
        WeaponFile w;
        if (!readFile(file, text) || !w.parse(text, error)) {
            error = file.filename().string() + ": " + (error.empty() ? "can't read" : error);
            return false;
        }
    }
    const fs::path dir = fs::path(zoneSourceDir) / kindSubdir(k);
    auto copyOne = [&](const fs::path& src, const fs::path& dstDir) {
        const fs::path dst = dstDir / src.filename();
        if (fs::exists(dst, ec)) {
            error = dst.string() + " already exists";
            return false;
        }
        fs::create_directories(dstDir, ec);
        if (!fs::copy_file(src, dst, ec)) {
            error = "copy " + src.string() + " failed: " + ec.message();
            return false;
        }
        copied.push_back(dst);
        return true;
    };
    if (!copyOne(file, dir)) return false;
    if (k == Kind::XModel) {
        // _lod1.._lod9 siblings and the textures the materials name, when they sit next to it.
        std::string base = lower(file.stem().string());
        if (base.size() > 5 && base.compare(base.size() - 5, 5, "_lod0") == 0) base.resize(base.size() - 5);
        for (const auto& e : fs::directory_iterator(file.parent_path(), ec)) {
            const std::string stem = lower(e.path().stem().string());
            if (e.path() != file && lower(e.path().extension().string()) == ".xmodel_export" && zsrc::isExtraLod(stem) &&
                stem.size() == base.size() + 5 && stem.compare(0, base.size(), base) == 0 && !fs::exists(dir / e.path().filename(), ec))
                copyOne(e.path(), dir);
        }
        xmodelexport::Model m;
        std::string err;
        if (xmodelexport::load(file.string(), m, err))
            for (const xmodelexport::Material& mat : m.materials) {
                if (!findTexture(zoneSourceDir, mat.texture, mat.name).empty()) continue;  // already there
                const fs::path found = findTexture(file.parent_path().parent_path().string(), mat.texture, mat.name);
                fs::path src = found;
                if (src.empty()) {  // beside the model itself
                    for (const std::string& c : {fs::path(mat.texture).filename().string(), fs::path(mat.texture).stem().string() + ".png",
                                                 fs::path(mat.texture).stem().string() + ".tga", mat.name + ".png", mat.name + ".tga"})
                        if (!c.empty() && c[0] != '.' && fs::is_regular_file(file.parent_path() / c, ec)) {
                            src = file.parent_path() / c;
                            break;
                        }
                }
                if (!src.empty() && !fs::exists(fs::path(zoneSourceDir) / "images" / src.filename(), ec))
                    copyOne(src, fs::path(zoneSourceDir) / "images");
            }
        error.clear();
    }
    return true;
}

namespace {
// The destination path of an asset's file renamed to newName (keeping the extension and any
// _LOD0 / _lodN suffix in its original case).
fs::path renamedPath(const Asset& a, const fs::path& file, const std::string& newName) {
    const std::string stem = file.stem().string(), ext = file.extension().string();
    switch (a.kind) {
        case Kind::XModel:
        case Kind::XAnim: {
            const std::string suffix = stem.size() > a.name.size() ? stem.substr(a.name.size()) : "";
            return file.parent_path() / (newName + suffix + ext);
        }
        case Kind::Weapon: return file.parent_path() / newName;
        default: return file.parent_path() / (newName + ext);
    }
}
}  // namespace

bool renameAsset(const Index& index, const Asset& a, const std::string& newName, bool updateRefs, std::vector<fs::path>& changed,
                 std::string& error) {
    if (!validName(newName)) {
        error = "names are lowercase letters, digits and _ only";
        return false;
    }
    // Effects and scripts keep their folder: only the file name changes.
    std::string fullNew = newName;
    if (a.kind == Kind::Effect || a.kind == Kind::Script) {
        const size_t slash = a.name.rfind('/');
        if (slash != std::string::npos) fullNew = a.name.substr(0, slash + 1) + newName;
        if (a.kind == Kind::Script) fullNew += lower(a.path.extension().string());
    }
    if (a.kind == Kind::Image) fullNew = newName + lower(a.path.extension().string());
    if (fullNew == a.name) return true;
    if (index.find(a.kind, fullNew)) {
        error = "there is already a " + std::string(kindName(a.kind)) + " asset named " + fullNew;
        return false;
    }
    std::vector<std::pair<fs::path, fs::path>> moves{{a.path, renamedPath(a, a.path, newName)}};
    for (const fs::path& l : a.lods) moves.push_back({l, renamedPath(a, l, newName)});
    std::error_code ec;
    for (const auto& [from, to] : moves)
        if (fs::exists(to, ec)) {
            error = to.string() + " already exists";
            return false;
        }
    for (size_t i = 0; i < moves.size(); i++) {
        fs::rename(moves[i].first, moves[i].second, ec);
        if (ec) {
            error = "rename " + moves[i].first.string() + " failed: " + ec.message();
            for (size_t j = 0; j < i; j++) fs::rename(moves[j].second, moves[j].first, ec);  // undo
            return false;
        }
    }
    if (!updateRefs) return true;
    weapondef::RefKind rk = a.kind == Kind::XModel ? weapondef::RefKind::Model
                            : a.kind == Kind::XAnim ? weapondef::RefKind::Anim
                            : a.kind == Kind::Effect ? weapondef::RefKind::Fx
                                                     : weapondef::RefKind::None;
    if (rk == weapondef::RefKind::None) return true;
    const std::vector<std::string> names = index.names(a.kind);
    for (const Asset& w : index.assets) {
        if (w.kind != Kind::Weapon) continue;
        const fs::path wp = (a.kind == Kind::Weapon && &w == &a) ? moves[0].second : w.path;
        std::string text, err;
        WeaponFile wf;
        if (!readFile(wp, text) || !wf.parse(text, err)) continue;
        bool any = false;
        for (auto& [k, v] : wf.kv) {
            if (v.empty() || weapondef::refKind(k) != rk) continue;
            const std::string target = rk == weapondef::RefKind::Fx ? lower(v) : zsrc::resolve(v, names);
            if (target == a.name) v = fullNew, any = true;
        }
        if (any) {
            if (!writeFile(wp, wf.format(), error)) return false;
            changed.push_back(wp);
        }
    }
    return true;
}

bool duplicateWeapon(const Index& index, const Asset& a, const std::string& newName, std::string& error) {
    if (a.kind != Kind::Weapon) {
        error = "only weapons can be duplicated";
        return false;
    }
    if (!validName(newName)) {
        error = "names are lowercase letters, digits and _ only";
        return false;
    }
    if (index.find(Kind::Weapon, newName)) {
        error = "weapon " + newName + " already exists";
        return false;
    }
    std::error_code ec;
    if (!fs::copy_file(a.path, a.path.parent_path() / newName, ec)) {
        error = "copy failed: " + ec.message();
        return false;
    }
    return true;
}

bool newWeapon(const std::string& zoneSourceDir, const std::string& name, const std::string& gunModel, const std::string& worldModel,
               std::string& error) {
    if (!validName(name)) {
        error = "names are lowercase letters, digits and _ only";
        return false;
    }
    const fs::path dir = fs::path(zoneSourceDir) / "weapons", p = dir / name;
    std::error_code ec;
    if (fs::exists(p, ec)) {
        error = "weapon " + name + " already exists";
        return false;
    }
    fs::create_directories(dir, ec);
    WeaponFile w;
    std::string display = name;
    for (char& c : display) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    w.set("displayName", display);
    w.set("gunModel", gunModel);
    w.set("handModel", "viewmodel_base_viewhands");
    w.set("worldModel", worldModel);
    for (const auto& [k, v] : kRifleTemplate) w.set(k, v);
    return writeFile(p, w.format(), error);
}

bool saveWeapon(const Asset& a, const WeaponFile& w, std::string& error) {
    if (a.kind != Kind::Weapon) {
        error = "not a weapon";
        return false;
    }
    return writeFile(a.path, w.format(), error);
}

}  // namespace assetman
