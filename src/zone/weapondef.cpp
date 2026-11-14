#include "zone/weapondef.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace weapondef {
namespace {

// Enum name tables (IW4 order; matched case-insensitively).
const char* const kPlayerAnimTypes[] = {"none", "default", "other", "pistol", "smg", "autorifle", "mg", "sniper",
                                        "rocketlauncher", "explosive", "grenade", "turret", "c4", "m203", "hold",
                                        "briefcase", "riotshield", "laptop", "throwingknife", nullptr};
const char* const kWeapTypes[] = {"bullet", "grenade", "projectile", "riotshield", nullptr};
const char* const kWeapClasses[] = {"rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher",
                                    "turret", "throwingknife", "non-player", "item", nullptr};
const char* const kPenetrateTypes[] = {"none", "small", "medium", "large", nullptr};
const char* const kImpactTypes[] = {"none", "bullet_small", "bullet_large", "bullet_ap", "bullet_explode", "shotgun",
                                    "shotgun_explode", "grenade_bounce", "grenade_explode", "rocket_explode",
                                    "projectile_dud", nullptr};
const char* const kInventoryTypes[] = {"primary", "offhand", "item", "altmode", "exclusive", "scavenger", nullptr};
const char* const kFireTypes[] = {"Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst",
                                  "Double Barrel", nullptr};
const char* const kOffhandClasses[] = {"None", "Frag Grenade", "Smoke Grenade", "Flash Grenade", "Throwing Knife",
                                       "Other", nullptr};
const char* const kStances[] = {"stand", "duck", "prone", nullptr};
const char* const kReticleTypes[] = {"None", "Pip-On-A-Stick", "Bouncing diamond", nullptr};
const char* const kIconRatios[] = {"1:1", "2:1", "4:1", nullptr};
const char* const kAmmoCounterClips[] = {"None", "Magazine", "ShortMagazine", "Shotgun", "Rocket", "Beltfed",
                                         "AltWeapon", nullptr};
const char* const kOverlayReticles[] = {"none", "crosshair", nullptr};
const char* const kOverlayInterfaces[] = {"None", "Javelin", "Turret Scope", nullptr};
const char* const kProjExplosions[] = {"grenade", "rocket", "flashbang", "none", "dud", "smoke", "heavy explosive", nullptr};
const char* const kStickiness[] = {"Don't stick", "Stick to all", "Stick to all, except ai and clients",
                                   "Stick to ground", "Stick to ground, maintain yaw", "Knife", nullptr};
const char* const kGuidedMissiles[] = {"None", "Sidewinder", "Hellfire", "Javelin", nullptr};

constexpr bool C = true, D = false;
using K = Kind;

std::vector<Field> makeFields() {
    std::vector<Field> f = {
        {"displayName", C, 16, K::Str},
        {"altWeapon", C, 80, K::Str},
        {"adsZoomFov", C, 40, K::Float},
        {"adsTransInTime", C, 44, K::Msec},
        {"adsTransOutTime", C, 48, K::Msec},
        {"clipSize", C, 52, K::Int},
        {"impactType", C, 56, K::Enum, kImpactTypes},
        {"fireTime", C, 60, K::Msec},
        {"dpadIconRatio", C, 64, K::Enum, kIconRatios},
        {"penetrateMultiplier", C, 68, K::Float},
        {"adsViewKickCenterSpeed", C, 72, K::Float},
        {"hipViewKickCenterSpeed", C, 76, K::Float},
        {"altRaiseTime", C, 92, K::Msec},
        {"killIcon", C, 96, K::Material},
        {"dpadIcon", C, 104, K::Material},
        {"firstRaiseTime", C, 116, K::Msec},
        {"dropAmmoMax", C, 120, K::Int},
        {"adsDofStart", C, 124, K::Float},
        {"adsDofEnd", C, 128, K::Float},
        {"motionTracker", C, 152, K::Bool},
        {"enhanced", C, 153, K::Bool},
        {"dpadIconShowsAmmo", C, 154, K::Bool},

        {"overlayName", D, 0, K::Str},
        {"handModel", D, 16, K::Model},
        {"modeName", D, 40, K::Str},
        {"playerAnimType", D, 80, K::Enum, kPlayerAnimTypes},
        {"weaponType", D, 84, K::Enum, kWeapTypes},
        {"weaponClass", D, 88, K::Enum, kWeapClasses},
        {"penetrateType", D, 92, K::Enum, kPenetrateTypes},
        {"inventoryType", D, 96, K::Enum, kInventoryTypes},
        {"fireType", D, 100, K::Enum, kFireTypes},
        {"offhandClass", D, 104, K::Enum, kOffhandClasses},
        {"stance", D, 108, K::Enum, kStances},
        {"viewFlashEffect", D, 112, K::Fx},
        {"worldFlashEffect", D, 120, K::Fx},
    };
    // Sounds 128..496 in struct order.
    const char* sounds[] = {"pickupSound", "pickupSoundPlayer", "ammoPickupSound", "ammoPickupSoundPlayer",
                            "projectileSound", "pullbackSound", "pullbackSoundPlayer", "fireSound", "fireSoundPlayer",
                            "fireSoundPlayerAkimbo", "loopFireSound", "loopFireSoundPlayer", "stopFireSound",
                            "stopFireSoundPlayer", "lastShotSound", "lastShotSoundPlayer", "emptyFireSound",
                            "emptyFireSoundPlayer", "meleeSwipeSound", "meleeSwipeSoundPlayer", "meleeHitSound",
                            "meleeMissSound", "rechamberSound", "rechamberSoundPlayer", "reloadSound", "reloadSoundPlayer",
                            "reloadEmptySound", "reloadEmptySoundPlayer", "reloadStartSound", "reloadStartSoundPlayer",
                            "reloadEndSound", "reloadEndSoundPlayer", "detonateSound", "detonateSoundPlayer",
                            "nightVisionWearSound", "nightVisionWearSoundPlayer", "nightVisionRemoveSound",
                            "nightVisionRemoveSoundPlayer", "altSwitchSound", "altSwitchSoundPlayer", "raiseSound",
                            "raiseSoundPlayer", "firstRaiseSound", "firstRaiseSoundPlayer", "putawaySound",
                            "putawaySoundPlayer", "scanSound"};
    static_assert(sizeof(sounds) / sizeof(sounds[0]) == 47, "47 weapon sounds");
    for (int i = 0; i < 47; i++) f.push_back({sounds[i], D, static_cast<uint16_t>(128 + 8 * i), K::Sound});
    const std::vector<Field> more = {
        {"viewShellEjectEffect", D, 512, K::Fx},
        {"worldShellEjectEffect", D, 520, K::Fx},
        {"viewLastShotEjectEffect", D, 528, K::Fx},
        {"worldLastShotEjectEffect", D, 536, K::Fx},
        {"reticleCenter", D, 544, K::Material},
        {"reticleSide", D, 552, K::Material},
        {"reticleCenterSize", D, 560, K::Int},
        {"reticleSideSize", D, 564, K::Int},
        {"reticleMinOfs", D, 568, K::Int},
        {"activeReticleType", D, 572, K::Enum, kReticleTypes},
        {"standMoveF", D, 576, K::Float}, {"standMoveR", D, 580, K::Float}, {"standMoveU", D, 584, K::Float},
        {"standRotP", D, 588, K::Float}, {"standRotY", D, 592, K::Float}, {"standRotR", D, 596, K::Float},
        {"strafeMoveF", D, 600, K::Float}, {"strafeMoveR", D, 604, K::Float}, {"strafeMoveU", D, 608, K::Float},
        {"strafeRotP", D, 612, K::Float}, {"strafeRotY", D, 616, K::Float}, {"strafeRotR", D, 620, K::Float},
        {"duckedOfsF", D, 624, K::Float}, {"duckedOfsR", D, 628, K::Float}, {"duckedOfsU", D, 632, K::Float},
        {"duckedMoveF", D, 636, K::Float}, {"duckedMoveR", D, 640, K::Float}, {"duckedMoveU", D, 644, K::Float},
        {"duckedRotP", D, 648, K::Float}, {"duckedRotY", D, 652, K::Float}, {"duckedRotR", D, 656, K::Float},
        {"proneOfsF", D, 660, K::Float}, {"proneOfsR", D, 664, K::Float}, {"proneOfsU", D, 668, K::Float},
        {"proneMoveF", D, 672, K::Float}, {"proneMoveR", D, 676, K::Float}, {"proneMoveU", D, 680, K::Float},
        {"proneRotP", D, 684, K::Float}, {"proneRotY", D, 688, K::Float}, {"proneRotR", D, 692, K::Float},
        {"posMoveRate", D, 696, K::Float},
        {"posProneMoveRate", D, 700, K::Float},
        {"standMoveMinSpeed", D, 704, K::Float},
        {"duckedMoveMinSpeed", D, 708, K::Float},
        {"proneMoveMinSpeed", D, 712, K::Float},
        {"posRotRate", D, 716, K::Float},
        {"posProneRotRate", D, 720, K::Float},
        {"standRotMinSpeed", D, 724, K::Float},
        {"duckedRotMinSpeed", D, 728, K::Float},
        {"proneRotMinSpeed", D, 732, K::Float},
        {"worldClipModel", D, 744, K::Model},
        {"rocketModel", D, 752, K::Model},
        {"knifeModel", D, 760, K::Model},
        {"worldKnifeModel", D, 768, K::Model},
        {"hudIcon", D, 776, K::Material},
        {"hudIconRatio", D, 784, K::Enum, kIconRatios},
        {"pickupIcon", D, 792, K::Material},
        {"pickupIconRatio", D, 800, K::Enum, kIconRatios},
        {"ammoCounterIcon", D, 808, K::Material},
        {"ammoCounterIconRatio", D, 816, K::Enum, kIconRatios},
        {"ammoCounterClip", D, 820, K::Enum, kAmmoCounterClips},
        {"startAmmo", D, 824, K::Int},
        {"ammoName", D, 832, K::Str},
        {"clipName", D, 848, K::Str},
        {"maxAmmo", D, 860, K::Int},
        {"shotCount", D, 864, K::Int},
        {"sharedAmmoCapName", D, 872, K::Str},
        {"sharedAmmoCap", D, 884, K::Int},
        {"damage", D, 888, K::Int},
        {"playerDamage", D, 892, K::Int},
        {"meleeDamage", D, 896, K::Int},
        {"fireDelay", D, 904, K::Msec},
        {"meleeDelay", D, 908, K::Msec},
        {"meleeChargeDelay", D, 912, K::Msec},
        {"detonateDelay", D, 916, K::Msec},
        {"rechamberTime", D, 920, K::Msec},
        {"rechamberTimeOneHanded", D, 924, K::Msec},
        {"rechamberBoltTime", D, 928, K::Msec},
        {"holdFireTime", D, 932, K::Msec},
        {"detonateTime", D, 936, K::Msec},
        {"meleeTime", D, 940, K::Msec},
        {"meleeChargeTime", D, 944, K::Msec},
        {"reloadTime", D, 948, K::Msec},
        {"reloadShowRocketTime", D, 952, K::Msec},
        {"reloadEmptyTime", D, 956, K::Msec},
        {"reloadAddTime", D, 960, K::Msec},
        {"reloadStartTime", D, 964, K::Msec},
        {"reloadStartAddTime", D, 968, K::Msec},
        {"reloadEndTime", D, 972, K::Msec},
        {"dropTime", D, 976, K::Msec},
        {"raiseTime", D, 980, K::Msec},
        {"altDropTime", D, 984, K::Msec},
        {"quickDropTime", D, 988, K::Msec},
        {"quickRaiseTime", D, 992, K::Msec},
        {"breachRaiseTime", D, 996, K::Msec},
        {"emptyRaiseTime", D, 1000, K::Msec},
        {"emptyDropTime", D, 1004, K::Msec},
        {"sprintInTime", D, 1008, K::Msec},
        {"sprintLoopTime", D, 1012, K::Msec},
        {"sprintOutTime", D, 1016, K::Msec},
        {"stunnedTimeBegin", D, 1020, K::Msec},
        {"stunnedTimeLoop", D, 1024, K::Msec},
        {"stunnedTimeEnd", D, 1028, K::Msec},
        {"nightVisionWearTime", D, 1032, K::Msec},
        {"nightVisionWearTimeFadeOutEnd", D, 1036, K::Msec},
        {"nightVisionWearTimePowerUp", D, 1040, K::Msec},
        {"nightVisionRemoveTime", D, 1044, K::Msec},
        {"nightVisionRemoveTimePowerDown", D, 1048, K::Msec},
        {"nightVisionRemoveTimeFadeInStart", D, 1052, K::Msec},
        {"fuseTime", D, 1056, K::Msec},
        {"aifuseTime", D, 1060, K::Msec},
        {"autoAimRange", D, 1064, K::Float},
        {"aimAssistRange", D, 1068, K::Float},
        {"aimAssistRangeAds", D, 1072, K::Float},
        {"aimPadding", D, 1076, K::Float},
        {"enemyCrosshairRange", D, 1080, K::Float},
        {"moveSpeedScale", D, 1084, K::Float},
        {"adsMoveSpeedScale", D, 1088, K::Float},
        {"sprintDurationScale", D, 1092, K::Float},
        {"adsZoomInFrac", D, 1096, K::Float},
        {"adsZoomOutFrac", D, 1100, K::Float},
        {"adsOverlayShader", D, 1104, K::Material},
        {"adsOverlayShaderLowRes", D, 1112, K::Material},
        {"adsOverlayShaderEMP", D, 1120, K::Material},
        {"adsOverlayShaderEMPLowRes", D, 1128, K::Material},
        {"adsOverlayReticle", D, 1136, K::Enum, kOverlayReticles},
        {"adsOverlayInterface", D, 1140, K::Enum, kOverlayInterfaces},
        {"adsOverlayWidth", D, 1144, K::Float},
        {"adsOverlayHeight", D, 1148, K::Float},
        {"adsOverlayWidthSplitscreen", D, 1152, K::Float},
        {"adsOverlayHeightSplitscreen", D, 1156, K::Float},
        {"adsBobFactor", D, 1160, K::Float},
        {"adsViewBobMult", D, 1164, K::Float},
        {"hipSpreadStandMin", D, 1168, K::Float},
        {"hipSpreadDuckedMin", D, 1172, K::Float},
        {"hipSpreadProneMin", D, 1176, K::Float},
        {"hipSpreadMax", D, 1180, K::Float},
        {"hipSpreadDuckedMax", D, 1184, K::Float},
        {"hipSpreadProneMax", D, 1188, K::Float},
        {"hipSpreadDecayRate", D, 1192, K::Float},
        {"hipSpreadFireAdd", D, 1196, K::Float},
        {"hipSpreadTurnAdd", D, 1200, K::Float},
        {"hipSpreadMoveAdd", D, 1204, K::Float},
        {"hipSpreadDuckedDecay", D, 1208, K::Float},
        {"hipSpreadProneDecay", D, 1212, K::Float},
        {"hipReticleSidePos", D, 1216, K::Float},
        {"adsIdleAmount", D, 1220, K::Float},
        {"hipIdleAmount", D, 1224, K::Float},
        {"adsIdleSpeed", D, 1228, K::Float},
        {"hipIdleSpeed", D, 1232, K::Float},
        {"idleCrouchFactor", D, 1236, K::Float},
        {"idleProneFactor", D, 1240, K::Float},
        {"gunMaxPitch", D, 1244, K::Float},
        {"gunMaxYaw", D, 1248, K::Float},
        {"swayMaxAngle", D, 1252, K::Float},
        {"swayLerpSpeed", D, 1256, K::Float},
        {"swayPitchScale", D, 1260, K::Float},
        {"swayYawScale", D, 1264, K::Float},
        {"swayHorizScale", D, 1268, K::Float},
        {"swayVertScale", D, 1272, K::Float},
        {"swayShellShockScale", D, 1276, K::Float},
        {"adsSwayMaxAngle", D, 1280, K::Float},
        {"adsSwayLerpSpeed", D, 1284, K::Float},
        {"adsSwayPitchScale", D, 1288, K::Float},
        {"adsSwayYawScale", D, 1292, K::Float},
        {"adsSwayHorizScale", D, 1296, K::Float},
        {"adsSwayVertScale", D, 1300, K::Float},
        {"adsViewErrorMin", D, 1304, K::Float},
        {"adsViewErrorMax", D, 1308, K::Float},
        {"physCollmap", D, 1312, K::Ignored},
        {"dualWieldViewModelOffset", D, 1320, K::Float},
        {"killIconRatio", D, 1324, K::Enum, kIconRatios},
        {"reloadAmmoAdd", D, 1328, K::Int},
        {"reloadStartAdd", D, 1332, K::Int},
        {"dropAmmoMin", D, 1336, K::Int},
        {"ammoDropClipPercentMin", D, 1340, K::Int},
        {"ammoDropClipPercentMax", D, 1344, K::Int},
        {"explosionRadius", D, 1348, K::Int},
        {"explosionRadiusMin", D, 1352, K::Int},
        {"explosionInnerDamage", D, 1356, K::Int},
        {"explosionOuterDamage", D, 1360, K::Int},
        {"damageConeAngle", D, 1364, K::Float},
        {"bulletExplDmgMult", D, 1368, K::Float},
        {"bulletExplRadiusMult", D, 1372, K::Float},
        {"projectileSpeed", D, 1376, K::Int},
        {"projectileSpeedUp", D, 1380, K::Int},
        {"projectileSpeedForward", D, 1384, K::Int},
        {"projectileActivateDist", D, 1388, K::Int},
        {"projectileLifetime", D, 1392, K::Float},
        {"timeToAccelerate", D, 1396, K::Float},
        {"projectileCurvature", D, 1400, K::Float},
        {"projectileModel", D, 1408, K::Model},
        {"projExplosionType", D, 1416, K::Enum, kProjExplosions},
        {"projExplosionEffect", D, 1424, K::Fx},
        {"projDudEffect", D, 1432, K::Fx},
        {"projExplosionSound", D, 1440, K::Sound},
        {"projDudSound", D, 1448, K::Sound},
        {"stickiness", D, 1456, K::Enum, kStickiness},
        {"lowAmmoWarningThreshold", D, 1460, K::Float},
        {"ricochetChance", D, 1464, K::Float},
        {"projTrailEffect", D, 1488, K::Fx},
        {"projBeaconEffect", D, 1496, K::Fx},
        {"projectileRed", D, 1504, K::Float},
        {"projectileGreen", D, 1508, K::Float},
        {"projectileBlue", D, 1512, K::Float},
        {"guidedMissileType", D, 1516, K::Enum, kGuidedMissiles},
        {"maxSteeringAccel", D, 1520, K::Float},
        {"projIgnitionDelay", D, 1524, K::Int},
        {"projIgnitionEffect", D, 1528, K::Fx},
        {"projIgnitionSound", D, 1536, K::Sound},
        {"adsAimPitch", D, 1544, K::Float},
        {"adsCrosshairInFrac", D, 1548, K::Float},
        {"adsCrosshairOutFrac", D, 1552, K::Float},
        {"adsGunKickReducedKickBullets", D, 1556, K::Int},
        {"adsGunKickReducedKickPercent", D, 1560, K::Float},
        {"adsGunKickPitchMin", D, 1564, K::Float},
        {"adsGunKickPitchMax", D, 1568, K::Float},
        {"adsGunKickYawMin", D, 1572, K::Float},
        {"adsGunKickYawMax", D, 1576, K::Float},
        {"adsGunKickAccel", D, 1580, K::Float},
        {"adsGunKickSpeedMax", D, 1584, K::Float},
        {"adsGunKickSpeedDecay", D, 1588, K::Float},
        {"adsGunKickStaticDecay", D, 1592, K::Float},
        {"adsViewKickPitchMin", D, 1596, K::Float},
        {"adsViewKickPitchMax", D, 1600, K::Float},
        {"adsViewKickYawMin", D, 1604, K::Float},
        {"adsViewKickYawMax", D, 1608, K::Float},
        {"adsViewScatterMin", D, 1612, K::Float},
        {"adsViewScatterMax", D, 1616, K::Float},
        {"adsSpread", D, 1620, K::Float},
        {"hipGunKickReducedKickBullets", D, 1624, K::Int},
        {"hipGunKickReducedKickPercent", D, 1628, K::Float},
        {"hipGunKickPitchMin", D, 1632, K::Float},
        {"hipGunKickPitchMax", D, 1636, K::Float},
        {"hipGunKickYawMin", D, 1640, K::Float},
        {"hipGunKickYawMax", D, 1644, K::Float},
        {"hipGunKickAccel", D, 1648, K::Float},
        {"hipGunKickSpeedMax", D, 1652, K::Float},
        {"hipGunKickSpeedDecay", D, 1656, K::Float},
        {"hipGunKickStaticDecay", D, 1660, K::Float},
        {"hipViewKickPitchMin", D, 1664, K::Float},
        {"hipViewKickPitchMax", D, 1668, K::Float},
        {"hipViewKickYawMin", D, 1672, K::Float},
        {"hipViewKickYawMax", D, 1676, K::Float},
        {"hipViewScatterMin", D, 1680, K::Float},
        {"hipViewScatterMax", D, 1684, K::Float},
        {"fightDist", D, 1688, K::Float},
        {"maxDist", D, 1692, K::Float},
        {"aiVsAiAccuracyGraph", D, 1696, K::Ignored},
        {"aiVsPlayerAccuracyGraph", D, 1704, K::Ignored},
        {"adsReloadTransTime", D, 1732, K::Msec},
        {"leftArc", D, 1736, K::Float},
        {"rightArc", D, 1740, K::Float},
        {"topArc", D, 1744, K::Float},
        {"bottomArc", D, 1748, K::Float},
        {"accuracy", D, 1752, K::Float},
        {"aiSpread", D, 1756, K::Float},
        {"playerSpread", D, 1760, K::Float},
        {"minVertTurnSpeed", D, 1764, K::Float},
        {"minHorTurnSpeed", D, 1768, K::Float},
        {"maxVertTurnSpeed", D, 1772, K::Float},
        {"maxHorTurnSpeed", D, 1776, K::Float},
        {"pitchConvergenceTime", D, 1780, K::Float},
        {"yawConvergenceTime", D, 1784, K::Float},
        {"suppressionTime", D, 1788, K::Float},
        {"maxRange", D, 1792, K::Float},
        {"animHorRotateInc", D, 1796, K::Float},
        {"playerPositionDist", D, 1800, K::Float},
        {"useHintString", D, 1808, K::Str},
        {"dropHintString", D, 1816, K::Str},
        {"horizViewJitter", D, 1832, K::Float},
        {"vertViewJitter", D, 1836, K::Float},
        {"scanSpeed", D, 1840, K::Float},
        {"scanAccel", D, 1844, K::Float},
        {"scanPauseTime", D, 1848, K::Msec},
        {"script", D, 1856, K::Str},
        {"minDamage", D, 1872, K::Int},
        {"minPlayerDamage", D, 1876, K::Int},
        {"maxDamageRange", D, 1880, K::Float},
        {"minDamageRange", D, 1884, K::Float},
        {"destabilizationRateTime", D, 1888, K::Float},
        {"destabilizationCurvatureMax", D, 1892, K::Float},
        {"destabilizeDistance", D, 1896, K::Int},
        {"fireRumble", D, 1912, K::Str},
        {"meleeImpactRumble", D, 1920, K::Str},
        {"tracerType", D, 1928, K::Ignored},
        {"turretScopeZoomRate", D, 1936, K::Float},
        {"turretScopeZoomMin", D, 1940, K::Float},
        {"turretScopeZoomMax", D, 1944, K::Float},
        {"turretOverheatUpRate", D, 1948, K::Float},
        {"turretOverheatDownRate", D, 1952, K::Float},
        {"turretOverheatPenalty", D, 1956, K::Float},
        {"turretOverheatSound", D, 1960, K::Sound},
        {"turretOverheatEffect", D, 1968, K::Fx},
        {"turretBarrelSpinRumble", D, 1976, K::Str},
        {"turretBarrelSpinSpeed", D, 1984, K::Float},
        {"turretBarrelSpinUpTime", D, 1988, K::Float},
        {"turretBarrelSpinDownTime", D, 1992, K::Float},
        {"turretBarrelSpinMaxSnd", D, 2000, K::Sound},
        {"turretBarrelSpinUpSnd1", D, 2008, K::Sound}, {"turretBarrelSpinUpSnd2", D, 2016, K::Sound},
        {"turretBarrelSpinUpSnd3", D, 2024, K::Sound}, {"turretBarrelSpinUpSnd4", D, 2032, K::Sound},
        {"turretBarrelSpinDownSnd1", D, 2040, K::Sound}, {"turretBarrelSpinDownSnd2", D, 2048, K::Sound},
        {"turretBarrelSpinDownSnd3", D, 2056, K::Sound}, {"turretBarrelSpinDownSnd4", D, 2064, K::Sound},
        {"missileConeSoundAlias", D, 2072, K::Sound},
        {"missileConeSoundAliasAtBase", D, 2080, K::Sound},
        {"missileConeSoundRadiusAtTop", D, 2088, K::Float},
        {"missileConeSoundRadiusAtBase", D, 2092, K::Float},
        {"missileConeSoundHeight", D, 2096, K::Float},
        {"missileConeSoundOriginOffset", D, 2100, K::Float},
        {"missileConeSoundVolumescaleAtCore", D, 2104, K::Float},
        {"missileConeSoundVolumescaleAtEdge", D, 2108, K::Float},
        {"missileConeSoundVolumescaleCoreSize", D, 2112, K::Float},
        {"missileConeSoundPitchAtTop", D, 2116, K::Float},
        {"missileConeSoundPitchAtBottom", D, 2120, K::Float},
        {"missileConeSoundPitchTopSize", D, 2124, K::Float},
        {"missileConeSoundPitchBottomSize", D, 2128, K::Float},
        {"missileConeSoundCrossfadeTopSize", D, 2132, K::Float},
        {"missileConeSoundCrossfadeBottomSize", D, 2136, K::Float},
    };
    f.insert(f.end(), more.begin(), more.end());
    const char* bools[] = {"shareAmmo", "lockonSupported", "requireLockonToFire", "bigExplosion", "noAdsWhenMagEmpty",
                           "avoidDropCleanup", "inheritsPerks", "crosshairColorChange", "rifleBullet", "armorPiercing",
                           "boltAction", "aimDownSight", "rechamberWhileAds", "bBulletExplosiveDamage", "cookOffHold",
                           "clipOnly", "noAmmoPickup", "adsFire", "cancelAutoHolsterWhenEmpty",
                           "disableSwitchToWhenEmpty", "suppressAmmoReserveDisplay", "laserSightDuringNightvision",
                           "markableViewmodel", "noDualWield", "flipKillIcon", "noPartialReload", "segmentedReload",
                           "blocksProne", "silenced", "isRollingGrenade", "projExplosionEffectForceNormalUp",
                           "projImpactExplode", "stickToPlayers", "hasDetonator", "disableFiring", "timedDetonation",
                           "rotate", "holdButtonToThrow", "freezeMovementWhenFiring", "thermalScope", "altModeSameWeapon",
                           "turretBarrelSpinEnabled", "missileConeSoundEnabled", "missileConeSoundPitchshiftEnabled",
                           "missileConeSoundCrossfadeEnabled", "offhandHoldIsCancelable"};
    static_assert(sizeof(bools) / sizeof(bools[0]) == 46, "46 WeaponDef bools");
    for (int i = 0; i < 46; i++) f.push_back({bools[i], D, static_cast<uint16_t>(2140 + i), K::Bool});
    return f;
}

// Anim keys -> weapAnimFiles_t slot.
const char* const kAnimKeys[kAnimCount] = {
    "rootAnim",          "idleAnim",          "emptyIdleAnim",        "fireAnim",        "holdFireAnim",
    "lastShotAnim",      "rechamberAnim",     "meleeAnim",            "meleeChargeAnim", "reloadAnim",
    "reloadEmptyAnim",   "reloadStartAnim",   "reloadEndAnim",        "raiseAnim",       "firstRaiseAnim",
    "breachRaiseAnim",   "dropAnim",          "altRaiseAnim",         "altDropAnim",     "quickRaiseAnim",
    "quickDropAnim",     "emptyRaiseAnim",    "emptyDropAnim",        "sprintInAnim",    "sprintLoopAnim",
    "sprintOutAnim",     "stunnedAnimStart",  "stunnedAnimLoop",      "stunnedAnimEnd",  "detonateAnim",
    "nightVisionWearAnim", "nightVisionRemoveAnim", "adsFireAnim",   "adsLastShotAnim", "adsRechamberAnim",
    "adsUpAnim",         "adsDownAnim"};

const char* const kHitLocKeys[kHitLocs] = {"locNone",          "locHelmet",        "locHead",          "locNeck",
                                          "locTorsoUpper",    "locTorsoLower",    "locRightArmUpper", "locLeftArmUpper",
                                          "locRightArmLower", "locLeftArmLower",  "locRightHand",     "locLeftHand",
                                          "locRightLegUpper", "locLeftLegUpper",  "locRightLegLower", "locLeftLegLower",
                                          "locRightFoot",     "locLeftFoot",      "locGun",           "locShield"};

bool iequals(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
template <class T>
void put(std::vector<uint8_t>& b, uint16_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof(T));
}

}  // namespace

const char* const kSurfaceNames[kSurfaceTypes] = {
    "default", "bark",  "brick",   "carpet",  "cloth",    "concrete", "dirt",    "flesh",       "foliage",
    "glass",   "grass", "gravel",  "ice",     "metal",    "mud",      "paper",   "plaster",     "rock",
    "sand",    "snow",  "water",   "wood",    "asphalt",  "ceramic",  "plastic", "rubber",      "cushion",
    "fruit",   "paintedmetal", "riotshield", "slush"};

const std::vector<Field>& fields() {
    static const std::vector<Field> f = makeFields();
    return f;
}

bool parse(const std::string& text, const std::string& name, Weapon& out, std::vector<std::string>& warnings,
           std::string& error) {
    out = Weapon{};
    out.name = name;
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\\') parts.push_back(cur), cur.clear();
            else cur += c;
        }
        parts.push_back(cur);
    }
    if (parts.empty() || parts[0] != "WEAPONFILE") {
        error = name + ": not a weapon file (expected WEAPONFILE\\key\\value...)";
        return false;
    }
    static const std::unordered_map<std::string, const Field*> byKey = [] {
        std::unordered_map<std::string, const Field*> m;
        for (const Field& f : fields()) m[lower(f.key)] = &f;
        return m;
    }();
    auto warn = [&](const std::string& s) { warnings.push_back("weapon " + name + ": " + s); };
    // Like the game's own weapon-file loader, every string field is non-null ("" when empty):
    // engine code reads some unchecked (e.g. szSharedAmmoCapName, iw4sp 0x85cdb).
    for (const Field& f : fields())
        if (f.kind == Kind::Str) (f.complete ? out.completeStrings : out.defStrings)[f.offset] = "";
    for (uint16_t g : {off::dAccuracyGraphName0, off::dAccuracyGraphName1}) out.defStrings[g] = "";
    for (size_t i = 1; i + 1 < parts.size(); i += 2) {
        const std::string key = parts[i], k = lower(key);
        std::string v = parts[i + 1];
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        // Arrays and lists first.
        if (k == "gunmodel" || (k.rfind("gunmodel", 0) == 0 && k.size() == 9 && std::isdigit(static_cast<unsigned char>(k[8])))) {
            int idx = k.size() == 8 ? 0 : k[8] - '1';
            if (idx >= 0 && idx < kModelCount) out.gunModels[idx] = v;
            continue;
        }
        if (k == "worldmodel" || (k.rfind("worldmodel", 0) == 0 && k.size() == 11 && std::isdigit(static_cast<unsigned char>(k[10])))) {
            int idx = k.size() == 10 ? 0 : k[10] - '1';
            if (idx >= 0 && idx < kModelCount) out.worldModels[idx] = v;
            continue;
        }
        bool done = false;
        for (int a = 0; a < kAnimCount && !done; a++)
            if (iequals(key, kAnimKeys[a])) out.anims[a] = v, done = true;
        for (int h = 0; h < kHitLocs && !done; h++)
            if (iequals(key, kHitLocKeys[h])) {
                out.locationDamage[h] = std::strtof(v.c_str(), nullptr);
                out.hasLocationDamage = true;
                done = true;
            }
        for (int s = 0; s < kSurfaceTypes && !done; s++) {
            std::string surf = kSurfaceNames[s];
            if (k == "parallel" + surf + "bounce") {
                out.parallelBounce[s] = std::strtof(v.c_str(), nullptr);
                out.hasParallelBounce = true;
                done = true;
            } else if (k == "perpendicular" + surf + "bounce") {
                out.perpendicularBounce[s] = std::strtof(v.c_str(), nullptr);
                out.hasPerpendicularBounce = true;
                done = true;
            }
        }
        if (done) continue;
        if (k == "hidetags") {
            out.hideTags = splitWords(v);
            continue;
        }
        if (k == "notetracksoundmap" || k == "notetrackrumblemap") {
            auto& dst = k == "notetracksoundmap" ? out.noteSounds : out.noteRumbles;
            std::vector<std::string> w = splitWords(v);
            for (size_t j = 0; j + 1 < w.size(); j += 2) dst.push_back({w[j], w[j + 1]});
            continue;
        }
        if (k == "bouncesound") {
            out.bounceSound = v;
            continue;
        }
        auto it = byKey.find(k);
        if (it == byKey.end()) {
            warn("unknown key '" + key + "' ignored");
            continue;
        }
        const Field& f = *it->second;
        std::vector<uint8_t>& b = f.complete ? out.complete : out.def;
        switch (f.kind) {
            case Kind::Int: put<int32_t>(b, f.offset, std::atoi(v.c_str())); break;
            case Kind::Float: put<float>(b, f.offset, std::strtof(v.c_str(), nullptr)); break;
            case Kind::Bool: b[f.offset] = std::atoi(v.c_str()) != 0; break;
            case Kind::Msec:
                put<int32_t>(b, f.offset, static_cast<int32_t>(std::lround(std::strtod(v.c_str(), nullptr) * 1000.0)));
                break;
            case Kind::Enum: {
                int idx = -1;
                for (int e = 0; f.enumNames[e]; e++)
                    if (iequals(v, f.enumNames[e])) idx = e;
                if (idx < 0 && !v.empty()) {
                    warn("'" + v + "' is not a valid " + key + "; using '" + f.enumNames[0] + "'");
                    idx = 0;
                }
                put<int32_t>(b, f.offset, std::max(idx, 0));
                break;
            }
            case Kind::Str: (f.complete ? out.completeStrings : out.defStrings)[f.offset] = v; break;
            case Kind::Material:
                if (!v.empty()) (f.complete ? out.completeMaterials : out.defMaterials)[f.offset] = v;
                break;
            case Kind::Model:
                if (!v.empty()) out.defModels[f.offset] = v;
                break;
            case Kind::Sound:
                if (!v.empty()) out.defSounds[f.offset] = v;
                break;
            case Kind::Fx:
                if (!v.empty()) out.defFx[f.offset] = v;
                break;
            case Kind::Ignored:
                if (!v.empty()) warn("'" + key + "' (" + v + ") isn't supported yet; left empty");
                break;
        }
    }
    // Stock defaults: ammo and clip named after the weapon (each weapon its own pool).
    if (out.defStrings[off::dAmmoName].empty()) out.defStrings[off::dAmmoName] = name;
    if (out.defStrings[off::dClipName].empty()) out.defStrings[off::dClipName] = name;
    // Embedded arrays the loader always provides.
    out.hasParallelBounce = out.hasPerpendicularBounce = true;
    if (!out.hasLocationDamage) out.locationDamage.fill(1.0f);
    out.hasLocationDamage = true;
    return true;
}

}  // namespace weapondef
