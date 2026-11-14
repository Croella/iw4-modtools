#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// IW4 weapon files ("WEAPONFILE\key\value\...", the format of the game's weapons/sp|mp/<name>
// files and of zone dumps) -> the x64 WeaponCompleteDef (160 bytes) + WeaponDef (2192 bytes).
//
// Layout: the 32-bit IW4 structs widened to 8-byte pointers. Every pointer offset is confirmed by
// the x64 loader (iw4mp Load_WeaponCompleteDef 0x120810, Load_WeaponDef 0x120c30), and the
// scalar fields between them sum exactly to each gap. Scalars are filled into the byte images;
// pointer fields stay zero and are listed by offset for the zone writer, which emits them in the
// loader's order.
namespace weapondef {

constexpr size_t kCompleteSize = 160, kDefSize = 2192;
constexpr int kAnimCount = 37, kModelCount = 16, kSurfaceTypes = 31, kHitLocs = 20, kHideTags = 32, kNoteMaps = 16;

enum class Kind : uint8_t {
    Str,       // XString
    Int,
    Float,
    Bool,      // 1 byte
    Msec,      // seconds in the file, int milliseconds in the struct
    Enum,      // name -> int (enumNames)
    Model,     // XModel* (by name)
    Sound,     // snd_alias_list_t* (by name, resolved at load)
    Fx,        // FxEffectDef* (by name)
    Material,  // Material* (by name)
    Ignored,   // known key we don't write (tracer, physCollmap)
};

struct Field {
    const char* key;
    bool complete;  // in WeaponCompleteDef (else WeaponDef)
    uint16_t offset;
    Kind kind;
    const char* const* enumNames = nullptr;
};

struct Weapon {
    std::string name;
    std::vector<uint8_t> complete = std::vector<uint8_t>(kCompleteSize), def = std::vector<uint8_t>(kDefSize);
    // Pointer fields by offset (in their struct), for every non-empty value.
    std::map<uint16_t, std::string> completeStrings, defStrings, completeMaterials, defMaterials, defModels, defSounds, defFx;
    std::array<std::string, kModelCount> gunModels, worldModels;
    std::array<std::string, kAnimCount> anims;
    std::string bounceSound;  // alias prefix: <prefix>_<surface> per surface type
    std::vector<std::string> hideTags;
    std::vector<std::pair<std::string, std::string>> noteSounds, noteRumbles;  // notetrack -> alias
    std::array<float, kSurfaceTypes> parallelBounce{}, perpendicularBounce{};
    std::array<float, kHitLocs> locationDamage{};
    bool hasParallelBounce = false, hasPerpendicularBounce = false, hasLocationDamage = false;
};

extern const char* const kSurfaceNames[kSurfaceTypes];  // bounce sound suffixes / bounce keys

// text: the file's contents. Unknown keys and bad enum values are reported in warnings.
bool parse(const std::string& text, const std::string& name, Weapon& out, std::vector<std::string>& warnings,
           std::string& error);

const std::vector<Field>& fields();

// Offsets of the pointer fields the writer (and zonesim) handle, in loader order.
namespace off {
// WeaponCompleteDef
constexpr uint16_t cName = 0, cWeapDef = 8, cDisplayName = 16, cHideTags = 24, cXAnims = 32, cAltWeapon = 80,
                   cKillIcon = 96, cDpadIcon = 104, cKnotCount = 132, cKnots0 = 136, cKnots1 = 144;
// WeaponDef
constexpr uint16_t dOverlayName = 0, dGunModels = 8, dHandModel = 16, dAnimsR = 24, dAnimsL = 32, dModeName = 40,
                   dNoteSoundKeys = 48, dNoteSoundValues = 56, dNoteRumbleKeys = 64, dNoteRumbleValues = 72,
                   dViewFlash = 112, dWorldFlash = 120, dSoundsBegin = 128, dSoundsEnd = 504 /* exclusive */,
                   dBounceSound = 504, dViewShellEject = 512, dWorldShellEject = 520, dViewLastShotEject = 528,
                   dWorldLastShotEject = 536, dReticleCenter = 544, dReticleSide = 552, dWorldModels = 736,
                   dWorldClipModel = 744, dRocketModel = 752, dKnifeModel = 760, dWorldKnifeModel = 768, dHudIcon = 776,
                   dPickupIcon = 792, dAmmoCounterIcon = 808, dAmmoName = 832, dClipName = 848, dSharedAmmoCapName = 872,
                   dOverlayMaterial = 1104, dOverlayMaterialLowRes = 1112, dOverlayMaterialEMP = 1120,
                   dOverlayMaterialEMPLowRes = 1128, dPhysCollmap = 1312, dProjectileModel = 1408, dProjExplosionEffect = 1424,
                   dProjDudEffect = 1432, dProjExplosionSound = 1440, dProjDudSound = 1448, dParallelBounce = 1472,
                   dPerpendicularBounce = 1480, dProjTrailEffect = 1488, dProjBeaconEffect = 1496, dProjIgnitionEffect = 1528,
                   dProjIgnitionSound = 1536, dAccuracyGraphName0 = 1696, dAccuracyGraphName1 = 1704, dKnots0 = 1712,
                   dKnots1 = 1720, dUseHint = 1808, dDropHint = 1816, dScript = 1856, dLocationDamage = 1904,
                   dFireRumble = 1912, dMeleeImpactRumble = 1920, dTracer = 1928, dTurretOverheatSound = 1960,
                   dTurretOverheatEffect = 1968, dTurretBarrelSpinRumble = 1976, dTurretBarrelSpinMaxSnd = 2000,
                   dTurretBarrelSpinUpSnd = 2008 /* 4 */, dTurretBarrelSpinDownSnd = 2040 /* 4 */,
                   dMissileConeSoundAlias = 2072, dMissileConeSoundAliasAtBase = 2080;
}  // namespace off

}  // namespace weapondef
