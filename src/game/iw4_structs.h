#pragma once
// IW4 (x64) game asset struct layouts for the map compiler.
//
// Provenance: the struct bodies below are extracted (lines 2..6120, through the XAssetList
// container) from the reverse-engineered header in the user's own iw4mp-controller project,
// which targets this exact x64 iw4mp build. The author's own static_asserts were stale 32-bit
// checks and have been stripped; the x64 asserts at the bottom of this file are validated
// against real zone bytes (see tools/structcheck and the notes below):
//   - XFile = 0x28, so zone stringCount is at +40 and assetCount at +56  (matches boneyard.ff)
//   - Material = 0x78, textureTable at +0x60                             (matches scanMaterials)
//   - MaterialTextureDef = 0x10                                          (matches scanMaterials)
//   - MapEnts.numEntityChars at +0x10, entityString (-1) at +8          (matches boneyard.ff)
//   - cmodel_t = 0x44                                                    (author assert + calc)
//
// Pointers are 8 bytes: these zones use 64-bit on-disk pointers (0xFFFFFFFFFFFFFFFF = follow).
#include <cstdint>
#include <cstddef>

typedef unsigned long DWORD;
typedef void* HANDLE;

// External types used only as pointers; forward declarations keep the layout exact and stay
// compatible with <d3d9.h> / <windows.h> if those are ever included too.
struct IDirect3DDevice9; struct IDirect3DVertexDeclaration9; struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9; struct IDirect3DBaseTexture9; struct IDirect3DTexture9;
struct IDirect3DVolumeTexture9; struct IDirect3DCubeTexture9; struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9; struct IDirect3DSurface9;
struct HWND__; struct HINSTANCE__; struct HHOOK__; struct HMONITOR__; struct HICON__;
struct HDC__; struct HKEY__; struct HMENU__; struct HBITMAP__;

// ---- extracted struct bodies (stale 32-bit static_asserts removed) ----

#define PROTOCOL 0x98
#define NUM_CUSTOM_CLASSES 15
#define FX_ELEM_FIELD_COUNT 90

#define MAX_QPATH 64
#define MAX_OSPATH 256

// This allows us to compile our structures in IDA, for easier reversing :3
#ifndef __cplusplus
#define IDA
#endif

constexpr std::size_t MAX_GENTITIES = 2048;
constexpr std::size_t ENTITYNUM_NONE = MAX_GENTITIES - 1;

typedef float vec_t;
typedef vec_t vec2_t[2];
typedef vec_t vec3_t[3];
typedef vec_t vec4_t[4];

typedef unsigned __int16 scr_string_t;

struct scr_entref_t
{
	unsigned __int16 entnum;
	unsigned __int16 classnum;
};

typedef void(*BuiltinFunction)();
typedef void(*BuiltinMethod)(scr_entref_t);

struct BuiltinFunctionDef
{
	const char* actionString;
	BuiltinFunction actionFunc;
	int type;
};

struct BuiltinMethodDef
{
	const char* actionString;
	BuiltinMethod actionFunc;
	int type;
};

enum XAssetType
{
	ASSET_TYPE_PHYSPRESET = 0x0,
	ASSET_TYPE_PHYSCOLLMAP = 0x1,
	ASSET_TYPE_XANIMPARTS = 0x2,
	ASSET_TYPE_XMODEL_SURFS = 0x3,
	ASSET_TYPE_XMODEL = 0x4,
	ASSET_TYPE_MATERIAL = 0x5,
	ASSET_TYPE_PIXELSHADER = 0x6,
	ASSET_TYPE_VERTEXSHADER = 0x7,
	ASSET_TYPE_VERTEXDECL = 0x8,
	ASSET_TYPE_TECHNIQUE_SET = 0x9,
	ASSET_TYPE_IMAGE = 0xA,
	ASSET_TYPE_SOUND = 0xB,
	ASSET_TYPE_SOUND_CURVE = 0xC,
	ASSET_TYPE_LOADED_SOUND = 0xD,
	ASSET_TYPE_CLIPMAP_SP = 0xE,
	ASSET_TYPE_CLIPMAP_MP = 0xF,
	ASSET_TYPE_COMWORLD = 0x10,
	ASSET_TYPE_GAMEWORLD_SP = 0x11,
	ASSET_TYPE_GAMEWORLD_MP = 0x12,
	ASSET_TYPE_MAP_ENTS = 0x13,
	ASSET_TYPE_FXWORLD = 0x14,
	ASSET_TYPE_GFXWORLD = 0x15,
	ASSET_TYPE_LIGHT_DEF = 0x16,
	ASSET_TYPE_UI_MAP = 0x17,
	ASSET_TYPE_FONT = 0x18,
	ASSET_TYPE_MENULIST = 0x19,
	ASSET_TYPE_MENU = 0x1A,
	ASSET_TYPE_LOCALIZE_ENTRY = 0x1B,
	ASSET_TYPE_WEAPON = 0x1C,
	ASSET_TYPE_SNDDRIVER_GLOBALS = 0x1D,
	ASSET_TYPE_FX = 0x1E,
	ASSET_TYPE_IMPACT_FX = 0x1F,
	ASSET_TYPE_AITYPE = 0x20,
	ASSET_TYPE_MPTYPE = 0x21,
	ASSET_TYPE_CHARACTER = 0x22,
	ASSET_TYPE_XMODELALIAS = 0x23,
	ASSET_TYPE_RAWFILE = 0x24,
	ASSET_TYPE_STRINGTABLE = 0x25,
	ASSET_TYPE_LEADERBOARD = 0x26,
	ASSET_TYPE_STRUCTURED_DATA_DEF = 0x27,
	ASSET_TYPE_TRACER = 0x28,
	ASSET_TYPE_VEHICLE = 0x29,
	ASSET_TYPE_ADDON_MAP_ENTS = 0x2A,
	ASSET_TYPE_COUNT = 0x2B,
	ASSET_TYPE_STRING = 0x2B,
	ASSET_TYPE_ASSETLIST = 0x2C,
	ASSET_TYPE_INVALID = -1,
};

enum GfxWarningType
{
	R_WARN_FRONTEND_ENT_LIMIT = 0x0,
	R_WARN_KNOWN_MODELS = 0x1,
	R_WARN_KNOWN_PER_CLIENT_MODELS = 0x2,
	R_WARN_KNOWN_SPECIAL_MODELS = 0x3,
	R_WARN_MODEL_LIGHT_CACHE = 0x4,
	R_WARN_SCENE_ENTITIES = 0x5,
	R_WARN_TEMP_SKIN_BUF_SIZE = 0x6,
	R_WARN_MAX_SKINNED_CACHE_VERTICES = 0x7,
	R_WARN_MAX_SCENE_SURFS_SIZE = 0x8,
	R_WARN_MAX_SURF_BUF = 0x9,
	R_WARN_PORTAL_PLANES = 0xA,
	R_WARN_MAX_CLOUDS = 0xB,
	R_WARN_MAX_DLIGHTS = 0xC,
	R_WARN_SMODEL_LIGHTING = 0xD,
	R_WARN_SMODEL_VIS_DATA_LIMIT = 0xE,
	R_WARN_SMODEL_SURF_LIMIT = 0xF,
	R_WARN_SMODEL_SURF_DELAY_LIMIT = 0x10,
	R_WARN_BSPSURF_DATA_LIMIT = 0x11,
	R_WARN_BSPSURF_PRETESS_LIMIT = 0x12,
	R_WARN_MAX_DRAWSURFS = 0x13,
	R_WARN_GFX_CODE_EMISSIVE_SURF_LIMIT = 0x14,
	R_WARN_GFX_CODE_TRANS_SURF_LIMIT = 0x15,
	R_WARN_GFX_GLASS_SURF_LIMIT = 0x16,
	R_WARN_GFX_MARK_SURF_LIMIT = 0x17,
	R_WARN_GFX_SPARK_SURF_LIMIT = 0x18,
	R_WARN_MAX_SCENE_DRAWSURFS = 0x19,
	R_WARN_MAX_FX_DRAWSURFS = 0x1A,
	R_WARN_NONEMISSIVE_FX_MATERIAL = 0x1B,
	R_WARN_NONLIT_MARK_MATERIAL = 0x1C,
	R_WARN_CMDBUF_OVERFLOW = 0x1D,
	R_WARN_MISSING_DECL_NONDEBUG = 0x1E,
	R_WARN_MAX_DYNENT_REFS = 0x1F,
	R_WARN_MAX_SCENE_DOBJ_REFS = 0x20,
	R_WARN_MAX_SCENE_MODEL_REFS = 0x21,
	R_WARN_MAX_SCENE_BRUSH_REFS = 0x22,
	R_WARN_MAX_CODE_EMISSIVE_INDS = 0x23,
	R_WARN_MAX_CODE_EMISSIVE_VERTS = 0x24,
	R_WARN_MAX_CODE_EMISSIVE_ARGS = 0x25,
	R_WARN_MAX_CODE_TRANS_INDS = 0x26,
	R_WARN_MAX_CODE_TRANS_VERTS = 0x27,
	R_WARN_MAX_CODE_TRANS_ARGS = 0x28,
	R_WARN_MAX_GLASS_INDS = 0x29,
	R_WARN_MAX_GLASS_VERTS = 0x2A,
	R_WARN_MAX_MARK_INDS = 0x2B,
	R_WARN_MAX_MARK_VERTS = 0x2C,
	R_WARN_MAX_SPARK_INDS = 0x2D,
	R_WARN_MAX_SPARK_VERTS = 0x2E,
	R_WARN_DEBUG_ALLOC = 0x2F,
	R_WARN_SPOT_LIGHT_LIMIT = 0x30,
	R_WARN_FX_ELEM_LIMIT = 0x31,
	R_WARN_FX_BOLT_LIMIT = 0x32,
	R_WARN_WORKER_CMD_SIZE = 0x33,
	R_WARN_UNKNOWN_STATICMODEL_SHADER = 0x34,
	R_WARN_UNKNOWN_XMODEL_SHADER = 0x35,
	R_WARN_DYNAMIC_INDEX_BUFFER_SIZE = 0x36,
	R_WARN_TOO_MANY_LIGHT_GRID_POINTS = 0x37,
	R_WARN_FOGABLE_2DTEXT = 0x38,
	R_WARN_FOGABLE_2DGLYPH = 0x39,
	R_WARN_SUN_QUERY = 0x3A,
	R_WARN_ESTIMATED_BOUNDS_TOO_SMALL = 0x3B,
	R_WARN_COUNT = 0x3C,
};

enum surfType_t
{
	R_SMODEL_SURFTYPE_RIGID = 0x0,
	R_SMODEL_SURFTYPE_SKINNED = 0x1,
	R_SMODEL_SURFTYPE_CACHED = 0x2,
	R_SMODEL_SURFTYPE_PRETESS = 0x3,
	R_SMODEL_SURFTYPE_COUNT = 0x4,
};

enum ShaderCodeConstants
{
	CONST_SRC_CODE_MAYBE_DIRTY_PS_BEGIN = 0x0,
	CONST_SRC_CODE_LIGHT_POSITION = 0x0,
	CONST_SRC_CODE_LIGHT_DIFFUSE = 0x1,
	CONST_SRC_CODE_LIGHT_SPECULAR = 0x2,
	CONST_SRC_CODE_LIGHT_SPOTDIR = 0x3,
	CONST_SRC_CODE_LIGHT_SPOTFACTORS = 0x4,
	CONST_SRC_CODE_LIGHT_FALLOFF_PLACEMENT = 0x5,
	CONST_SRC_CODE_PARTICLE_CLOUD_COLOR = 0x6,
	CONST_SRC_CODE_GAMETIME = 0x7,
	CONST_SRC_CODE_MAYBE_DIRTY_PS_END = 0x8,
	CONST_SRC_CODE_ALWAYS_DIRTY_PS_BEGIN = 0x8,
	CONST_SRC_CODE_PIXEL_COST_FRACS = 0x8,
	CONST_SRC_CODE_PIXEL_COST_DECODE = 0x9,
	CONST_SRC_CODE_FILTER_TAP_0 = 0xA,
	CONST_SRC_CODE_FILTER_TAP_1 = 0xB,
	CONST_SRC_CODE_FILTER_TAP_2 = 0xC,
	CONST_SRC_CODE_FILTER_TAP_3 = 0xD,
	CONST_SRC_CODE_FILTER_TAP_4 = 0xE,
	CONST_SRC_CODE_FILTER_TAP_5 = 0xF,
	CONST_SRC_CODE_FILTER_TAP_6 = 0x10,
	CONST_SRC_CODE_FILTER_TAP_7 = 0x11,
	CONST_SRC_CODE_COLOR_MATRIX_R = 0x12,
	CONST_SRC_CODE_COLOR_MATRIX_G = 0x13,
	CONST_SRC_CODE_COLOR_MATRIX_B = 0x14,
	CONST_SRC_CODE_SHADOWMAP_POLYGON_OFFSET = 0x15,
	CONST_SRC_CODE_RENDER_TARGET_SIZE = 0x16,
	CONST_SRC_CODE_ALWAYS_DIRTY_PS_END = 0x17,
	CONST_SRC_CODE_FIXED_PS_BEGIN = 0x17,
	CONST_SRC_CODE_DOF_EQUATION_VIEWMODEL_AND_FAR_BLUR = 0x17,
	CONST_SRC_CODE_DOF_EQUATION_SCENE = 0x18,
	CONST_SRC_CODE_DOF_LERP_SCALE = 0x19,
	CONST_SRC_CODE_DOF_LERP_BIAS = 0x1A,
	CONST_SRC_CODE_DOF_ROW_DELTA = 0x1B,
	CONST_SRC_CODE_MOTION_MATRIX_X = 0x1C,
	CONST_SRC_CODE_MOTION_MATRIX_Y = 0x1D,
	CONST_SRC_CODE_MOTION_MATRIX_W = 0x1E,
	CONST_SRC_CODE_SHADOWMAP_SWITCH_PARTITION = 0x1F,
	CONST_SRC_CODE_SHADOWMAP_SCALE = 0x20,
	CONST_SRC_CODE_ZNEAR = 0x21,
	CONST_SRC_CODE_LIGHTING_LOOKUP_SCALE = 0x22,
	CONST_SRC_CODE_DEBUG_BUMPMAP = 0x23,
	CONST_SRC_CODE_MATERIAL_COLOR = 0x24,
	CONST_SRC_CODE_FOG = 0x25,
	CONST_SRC_CODE_FOG_COLOR_LINEAR = 0x26,
	CONST_SRC_CODE_FOG_COLOR_GAMMA = 0x27,
	CONST_SRC_CODE_FOG_SUN_CONSTS = 0x28,
	CONST_SRC_CODE_FOG_SUN_COLOR_LINEAR = 0x29,
	CONST_SRC_CODE_FOG_SUN_COLOR_GAMMA = 0x2A,
	CONST_SRC_CODE_FOG_SUN_DIR = 0x2B,
	CONST_SRC_CODE_GLOW_SETUP = 0x2C,
	CONST_SRC_CODE_GLOW_APPLY = 0x2D,
	CONST_SRC_CODE_COLOR_BIAS = 0x2E,
	CONST_SRC_CODE_COLOR_TINT_BASE = 0x2F,
	CONST_SRC_CODE_COLOR_TINT_DELTA = 0x30,
	CONST_SRC_CODE_COLOR_TINT_QUADRATIC_DELTA = 0x31,
	CONST_SRC_CODE_OUTDOOR_FEATHER_PARMS = 0x32,
	CONST_SRC_CODE_ENVMAP_PARMS = 0x33,
	CONST_SRC_CODE_SUN_SHADOWMAP_PIXEL_ADJUST = 0x34,
	CONST_SRC_CODE_SPOT_SHADOWMAP_PIXEL_ADJUST = 0x35,
	CONST_SRC_CODE_COMPOSITE_FX_DISTORTION = 0x36,
	CONST_SRC_CODE_POSTFX_FADE_EFFECT = 0x37,
	CONST_SRC_CODE_VIEWPORT_DIMENSIONS = 0x38,
	CONST_SRC_CODE_FRAMEBUFFER_READ = 0x39,
	CONST_SRC_CODE_FIXED_PS_END = 0x3A,
	CONST_SRC_CODE_NON_PS_BEGIN = 0x3A,
	CONST_SRC_CODE_BASE_LIGHTING_COORDS = 0x3A,
	CONST_SRC_CODE_LIGHT_PROBE_AMBIENT = 0x3B,
	CONST_SRC_CODE_NEARPLANE_ORG = 0x3C,
	CONST_SRC_CODE_NEARPLANE_DX = 0x3D,
	CONST_SRC_CODE_NEARPLANE_DY = 0x3E,
	CONST_SRC_CODE_CLIP_SPACE_LOOKUP_SCALE = 0x3F,
	CONST_SRC_CODE_CLIP_SPACE_LOOKUP_OFFSET = 0x40,
	CONST_SRC_CODE_PARTICLE_CLOUD_MATRIX0 = 0x41,
	CONST_SRC_CODE_PARTICLE_CLOUD_MATRIX1 = 0x42,
	CONST_SRC_CODE_PARTICLE_CLOUD_MATRIX2 = 0x43,
	CONST_SRC_CODE_PARTICLE_CLOUD_SPARK_COLOR0 = 0x44,
	CONST_SRC_CODE_PARTICLE_CLOUD_SPARK_COLOR1 = 0x45,
	CONST_SRC_CODE_PARTICLE_CLOUD_SPARK_COLOR2 = 0x46,
	CONST_SRC_CODE_PARTICLE_FOUNTAIN_PARM0 = 0x47,
	CONST_SRC_CODE_PARTICLE_FOUNTAIN_PARM1 = 0x48,
	CONST_SRC_CODE_DEPTH_FROM_CLIP = 0x49,
	CONST_SRC_CODE_CODE_MESH_ARG_0 = 0x4A,
	CONST_SRC_CODE_CODE_MESH_ARG_1 = 0x4B,
	CONST_SRC_CODE_CODE_MESH_ARG_LAST = 0x4B,
	CONST_SRC_CODE_NON_PS_END = 0x4C,
	CONST_SRC_CODE_COUNT_FLOAT4 = 0x4C,
	CONST_SRC_FIRST_CODE_MATRIX = 0x4C,
	CONST_SRC_CODE_VIEW_MATRIX = 0x4C,
	CONST_SRC_CODE_INVERSE_VIEW_MATRIX = 0x4D,
	CONST_SRC_CODE_TRANSPOSE_VIEW_MATRIX = 0x4E,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_VIEW_MATRIX = 0x4F,
	CONST_SRC_CODE_PROJECTION_MATRIX = 0x50,
	CONST_SRC_CODE_INVERSE_PROJECTION_MATRIX = 0x51,
	CONST_SRC_CODE_TRANSPOSE_PROJECTION_MATRIX = 0x52,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_PROJECTION_MATRIX = 0x53,
	CONST_SRC_CODE_VIEW_PROJECTION_MATRIX = 0x54,
	CONST_SRC_CODE_INVERSE_VIEW_PROJECTION_MATRIX = 0x55,
	CONST_SRC_CODE_TRANSPOSE_VIEW_PROJECTION_MATRIX = 0x56,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_VIEW_PROJECTION_MATRIX = 0x57,
	CONST_SRC_CODE_SHADOW_LOOKUP_MATRIX = 0x58,
	CONST_SRC_CODE_INVERSE_SHADOW_LOOKUP_MATRIX = 0x59,
	CONST_SRC_CODE_TRANSPOSE_SHADOW_LOOKUP_MATRIX = 0x5A,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_SHADOW_LOOKUP_MATRIX = 0x5B,
	CONST_SRC_CODE_WORLD_OUTDOOR_LOOKUP_MATRIX = 0x5C,
	CONST_SRC_CODE_INVERSE_WORLD_OUTDOOR_LOOKUP_MATRIX = 0x5D,
	CONST_SRC_CODE_TRANSPOSE_WORLD_OUTDOOR_LOOKUP_MATRIX = 0x5E,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_OUTDOOR_LOOKUP_MATRIX = 0x5F,
	CONST_SRC_CODE_WORLD_MATRIX0 = 0x60,
	CONST_SRC_CODE_INVERSE_WORLD_MATRIX0 = 0x61,
	CONST_SRC_CODE_TRANSPOSE_WORLD_MATRIX0 = 0x62,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_MATRIX0 = 0x63,
	CONST_SRC_CODE_WORLD_VIEW_MATRIX0 = 0x64,
	CONST_SRC_CODE_INVERSE_WORLD_VIEW_MATRIX0 = 0x65,
	CONST_SRC_CODE_TRANSPOSE_WORLD_VIEW_MATRIX0 = 0x66,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_VIEW_MATRIX0 = 0x67,
	CONST_SRC_CODE_WORLD_VIEW_PROJECTION_MATRIX0 = 0x68,
	CONST_SRC_CODE_INVERSE_WORLD_VIEW_PROJECTION_MATRIX0 = 0x69,
	CONST_SRC_CODE_TRANSPOSE_WORLD_VIEW_PROJECTION_MATRIX0 = 0x6A,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_VIEW_PROJECTION_MATRIX0 = 0x6B,
	CONST_SRC_CODE_WORLD_MATRIX1 = 0x6C,
	CONST_SRC_CODE_INVERSE_WORLD_MATRIX1 = 0x6D,
	CONST_SRC_CODE_TRANSPOSE_WORLD_MATRIX1 = 0x6E,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_MATRIX1 = 0x6F,
	CONST_SRC_CODE_WORLD_VIEW_MATRIX1 = 0x70,
	CONST_SRC_CODE_INVERSE_WORLD_VIEW_MATRIX1 = 0x71,
	CONST_SRC_CODE_TRANSPOSE_WORLD_VIEW_MATRIX1 = 0x72,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_VIEW_MATRIX1 = 0x73,
	CONST_SRC_CODE_WORLD_VIEW_PROJECTION_MATRIX1 = 0x74,
	CONST_SRC_CODE_INVERSE_WORLD_VIEW_PROJECTION_MATRIX1 = 0x75,
	CONST_SRC_CODE_TRANSPOSE_WORLD_VIEW_PROJECTION_MATRIX1 = 0x76,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_VIEW_PROJECTION_MATRIX1 = 0x77,
	CONST_SRC_CODE_WORLD_MATRIX2 = 0x78,
	CONST_SRC_CODE_INVERSE_WORLD_MATRIX2 = 0x79,
	CONST_SRC_CODE_TRANSPOSE_WORLD_MATRIX2 = 0x7A,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_MATRIX2 = 0x7B,
	CONST_SRC_CODE_WORLD_VIEW_MATRIX2 = 0x7C,
	CONST_SRC_CODE_INVERSE_WORLD_VIEW_MATRIX2 = 0x7D,
	CONST_SRC_CODE_TRANSPOSE_WORLD_VIEW_MATRIX2 = 0x7E,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_VIEW_MATRIX2 = 0x7F,
	CONST_SRC_CODE_WORLD_VIEW_PROJECTION_MATRIX2 = 0x80,
	CONST_SRC_CODE_INVERSE_WORLD_VIEW_PROJECTION_MATRIX2 = 0x81,
	CONST_SRC_CODE_TRANSPOSE_WORLD_VIEW_PROJECTION_MATRIX2 = 0x82,
	CONST_SRC_CODE_INVERSE_TRANSPOSE_WORLD_VIEW_PROJECTION_MATRIX2 = 0x83,
	CONST_SRC_TOTAL_COUNT = 0x84,
	CONST_SRC_NONE = 0x85,
};

enum FsThread
{
	FS_THREAD_MAIN = 0x0,
	FS_THREAD_STREAM = 0x1,
	FS_THREAD_DATABASE = 0x2,
	FS_THREAD_BACKEND = 0x3,
	FS_THREAD_SERVER = 0x4,
	FS_THREAD_COUNT = 0x5,
	FS_THREAD_INVALID = 0x6,
};

enum TextureSemantic : char
{
	TS_2D = 0x0,
	TS_FUNCTION = 0x1,
	TS_COLOR_MAP = 0x2,
	TS_DETAIL_MAP = 0x3,
	TS_UNUSED_2 = 0x4,
	TS_NORMAL_MAP = 0x5,
	TS_UNUSED_3 = 0x6,
	TS_UNUSED_4 = 0x7,
	TS_SPECULAR_MAP = 0x8,
	TS_UNUSED_5 = 0x9,
	TS_UNUSED_6 = 0xA,
	TS_WATER_MAP = 0xB,
};


enum materialSurfType_t
{
	SURF_TYPE_DEFAULT,
	SURF_TYPE_BARK,
	SURF_TYPE_BRICK,
	SURF_TYPE_CARPET,
	SURF_TYPE_CLOTH,
	SURF_TYPE_CONCRETE,
	SURF_TYPE_DIRT,
	SURF_TYPE_FLESH,
	SURF_TYPE_FOLIAGE,
	SURF_TYPE_GLASS,
	SURF_TYPE_GRASS,
	SURF_TYPE_GRAVEL,
	SURF_TYPE_ICE,
	SURF_TYPE_METAL,
	SURF_TYPE_MUD,
	SURF_TYPE_PAPER,
	SURF_TYPE_PLASTER,
	SURF_TYPE_ROCK,
	SURF_TYPE_SAND,
	SURF_TYPE_SNOW,
	SURF_TYPE_WATER,
	SURF_TYPE_WOOD,
	SURF_TYPE_ASPHALT,
	SURF_TYPE_CERAMIC,
	SURF_TYPE_PLASTIC,
	SURF_TYPE_RUBBER,
	SURF_TYPE_CUSHION,
	SURF_TYPE_FRUIT,
	SURF_TYPE_PAINTED_METAL,
	SURF_TYPE_RIOT_SHIELD,
	SURF_TYPE_SLUSH,

	SURF_TYPE_COUNT
};

enum : unsigned __int16
{
	DVAR_NONE = 0,	// No flags
	DVAR_ARCHIVE = 1 << 0,	// Set to cause it to be saved to config_mp.cfg of the client
	DVAR_LATCH = 1 << 1,	// Will only change when C code next does a Dvar_Get(), so it can't be changed
	// without proper initialization. Modified will be set, even though the value hasn't changed yet
	DVAR_CHEAT = 1 << 2,	// Can not be changed if cheats are disabled
	DVAR_CODINFO = 1 << 3,	// On change, this is sent to all clients (if you are host)
	DVAR_SCRIPTINFO = 1 << 4,
	DVAR_TEMP = 1 << 5, // Best educated guess
	DVAR_SAVED = 1 << 6,
	DVAR_INTERNAL = 1 << 7, // Best educated guess
	DVAR_EXTERNAL = 1 << 8,	// Created by a set command
	DVAR_USERINFO = 1 << 9,	// Sent to server on connect or change
	DVAR_SERVERINFO = 1 << 10, // Sent in response to front end requests
	DVAR_INIT = 1 << 11, // Don't allow change from console at all
	DVAR_SYSTEMINFO = 1 << 12, // Will be duplicated on all clients
	DVAR_ROM = 1 << 13, // Display only, cannot be set by user at all
	DVAR_CHANGEABLE_RESET = 1 << 14,
	DVAR_AUTOEXEC = 1 << 15, // isLoadingAutoExecGlobalFlag is always false so it should be never set by the game
};

enum ImageCategory : char
{
	IMG_CATEGORY_UNKNOWN = 0x0,
	IMG_CATEGORY_AUTO_GENERATED = 0x1,
	IMG_CATEGORY_LIGHTMAP = 0x2,
	IMG_CATEGORY_LOAD_FROM_FILE = 0x3,
	IMG_CATEGORY_RAW = 0x4,
	IMG_CATEGORY_FIRST_UNMANAGED = 0x5,
	IMG_CATEGORY_WATER = 0x5,
	IMG_CATEGORY_RENDERTARGET = 0x6,
	IMG_CATEGORY_TEMP = 0x7,
};

enum buttons_t
{
	KB_LEFT = 0x0,
	KB_RIGHT = 0x1,
	KB_FORWARD = 0x2,
	KB_BACK = 0x3,
	KB_LOOKUP = 0x4,
	KB_LOOKDOWN = 0x5,
	KB_MOVELEFT = 0x6,
	KB_MOVERIGHT = 0x7,
	KB_STRAFE = 0x8,
	KB_SPEED = 0x9,
	KB_UP = 0xA,
	KB_DOWN = 0xB,
	KB_ANYUP = 0xC,
	KB_MLOOK = 0xD,
	KB_ATTACK = 0xE,
	KB_BREATH = 0xF,
	KB_FRAG = 0x10,
	KB_OFFHANDSECONDARY = 0x11,
	KB_MELEE = 0x12,
	KB_ACTIVATE = 0x13,
	KB_RELOAD = 0x14,
	KB_USE_RELOAD = 0x15,
	KB_PRONE = 0x16,
	KB_CROUCH = 0x17,
	KB_THROW = 0x18,
	KB_SPRINT = 0x19,
	KB_NIGHTVISION = 0x1A,
	KB_TALK = 0x1B,
	NUM_BUTTONS = 0x1C
};

enum DvarSetSource
{
	DVAR_SOURCE_INTERNAL = 0x0,
	DVAR_SOURCE_EXTERNAL = 0x1,
	DVAR_SOURCE_SCRIPT = 0x2,
	DVAR_SOURCE_DEVGUI = 0x3,
};

enum dvar_type : char
{
	DVAR_TYPE_BOOL = 0x0,
	DVAR_TYPE_FLOAT = 0x1,
	DVAR_TYPE_FLOAT_2 = 0x2,
	DVAR_TYPE_FLOAT_3 = 0x3,
	DVAR_TYPE_FLOAT_4 = 0x4,
	DVAR_TYPE_INT = 0x5,
	DVAR_TYPE_ENUM = 0x6,
	DVAR_TYPE_STRING = 0x7,
	DVAR_TYPE_COLOR = 0x8,
	DVAR_TYPE_FLOAT_3_COLOR = 0x9,
	DVAR_TYPE_COUNT = 0xA,
};

enum clientState_t
{
	CS_FREE = 0x0,
	CS_ZOMBIE = 0x1,
	CS_RECONNECTING = 0x2,
	CS_CONNECTED = 0x3,
	CS_CLIENTLOADING = 0x4,
	CS_ACTIVE = 0x5,
};

enum serverState_t
{
	SS_DEAD = 0x0,
	SS_LOADING = 0x1,
	SS_GAME = 0x2,
};

enum GfxLightType
{
	GFX_LIGHT_TYPE_NONE = 0x0,
	GFX_LIGHT_TYPE_DIR = 0x1,
	GFX_LIGHT_TYPE_SPOT = 0x2,
	GFX_LIGHT_TYPE_OMNI = 0x3,
	GFX_LIGHT_TYPE_COUNT = 0x4,
	GFX_LIGHT_TYPE_DIR_SHADOWMAP = 0x4,
	GFX_LIGHT_TYPE_SPOT_SHADOWMAP = 0x5,
	GFX_LIGHT_TYPE_OMNI_SHADOWMAP = 0x6,
	GFX_LIGHT_TYPE_COUNT_WITH_SHADOWMAP_VERSIONS = 0x7,
};

enum errorParm_t
{
	ERR_FATAL = 0x0,
	ERR_DROP = 0x1,
	ERR_SERVERDISCONNECT = 0x2,
	ERR_DISCONNECT = 0x3,
	ERR_SCRIPT = 0x4,
	ERR_SCRIPT_DROP = 0x5,
	ERR_LOCALIZATION = 0x6,
	ERR_MAPLOADERRORSUMMARY = 0x7
};

enum ConfigString
{
	CS_VOTE_TIME = 0x11,
	CS_VOTE_STRING = 0x12,
	CS_VOTE_YES = 0x13,
	CS_VOTE_NO = 0x14,
	CS_VOTE_MAPNAME = 0x15,
	CS_VOTE_GAMETYPE = 0x16,
	CS_MODELS = 0x465,	// Models (confirmed) 1125
	// 1580<>1637 models not cached (something's going on, not sure what)
	CS_MODELS_LAST = 0x664,	// End of models
	CS_SHELLSHOCKS = 0x985,
	CS_WEAPONFILES = 0xAF5, // 2805 Confirmed
	CS_WEAPONFILES_LAST = 0xFA3, // Confirmed too // 4003
	CS_ITEMS = 4138, // Correct! CS_ITEMS is actually an item **COUNT**

	CS_LAST = CS_ITEMS,
	MAX_CONFIGSTRINGS = 4139
}; // Incomplete

enum conChannel_t
{
	CON_CHANNEL_DONT_FILTER,
	CON_CHANNEL_ERROR,
	CON_CHANNEL_GAMENOTIFY,
	CON_CHANNEL_BOLDGAME,
	CON_CHANNEL_SUBTITLE,
	CON_CHANNEL_OBITUARY,
	CON_CHANNEL_LOGFILEONLY,
	CON_CHANNEL_CONSOLEONLY,
	CON_CHANNEL_GFX,
	CON_CHANNEL_SOUND,
	CON_CHANNEL_FILES,
	CON_CHANNEL_DEVGUI,
	CON_CHANNEL_PROFILE,
	CON_CHANNEL_UI,
	CON_CHANNEL_CLIENT,
	CON_CHANNEL_SERVER,
	CON_CHANNEL_SYSTEM,
	CON_CHANNEL_PLAYERWEAP,
	CON_CHANNEL_AI,
	CON_CHANNEL_ANIM,
	CON_CHANNEL_PHYS,
	CON_CHANNEL_FX,
	CON_CHANNEL_LEADERBOARDS,
	CON_CHANNEL_PARSERSCRIPT,
	CON_CHANNEL_SCRIPT,
	CON_CHANNEL_NETWORK,

	CON_BUILTIN_CHANNEL_COUNT,
};

enum meansOfDeath_t
{
	MOD_UNKNOWN = 0x0,
	MOD_PISTOL_BULLET = 0x1,
	MOD_RIFLE_BULLET = 0x2,
	MOD_EXPLOSIVE_BULLET = 0x3,
	MOD_GRENADE = 0x4,
	MOD_GRENADE_SPLASH = 0x5,
	MOD_PROJECTILE = 0x6,
	MOD_PROJECTILE_SPLASH = 0x7,
	MOD_MELEE = 0x8,
	MOD_HEAD_SHOT = 0x9,
	MOD_CRUSH = 0xA,
	MOD_FALLING = 0xB,
	MOD_SUICIDE = 0xC,
	MOD_TRIGGER_HURT = 0xD,
	MOD_EXPLOSIVE = 0xE,
	MOD_IMPACT = 0xF,
	MOD_NUM = 0x10,
};

enum
{
	FL_GODMODE = 1 << 0,
	FL_DEMI_GODMODE = 1 << 1,
	FL_NOTARGET = 1 << 2,
	FL_NO_KNOCKBACK = 1 << 3,
	FL_NO_RADIUS_DAMAGE = 1 << 4,
	FL_SUPPORTS_LINKTO = 1 << 12,
	FL_NO_AUTO_ANIM_UPDATE = 1 << 13,
	FL_GRENADE_TOUCH_DAMAGE = 1 << 14,
	FL_STABLE_MISSILES = 1 << 17,
	FL_REPEAT_ANIM_UPDATE = 1 << 18,
	FL_VEHICLE_TARGET = 1 << 19,
	FL_GROUND_ENT = 1 << 20,
	FL_CURSOR_HINT = 1 << 21,
	FL_MISSILE_ATTRACTOR = 1 << 23,
	FL_WEAPON_BEING_GRABBED = 1 << 24,
	FL_DELETE = 1 << 25,
	FL_BOUNCE = 1 << 26,
	FL_MOVER_SLIDE = 1 << 27
};

enum ClassNum : unsigned int
{
	CLASS_NUM_ENTITY = 0x0,
	CLASS_NUM_HUDELEM = 0x1,
	CLASS_NUM_PATHNODE = 0x2,
	CLASS_NUM_VEHICLENODE = 0x3,
	CLASS_NUM_VEHTRACK_SEGMENT = 0x4,
	CLASS_NUM_FXENTITY = 0x5,
	CLASS_NUM_COUNT = 0x6,
};

enum hitLocation_t
{
	HITLOC_NONE,
	HITLOC_HELMET,
	HITLOC_HEAD,
	HITLOC_NECK,
	HITLOC_TORSO_UPR,
	HITLOC_TORSO_LWR,
	HITLOC_R_ARM_UPR,
	HITLOC_L_ARM_UPR,
	HITLOC_R_ARM_LWR,
	HITLOC_L_ARM_LWR,
	HITLOC_R_HAND,
	HITLOC_L_HAND,
	HITLOC_R_LEG_UPR,
	HITLOC_L_LEG_UPR,
	HITLOC_R_LEG_LWR,
	HITLOC_L_LEG_LWR,
	HITLOC_R_FOOT,
	HITLOC_L_FOOT,
	HITLOC_GUN,
	HITLOC_SHIELD,
	HITLOC_NUM
};

enum svscmd_type
{
	SV_CMD_CAN_IGNORE = 0x0,
	SV_CMD_RELIABLE = 0x1,
};

enum RumbleSourceType
{
	RUMBLESOURCE_INVALID = 0x0,
	RUMBLESOURCE_ENTITY = 0x1,
	RUMBLESOURCE_POS = 0x2,
};

struct FxEffectDef;
struct pathnode_t;
struct pathnode_tree_t;
struct GfxPortal;
struct Statement_s;
struct MenuEventHandlerSet;
struct menuDef_t;

struct cspField_t
{
	const char* szName;
	int iOffset;
	int iFieldType;
};

struct CmdArgs
{
	int nesting;
	int localClientNum[8];
	int controllerIndex[8];
	int argc[8];
	const char** argv[8];
};


struct cmd_function_s
{
	cmd_function_s* next;
	const char* name;
	const char* autoCompleteDir;
	const char* autoCompleteExt;
	void(__cdecl* function)();
	int flags;
};

#pragma pack(push, 4)
struct kbutton_t
{
	int down[2];
	unsigned int downtime;
	unsigned int msec;
	bool active;
	bool wasPressed;
};
#pragma pack(pop)

enum PlayerHandIndex
{
	WEAPON_HAND_RIGHT = 0x0,
	WEAPON_HAND_LEFT = 0x1,
	NUM_WEAPON_HANDS = 0x2,
	WEAPON_HAND_DEFAULT = 0x0,
};

enum KeyCatch_t
{
	KEYCATCH_MASK_ANY = -1,
	KEYCATCH_CONSOLE = 0x1,
	KEYCATCH_UNKNOWN2 = 0x2,
	KEYCATCH_UNKNOWN4 = 0x4,
	KEYCATCH_LOCATION_SELECTION = 0x8,
	KEYCATCH_UI = 0x10,
	KEYCATCH_CHAT = 0x20,
	KEYCATCH_UNKNOWN40 = 0x40,
	KEYCATCH_UNKNOWN80 = 0x80,
	KEYCATCH_UNKNOWN100 = 0x100,
};

enum keyNum_t
{
	K_NONE = 0x0,
	K_FIRSTGAMEPADBUTTON_RANGE_1 = 0x1, // First Gamepad 1
	K_BUTTON_A = 0x1,
	K_BUTTON_B = 0x2,
	K_BUTTON_X = 0x3,
	K_BUTTON_Y = 0x4,
	K_BUTTON_LSHLDR = 0x5,
	K_BUTTON_RSHLDR = 0x6,
	K_LASTGAMEPADBUTTON_RANGE_1 = 0x6, // Last Gamepad 1
	K_TAB = 0x9,
	K_ENTER = 0xD,
	K_FIRSTGAMEPADBUTTON_RANGE_2 = 0xE, // First Gamepad 2
	K_BUTTON_START = 0xE,
	K_BUTTON_BACK = 0xF,
	K_BUTTON_LSTICK = 0x10,
	K_BUTTON_RSTICK = 0x11,
	K_BUTTON_LTRIG = 0x12,
	K_BUTTON_RTRIG = 0x13,
	K_FIRSTDPAD = 0x14, // First Dpad
	K_DPAD_UP = 0x14,
	K_DPAD_DOWN = 0x15,
	K_DPAD_LEFT = 0x16,
	K_DPAD_RIGHT = 0x17,
	K_LASTDPAD = 0x17, // Last Dpad
	K_DPAD_LEFTRIGHT = 0x18,
	K_DPAD_UPDOWN = 0x19,
	K_LASTGAMEPADBUTTON_RANGE_2 = 0x19, // Last Gamepad 2
	K_ESCAPE = 0x1B,
	K_FIRSTGAMEPADBUTTON_RANGE_3 = 0x1C, // First Gamepad 3
	K_FIRSTAPAD = 0x1C, // First APad
	K_APAD_UP = 0x1C,
	K_APAD_DOWN = 0x1D,
	K_APAD_LEFT = 0x1E,
	K_APAD_RIGHT = 0x1F,
	K_LASTAPAD = 0x1F, // Last APad
	K_LASTGAMEPADBUTTON_RANGE_3 = 0x1F, // Last Gamepad 3
	K_SPACE = 0x20,
	K_BACKSPACE = 0x7F,
	K_ASCII_FIRST = 0x80,
	K_ASCII_181 = 0x80,
	K_ASCII_191 = 0x81,
	K_ASCII_223 = 0x82,
	K_ASCII_224 = 0x83,
	K_ASCII_225 = 0x84,
	K_ASCII_228 = 0x85,
	K_ASCII_229 = 0x86,
	K_ASCII_230 = 0x87,
	K_ASCII_231 = 0x88,
	K_ASCII_232 = 0x89,
	K_ASCII_233 = 0x8A,
	K_ASCII_236 = 0x8B,
	K_ASCII_241 = 0x8C,
	K_ASCII_242 = 0x8D,
	K_ASCII_243 = 0x8E,
	K_ASCII_246 = 0x8F,
	K_ASCII_248 = 0x90,
	K_ASCII_249 = 0x91,
	K_ASCII_250 = 0x92,
	K_ASCII_252 = 0x93,
	K_END_ASCII_CHARS = 0x94,
	K_COMMAND = 0x96,
	K_CAPSLOCK = 0x97,
	K_POWER = 0x98,
	K_PAUSE = 0x99,
	K_UPARROW = 0x9A,
	K_DOWNARROW = 0x9B,
	K_LEFTARROW = 0x9C,
	K_RIGHTARROW = 0x9D,
	K_ALT = 0x9E,
	K_CTRL = 0x9F,
	K_SHIFT = 0xA0,
	K_INS = 0xA1,
	K_DEL = 0xA2,
	K_PGDN = 0xA3,
	K_PGUP = 0xA4,
	K_HOME = 0xA5,
	K_END = 0xA6,
	K_F1 = 0xA7,
	K_F2 = 0xA8,
	K_F3 = 0xA9,
	K_F4 = 0xAA,
	K_F5 = 0xAB,
	K_F6 = 0xAC,
	K_F7 = 0xAD,
	K_F8 = 0xAE,
	K_F9 = 0xAF,
	K_F10 = 0xB0,
	K_F11 = 0xB1,
	K_F12 = 0xB2,
	K_F13 = 0xB3,
	K_F14 = 0xB4,
	K_F15 = 0xB5,
	K_KP_HOME = 0xB6,
	K_KP_UPARROW = 0xB7,
	K_KP_PGUP = 0xB8,
	K_KP_LEFTARROW = 0xB9,
	K_KP_5 = 0xBA,
	K_KP_RIGHTARROW = 0xBB,
	K_KP_END = 0xBC,
	K_KP_DOWNARROW = 0xBD,
	K_KP_PGDN = 0xBE,
	K_KP_ENTER = 0xBF,
	K_KP_INS = 0xC0,
	K_KP_DEL = 0xC1,
	K_KP_SLASH = 0xC2,
	K_KP_MINUS = 0xC3,
	K_KP_PLUS = 0xC4,
	K_KP_NUMLOCK = 0xC5,
	K_KP_STAR = 0xC6,
	K_KP_EQUALS = 0xC7,
	K_MOUSE1 = 0xC8,
	K_MOUSE2 = 0xC9,
	K_MOUSE3 = 0xCA,
	K_MOUSE4 = 0xCB,
	K_MOUSE5 = 0xCC,
	K_MWHEELDOWN = 0xCD,
	K_MWHEELUP = 0xCE,
	K_AUX1 = 0xCF,
	K_AUX2 = 0xD0,
	K_AUX3 = 0xD1,
	K_AUX4 = 0xD2,
	K_AUX5 = 0xD3,
	K_AUX6 = 0xD4,
	K_AUX7 = 0xD5,
	K_AUX8 = 0xD6,
	K_AUX9 = 0xD7,
	K_AUX10 = 0xD8,
	K_AUX11 = 0xD9,
	K_AUX12 = 0xDA,
	K_AUX13 = 0xDB,
	K_AUX14 = 0xDC,
	K_AUX15 = 0xDD,
	K_AUX16 = 0xDE,
	K_LAST_KEY = 0xDF,
};

enum uiMenuCommand_t
{
	UIMENU_NONE = 0x0,
	UIMENU_MAIN = 0x1,
	UIMENU_INGAME = 0x2,
	UIMENU_PREGAME = 0x3,
	UIMENU_POSTGAME = 0x4,
	UIMENU_SCRIPT_POPUP = 0x5,
	UIMENU_SCOREBOARD = 0x6,
	UIMENU_PARTY = 0x7,
	UIMENU_GAMELOBBY = 0x8,
	UIMENU_PRIVATELOBBY = 0x9,
	UIMENU_ENDOFGAME = 0xA,
	UIMENU_MIGRATION = 0xB,
};

struct __declspec(align(4)) PhysPreset
{
	const char* name;
	int type;
	float mass;
	float bounce;
	float friction;
	float bulletForceScale;
	float explosiveForceScale;
	const char* sndAliasPrefix;
	float piecesSpreadFraction;
	float piecesUpwardVelocity;
	bool tempDefaultToCylinder;
	bool perSurfaceSndAlias;
};

struct Bounds
{
	float midPoint[3];
	float halfSize[3];
};

struct cplane_s
{
	float normal[3];
	float dist;
	unsigned char type;
	unsigned char pad[3];
};

struct cbrushside_t
{
	cplane_s* plane;
	unsigned __int16 materialNum;
	char firstAdjacentSideOffset;
	char edgeCount;
};

struct cbrush_t
{
	unsigned short numsides;
	unsigned short glassPieceIndex;
	cbrushside_t* sides;
	unsigned char* baseAdjacentSide;
	unsigned short axialMaterialNum[2][3];
	unsigned char firstAdjacentSideOffsets[2][3];
	unsigned char edgeCount[2][3];
};

struct BrushWrapper
{
	Bounds bounds;
	cbrush_t brush;
	int totalEdgeCount;
	cplane_s* planes;
};

struct PhysGeomInfo
{
	BrushWrapper* brushWrapper;
	int type;
	float orientation[3][3];
	Bounds bounds;
};

struct PhysMass
{
	float centerOfMass[3];
	float momentsOfInertia[3];
	float productsOfInertia[3];
};

struct PhysCollmap
{
	const char* name;
	unsigned int count;
	PhysGeomInfo* geoms;
	PhysMass mass;
	Bounds bounds;
};

union XAnimIndices
{
	char* _1;
	unsigned __int16* _2;
	void* data;
};

struct XAnimNotifyInfo
{
	unsigned __int16 name;
	float time;
};

union XAnimDynamicFrames
{
	uint8_t(*_1)[3];
	unsigned __int16(*_2)[3];
};

union XAnimDynamicIndices
{
	char _1[1];
	unsigned __int16 _2[1];
};

struct __declspec(align(4)) XAnimPartTransFrames
{
	float mins[3];
	float size[3];
	XAnimDynamicFrames frames;
	XAnimDynamicIndices indices;
};

union XAnimPartTransData
{
	XAnimPartTransFrames frames;
	float frame0[3];
};

struct XAnimPartTrans
{
	unsigned __int16 size;
	char smallTrans;
	XAnimPartTransData u;
};

struct __declspec(align(4)) XAnimDeltaPartQuatDataFrames2
{
	__int16(*frames)[2];
	XAnimDynamicIndices indices;
};

union XAnimDeltaPartQuatData2
{
	XAnimDeltaPartQuatDataFrames2 frames;
	__int16 frame0[2];
};

struct XAnimDeltaPartQuat2
{
	unsigned __int16 size;
	XAnimDeltaPartQuatData2 u;
};

struct __declspec(align(4)) XAnimDeltaPartQuatDataFrames
{
	__int16(*frames)[4];
	XAnimDynamicIndices indices;
};

union XAnimDeltaPartQuatData
{
	XAnimDeltaPartQuatDataFrames frames;
	__int16 frame0[4];
};

struct XAnimDeltaPartQuat
{
	unsigned __int16 size;
	XAnimDeltaPartQuatData u;
};

struct XAnimDeltaPart
{
	XAnimPartTrans* trans;
	XAnimDeltaPartQuat2* quat2;
	XAnimDeltaPartQuat* quat;
};

struct XAnimParts
{
	const char* name;
	unsigned __int16 dataByteCount;
	unsigned __int16 dataShortCount;
	unsigned __int16 dataIntCount;
	unsigned __int16 randomDataByteCount;
	unsigned __int16 randomDataIntCount;
	unsigned __int16 numframes;
	char flags;
	unsigned char boneCount[10];
	unsigned char notifyCount;
	char assetType;
	bool isDefault;
	unsigned int randomDataShortCount;
	unsigned int indexCount;
	float framerate;
	float frequency;
	unsigned __int16* names;
	char* dataByte;
	__int16* dataShort;
	int* dataInt;
	__int16* randomDataShort;
	char* randomDataByte;
	int* randomDataInt;
	XAnimIndices indices;
	XAnimNotifyInfo* notify;
	XAnimDeltaPart* deltaPart;
};

struct XSurfaceVertexInfo
{
	unsigned __int16 vertCount[4];
	unsigned __int16* vertsBlend;
};

union GfxColor
{
	unsigned int packed;
	unsigned char array[4];
};

union PackedTexCoords
{
	unsigned int packed;
};

union PackedUnitVec
{
	unsigned int packed;
	char array[4];
};

struct GfxPackedVertex
{
	float xyz[3];
	float binormalSign;
	GfxColor color;
	PackedTexCoords texCoord;
	PackedUnitVec normal;
	PackedUnitVec tangent;
};

struct XSurfaceCollisionAabb
{
	unsigned __int16 mins[3];
	unsigned __int16 maxs[3];
};

struct XSurfaceCollisionNode
{
	XSurfaceCollisionAabb aabb;
	unsigned __int16 childBeginIndex;
	unsigned __int16 childCount;
};

struct XSurfaceCollisionLeaf
{
	unsigned __int16 triangleBeginIndex;
};

struct XSurfaceCollisionTree
{
	float trans[3];
	float scale[3];
	unsigned int nodeCount;
	XSurfaceCollisionNode* nodes;
	unsigned int leafCount;
	XSurfaceCollisionLeaf* leafs;
};

struct XRigidVertList
{
	unsigned __int16 boneOffset;
	unsigned __int16 vertCount;
	unsigned __int16 triOffset;
	unsigned __int16 triCount;
	XSurfaceCollisionTree* collisionTree;
};

struct DObjSkelMat
{
	float axis[3][4];
	float origin[4];
};

struct XSurface
{
	char tileMode;
	bool deformed;
	unsigned __int16 vertCount;
	unsigned __int16 triCount;
	char zoneHandle;
	unsigned __int16 baseTriIndex;
	unsigned __int16 baseVertIndex;
	unsigned __int16* triIndices;
	XSurfaceVertexInfo vertInfo;
	GfxPackedVertex* verts0;
	unsigned int vertListCount;
	XRigidVertList* vertList;
	int partBits[6];
};

struct XModelSurfs
{
	const char* name;
	XSurface* surfs;
	unsigned __int16 numsurfs;
	int partBits[6];
};

struct DObjAnimMat
{
	float quat[4];
	float trans[3];
	float transWeight;
};

struct GfxDrawSurfFields
{
	unsigned __int64 objectId : 16;
	unsigned __int64 reflectionProbeIndex : 8;
	unsigned __int64 hasGfxEntIndex : 1;
	unsigned __int64 customIndex : 5;
	unsigned __int64 materialSortedIndex : 12;
	unsigned __int64 prepass : 2;
	unsigned __int64 useHeroLighting : 1;
	unsigned __int64 sceneLightIndex : 8;
	unsigned __int64 surfType : 4;
	unsigned __int64 primarySortKey : 6;
	unsigned __int64 unused : 1;
};

union GfxDrawSurf
{
	GfxDrawSurfFields fields;
	unsigned __int64 packed;
};

struct MaterialInfo
{
	const char* name;
	unsigned char gameFlags;
	unsigned char sortKey;
	unsigned char textureAtlasRowCount;
	unsigned char textureAtlasColumnCount;
	GfxDrawSurf drawSurf;
	unsigned int surfaceTypeBits;
	unsigned __int16 hashIndex;
};

struct MaterialStreamRouting
{
	char source;
	char dest;
};

struct MaterialVertexStreamRouting
{
	MaterialStreamRouting data[13];
	IDirect3DVertexDeclaration9* decl[16];
};

struct MaterialVertexDeclaration
{
	const char* name;
	char streamCount;
	bool hasOptionalSource;
	MaterialVertexStreamRouting routing;
};

struct GfxVertexShaderLoadDef
{
	unsigned int* program;
	unsigned __int16 programSize;
	unsigned __int16 loadForRenderer;
};

struct MaterialVertexShaderProgram
{
	IDirect3DVertexShader9* vs;
	GfxVertexShaderLoadDef loadDef;
};

struct MaterialVertexShader
{
	const char* name;
	MaterialVertexShaderProgram prog;
};

struct GfxPixelShaderLoadDef
{
	unsigned int* program;
	unsigned __int16 programSize;
	unsigned __int16 loadForRenderer;
};

struct MaterialPixelShaderProgram
{
	IDirect3DPixelShader9* ps;
	GfxPixelShaderLoadDef loadDef;
};

struct MaterialPixelShader
{
	const char* name;
	MaterialPixelShaderProgram prog;
};


enum MaterialShaderArgumentType : unsigned __int16
{
	MTL_ARG_MATERIAL_VERTEX_CONST = 0x0,
	MTL_ARG_LITERAL_VERTEX_CONST = 0x1,
	MTL_ARG_MATERIAL_PIXEL_SAMPLER = 0x2,
	MTL_ARG_CODE_PRIM_BEGIN = 0x3,
	MTL_ARG_CODE_VERTEX_CONST = 0x3,
	MTL_ARG_CODE_PIXEL_SAMPLER = 0x4,
	MTL_ARG_CODE_PIXEL_CONST = 0x5,
	MTL_ARG_CODE_PRIM_END = 0x6,
	MTL_ARG_MATERIAL_PIXEL_CONST = 0x6,
	MTL_ARG_LITERAL_PIXEL_CONST = 0x7,
	MTL_ARG_COUNT = 0x8,
};


struct MaterialArgumentCodeConst
{
	unsigned __int16 index;
	char firstRow;
	char rowCount;
};

union MaterialArgumentDef
{
	float* literalConst;
	MaterialArgumentCodeConst codeConst;
	unsigned int codeSampler;
	unsigned int nameHash;
};

struct MaterialShaderArgument
{
	MaterialShaderArgumentType type;
	unsigned __int16 dest;
	MaterialArgumentDef u;
};

struct MaterialPass
{
	MaterialVertexDeclaration* vertexDecl;
	MaterialVertexShader* vertexShader;
	MaterialPixelShader* pixelShader;
	char perPrimArgCount;
	char perObjArgCount;
	char stableArgCount;
	char customSamplerFlags;
	MaterialShaderArgument* args;
};

struct visionSetVars_t
{
	bool glowEnable;
	float glowBloomCutoff;
	float glowBloomDesaturation;
	float glowBloomIntensity0;
	float glowBloomIntensity1;
	float glowRadius0;
	float glowRadius1;
	float glowSkyBleedIntensity0;
	float glowSkyBleedIntensity1;
	bool filmEnable;
	float filmBrightness;
	float filmContrast;
	float filmDesaturation;
	float filmDesaturationDark;
	bool filmInvert;
	float filmLightTint[3];
	float filmMediumTint[3];
	float filmDarkTint[3];
	bool charPrimaryUseTweaks;
	float charPrimaryDiffuseScale;
	float charPrimarySpecularScale;
};

struct visField_t
{
	const char* name;
	int offset;
	// 0 is for int, 1 is for float, otherwise it's a vector
	int fieldType;
};

enum OffhandClass
{
	OFFHAND_CLASS_NONE = 0x0,
	OFFHAND_CLASS_FRAG_GRENADE = 0x1,
	OFFHAND_CLASS_SMOKE_GRENADE = 0x2,
	OFFHAND_CLASS_FLASH_GRENADE = 0x3,
	OFFHAND_CLASS_THROWINGKNIFE = 0x4,
	OFFHAND_CLASS_OTHER = 0x5,
	OFFHAND_CLASS_COUNT = 0x6,
};

enum ViewLockTypes
{
	PLAYERVIEWLOCK_NONE = 0x0,
	PLAYERVIEWLOCK_FULL = 0x1,
	PLAYERVIEWLOCK_WEAPONJITTER = 0x2,
	PLAYERVIEWLOCKCOUNT = 0x3,
};

struct SprintState
{
	int sprintButtonUpRequired;
	int sprintDelay;
	int lastSprintStart;
	int lastSprintEnd;
	int sprintStartMaxLength;
};

struct MantleState
{
	float yaw;
	int timer;
	int transIndex;
	int flags;
};

struct PlayerActiveWeaponState
{
	int weapAnim;
	int weaponTime;
	int weaponDelay;
	int weaponRestrictKickTime;
	int weaponState;
	int weapHandFlags;
	unsigned int weaponShotCount;
};

struct PlayerEquippedWeaponState
{
	bool usedBefore;
	bool dualWielding;
	char weaponModel;
	bool needsRechamber[2];
};

struct GlobalAmmo
{
	int ammoType;
	int ammoCount;
};

struct ClipAmmo
{
	int clipIndex;
	int ammoCount[2];
};

struct PlayerWeaponCommonState
{
	int offHandIndex;
	OffhandClass offhandPrimary;
	OffhandClass offhandSecondary;
	unsigned int weapon;
	unsigned int primaryWeaponForAltMode;
	int weapFlags;
	float fWeaponPosFrac;
	float aimSpreadScale;
	int adsDelayTime;
	int spreadOverride;
	int spreadOverrideState;
	PlayerHandIndex lastWeaponHand;
	GlobalAmmo ammoNotInClip[15];
	ClipAmmo ammoInClip[15];
	int weapLockFlags;
	int weapLockedEntnum;
	float weapLockedPos[3];
	int weaponIdleTime;
};

enum ActionSlotType
{
	ACTIONSLOTTYPE_DONOTHING = 0x0,
	ACTIONSLOTTYPE_SPECIFYWEAPON = 0x1,
	ACTIONSLOTTYPE_ALTWEAPONTOGGLE = 0x2,
	ACTIONSLOTTYPE_NIGHTVISION = 0x3,
	ACTIONSLOTTYPECOUNT = 0x4,
};

struct ActionSlotParam_SpecifyWeapon
{
	unsigned int index;
};

struct ActionSlotParam
{
	ActionSlotParam_SpecifyWeapon specifyWeapon;
};

enum objectiveState_t
{
	OBJST_EMPTY = 0x0,
	OBJST_ACTIVE = 0x1,
	OBJST_INVISIBLE = 0x2,
	OBJST_DONE = 0x3,
	OBJST_CURRENT = 0x4,
	OBJST_FAILED = 0x5,
	OBJST_NUMSTATES = 0x6,
};

struct objective_t
{
	objectiveState_t state;
	float origin[3];
	int entNum;
	int teamNum;
	int icon;
};

enum he_type_t
{
	HE_TYPE_FREE = 0x0,
	HE_TYPE_TEXT = 0x1,
	HE_TYPE_VALUE = 0x2,
	HE_TYPE_PLAYERNAME = 0x3,
	HE_TYPE_MAPNAME = 0x4,
	HE_TYPE_GAMETYPE = 0x5,
	HE_TYPE_MATERIAL = 0x6,
	HE_TYPE_TIMER_DOWN = 0x7,
	HE_TYPE_TIMER_UP = 0x8,
	HE_TYPE_TIMER_STATIC = 0x9,
	HE_TYPE_TENTHS_TIMER_DOWN = 0xA,
	HE_TYPE_TENTHS_TIMER_UP = 0xB,
	HE_TYPE_TENTHS_TIMER_STATIC = 0xC,
	HE_TYPE_CLOCK_DOWN = 0xD,
	HE_TYPE_CLOCK_UP = 0xE,
	HE_TYPE_WAYPOINT = 0xF,
	HE_TYPE_COUNT = 0x10,
};

struct hud_color
{
	char r;
	char g;
	char b;
	char a;
};

union hudelem_color_t
{
	hud_color __s0;
	int rgba;
};

struct hudelem_s
{
	he_type_t type;
	float x;
	float y;
	float z;
	int targetEntNum;
	float fontScale;
	float fromFontScale;
	int fontScaleStartTime;
	int fontScaleTime;
	int font;
	int alignOrg;
	int alignScreen;
	hudelem_color_t color;
	hudelem_color_t fromColor;
	int fadeStartTime;
	int fadeTime;
	int label;
	int width;
	int height;
	int materialIndex;
	int fromWidth;
	int fromHeight;
	int scaleStartTime;
	int scaleTime;
	float fromX;
	float fromY;
	int fromAlignOrg;
	int fromAlignScreen;
	int moveStartTime;
	int moveTime;
	int time;
	int duration;
	float value;
	int text;
	float sort;
	hudelem_color_t glowColor;
	int fxBirthTime;
	int fxLetterTime;
	int fxDecayStartTime;
	int fxDecayDuration;
	int soundID;
	int flags;
};

struct game_hudelem_s
{
	hudelem_s elem;
	int clientNum;
	int team;
	int archived;
};

enum
{
	PMF_PRONE = 1 << 0,
	PMF_DUCKED = 1 << 1,
	PMF_MANTLE = 1 << 2,
	PMF_LADDER = 1 << 3,
	PMF_SIGHT_AIMING = 1 << 4,
	PMF_BACKWARDS_RUN = 1 << 5,
	PMF_WALKING = 1 << 6,
	PMF_TIME_HARDLANDING = 1 << 7,
	PMF_TIME_KNOCKBACK = 1 << 8,
	PMF_PRONEMOVE_OVERRIDDEN = 1 << 9,
	PMF_RESPAWNED = 1 << 10,
	PMF_FROZEN = 1 << 11,
	PMF_LADDER_FALL = 1 << 12,
	PMF_JUMPING = 1 << 13,
	PMF_SPRINTING = 1 << 14,
	PMF_SHELLSHOCKED = 1 << 15,
	PMF_MELEE_CHARGE = 1 << 16,
	PMF_NO_SPRINT = 1 << 17,
	PMF_NO_JUMP = 1 << 18,
	PMF_REMOTE_CONTROLLING = 1 << 19,
	PMF_ANIM_SCRIPTED = 1 << 20,
	PMF_UNK1 = 1 << 21,
	PMF_DIVING = 1 << 22,
};

enum
{
	POF_INVULNERABLE = 1 << 0,
	POF_REMOTE_EYES = 1 << 1,
	POF_LASER_ALTVIEW = 1 << 2,
	POF_THERMAL_VISION = 1 << 3,
	POF_THERMAL_VISION_OVERLAY_FOF = 1 << 4,
	POF_REMOTE_CAMERA_SOUNDS = 1 << 5,
	POF_ALT_SCENE_REAR_VIEW = 1 << 6,
	POF_ALT_SCENE_TAG_VIEW = 1 << 7,
	POF_SHIELD_ATTACHED_TO_WORLD_MODEL = 1 << 8,
	POF_DONT_LERP_VIEWANGLES = 1 << 9,
	POF_EMP_JAMMED = 1 << 10,
	POF_FOLLOW = 1 << 11,
	POF_PLAYER = 1 << 12,
	POF_SPEC_ALLOW_CYCLE = 1 << 13,
	POF_SPEC_ALLOW_FREELOOK = 1 << 14,
	POF_AC130 = 1 << 15,
	POF_COMPASS_PING = 1 << 16,
	POF_ADS_THIRD_PERSON_TOGGLE = 1 << 17,
};

enum pmtype_t
{
	PM_NORMAL = 0x0,
	PM_NORMAL_LINKED = 0x1,
	PM_NOCLIP = 0x2,
	PM_UFO = 0x3,
	PM_MPVIEWER = 0x4,
	PM_SPECTATOR = 0x5,
	PM_INTERMISSION = 0x6,
	PM_LASTSTAND = 0x7,
	PM_DEAD = 0x8,
	PM_DEAD_LINKED = 0x9,
};

enum
{
	EF_NONSOLID_BMODEL = 1 << 0,
	EF_TELEPORT_BIT = 1 << 1,
	EF_CROUCHING = 1 << 2,
	EF_PRONE = 1 << 3,
	EF_UNK1 = 1 << 4,
	EF_NODRAW = 1 << 5,
	EF_TIMED_OBJECT = 1 << 6,
	EF_VOTED = 1 << 7,
	EF_TALK = 1 << 8,
	EF_FIRING = 1 << 9,
	EF_TURRET_ACTIVE_PRONE = 1 << 10,
	EF_TURRET_ACTIVE_DUCK = 1 << 11,
	EF_LOCK_LIGHT_VIS = 1 << 12,
	EF_AIM_ASSIST = 1 << 13,
	EF_LOOP_RUMBLE = 1 << 14,
	EF_LASER_SIGHT = 1 << 15,
	EF_MANTLE = 1 << 16,
	EF_DEAD = 1 << 17,
	EF_ADS = 1 << 18,
	EF_NEW = 1 << 19,
	EF_VEHICLE_ACTIVE = 1 << 20,
	EF_JAMMING = 1 << 21,
	EF_COMPASS_PING = 1 << 22,
	EF_SOFT = 1 << 23,
};

enum
{
	PLF_ANGLES_LOCKED = 1 << 0,
	PLF_USES_OFFSET = 1 << 1,
	PLF_WEAPONVIEW_ONLY = 1 << 2,
};

enum
{
	PWF_USE_RELOAD = 1 << 0,
	PWF_USING_OFFHAND = 1 << 1,
	PWF_HOLDING_BREATH = 1 << 2,
	PWF_FRIENDLY_FIRE = 1 << 3,
	PWF_ENEMY_FIRE = 1 << 4,
	PWF_NO_ADS = 1 << 5,
	PWF_USING_NIGHTVISION = 1 << 6,
	PWF_DISABLE_WEAPONS = 1 << 7,
	PWF_TRIGGER_LEFT_FIRE = 1 << 8,
	PWF_TRIGGER_DOUBLE_FIRE = 1 << 9,
	PWF_USING_RECOILSCALE = 1 << 10,
	PWF_DISABLE_WEAPON_SWAPPING = 1 << 11,
	PWF_DISABLE_OFFHAND_WEAPONS = 1 << 12,
	PWF_SWITCHING_TO_RIOTSHIELD = 1 << 13,
	// IW5 flags backported
	PWF_DISABLE_WEAPON_PICKUP = 1 << 16
};

enum perksEnum
{
	PERK_JAMRADAR = 0x0,
	PERK_ACCURACY = 0x1,
	PERK_FASTRELOAD = 0x2,
	PERK_RATEOFFIRE = 0x3,
	PERK_EXTRABREATH = 0x4,
	PERK_EXTRABP = 0x5,
	PERK_GRENADEDEATH = 0x6,
	PERK_PISTOLDEATH = 0x7,
	PERK_QUIETMOVE = 0x8,
	PERK_PARABOLIC = 0x9,
	PERK_LONGERSPRINT = 0xA,
	PERK_DETECTEXPLOSIVE = 0xB,
	PERK_EXPOSEENEMY = 0xC,
	PERK_BULLETDMG = 0xD,
	PERK_EXTRAAMMO = 0xE,
	PERK_TWOPRIMARIES = 0xF,
	PERK_ARMORVEST = 0x10,
	PERK_FRAGGRENADE = 0x11,
	PERK_SPECIALGRENADE = 0x12,
	PERK_FASTMANTLE = 0x13,
	PERK_JUMPDIVE = 0x14,
	PERK_EXPLOSIVEBULLETS = 0x15,
	PERK_SCAVENGER = 0x16,
	PERK_EXTENDEDMAGS = 0x17,
	PERK_LIGHTWEIGHT = 0x18,
	PERK_MARATHON = 0x19,
	PERK_QUICKDRAW = 0x1A,
	PERK_COLDBLOODED = 0x1B,
	PERK_HEARTBREAKER = 0x1C,
	PERK_SELECTIVEHEARING = 0x1D,
	PERK_FASTSNIPE = 0x1E,
	PERK_IMPROVEDEXTRABREATH = 0x1F,
	PERK_SPYGAME = 0x20,
	PERK_AUTOMANTLE = 0x21,
	PERK_FASTSPRINTRECOVERY = 0x22,
	PERK_EXTENDEDMELEE = 0x23,
	PERK_COUNT = 0x24,
	PERK_UNKNOWN = 0x24,
};

constexpr std::size_t PERK_ARRAY_COUNT = 2;

struct playerState_s
{
	int commandTime;
	int pm_type;
	int pm_time;
	int pm_flags;
	int otherFlags;
	int linkFlags;
	int bobCycle;
	float origin[3];
	float velocity[3];
	int grenadeTimeLeft;
	int throwbackGrenadeOwner;
	int throwbackGrenadeTimeLeft;
	unsigned int throwbackWeaponIndex;
	int remoteEyesEnt;
	int remoteEyesTagname;
	int remoteControlEnt;
	int foliageSoundTime;
	int gravity;
	float leanf;
	int speed;
	float delta_angles[3];
	int groundEntityNum;
	float vLadderVec[3];
	int jumpTime;
	float jumpOriginZ;
	int legsTimer;
	int legsAnim;
	int torsoTimer;
	int torsoAnim;
	int legsAnimDuration;
	int torsoAnimDuration;
	int damageTimer;
	int damageDuration;
	int flinchYawAnim;
	int corpseIndex;
	int movementDir;
	int eFlags;
	int eventSequence;
	int events[4];
	unsigned int eventParms[4];
	int oldEventSequence;
	int unpredictableEventSequence;
	int unpredictableEventSequenceOld;
	int unpredictableEvents[4];
	unsigned int unpredictableEventParms[4];
	int clientNum; // 260
	int viewmodelIndex;
	float viewangles[3];
	int viewHeightTarget;
	float viewHeightCurrent;
	int viewHeightLerpTime;
	int viewHeightLerpTarget;
	int viewHeightLerpDown;
	float viewAngleClampBase[2];
	float viewAngleClampRange[2];
	int damageEvent;
	int damageYaw;
	int damagePitch;
	int damageCount;
	int damageFlags;
	int stats[4];
	float proneDirection;
	float proneDirectionPitch;
	float proneTorsoPitch;
	ViewLockTypes viewlocked;
	int viewlocked_entNum;
	float linkAngles[3];
	float linkWeaponAngles[3];
	int linkWeaponEnt;
	int loopSound;
	int cursorHint;
	int cursorHintString;
	int cursorHintEntIndex;
	int cursorHintDualWield;
	int iCompassPlayerInfo;
	int radarEnabled;
	int radarBlocked;
	int radarMode;
	int locationSelectionInfo;
	SprintState sprintState;
	float holdBreathScale;
	int holdBreathTimer;
	float moveSpeedScaleMultiplier;
	MantleState mantleState;
	PlayerActiveWeaponState weapState[2];
	unsigned int weaponsEquipped[15];
	PlayerEquippedWeaponState weapEquippedData[15];
	PlayerWeaponCommonState weapCommon;
	float meleeChargeYaw;
	int meleeChargeDist;
	int meleeChargeTime;
	unsigned int perks[2];
	unsigned int perkSlots[8];
	ActionSlotType actionSlotType[4];
	ActionSlotParam actionSlotParam[4];
	int weaponHudIconOverrides[6];
	int animScriptedType;
	int shellshockIndex;
	int shellshockTime;
	int shellshockDuration;
	float dofNearStart;
	float dofNearEnd;
	float dofFarStart;
	float dofFarEnd;
	float dofNearBlur;
	float dofFarBlur;
	float dofViewmodelStart;
	float dofViewmodelEnd;
	objective_t objective[32];
	int deltaTime;
	int killCamEntity;
	int killCamLookAtEntity;
	int killCamClientNum;
	struct
	{
		hudelem_s current[31];
		hudelem_s archival[31];
	} hud;
	unsigned int partBits[6];
	int recoilScale;
	int diveDirection;
	int stunTime;
};


enum LocSelInputState
{
	LOC_SEL_INPUT_NONE = 0x0,
	LOC_SEL_INPUT_CONFIRM = 0x1,
	LOC_SEL_INPUT_CANCEL = 0x2,
};

struct field_t
{
	int cursor;
	int scroll;
	int drawWidth;
	int widthInPixels;
	float charHeight;
	int fixedSize;
	char buffer[256];
};

struct KeyState
{
	int down;
	int repeats;
	const char* binding;
};

struct PlayerKeyState
{
	field_t chatField;
	int chat_team;
	int overstrikeMode;
	int anyKeyDown;
	KeyState keys[256];
	LocSelInputState locSelInputState;
};

struct keyname_t
{
	const char* name;
	int keynum;
};

struct clSnapshot_t
{
	playerState_s ps;
	int valid;
	int snapFlags;
	int serverTime;
	int messageNum;
	int deltaNum;
	int ping;
	int cmdNum;
	int numEntities;
	int numClients;
	int parseEntitiesIndex;
	int parseClientsIndex;
	int serverCommandNum;
};


enum StanceState
{
	CL_STANCE_STAND = 0x0,
	CL_STANCE_CROUCH = 0x1,
	CL_STANCE_PRONE = 0x2,
};

struct ClientArchiveData
{
	int serverTime;
	float origin[3];
	float velocity[3];
	int bobCycle;
	int movementDir;
	float viewangles[3];
	int locationSelectionInfo;
	float selectedLocation[2];
	float selectedLocationAngle;
};

struct outPacket_t
{
	int p_cmdNumber;
	int p_serverTime;
	int p_realtime;
};

enum team_t
{
	TEAM_FREE = 0x0,
	TEAM_AXIS = 0x1,
	TEAM_ALLIES = 0x2,
	TEAM_SPECTATOR = 0x3,
	TEAM_NUM_TEAMS = 0x4,
};

struct clientState_s
{
	int clientIndex;
	team_t team;
	int modelindex;
	int dualWielding;
	int riotShieldNext;
	int attachModelIndex[6];
	int attachTagIndex[6];
	char name[16];
	float maxSprintTimeMultiplier;
	int rank;
	int prestige;
	unsigned int perks[2];
	int diveState;
	int voiceConnectivityBits;
	unsigned int playerCardIcon;
	unsigned int playerCardTitle;
	unsigned int playerCardNameplate;
};


enum PlayerCardClientLookupType
{
	PLAYERCARD_LOOKUP_SCRIPTSLOT = 0x0,
	PLAYERCARD_LOOKUP_LIVEPROFILE_CLIENT = 0x1,
	PLAYERCARD_LOOKUP_LIVEPROFILE_CONTROLLER = 0x2,
	PLAYERCARD_LOOKUP_LOBBY = 0x3,
	PLAYERCARD_LOOKUP_MYTEAM = 0x4,
	PLAYERCARD_LOOKUP_ENEMYTEAM = 0x5,
	PLAYERCARD_LOOKUP_COUNT = 0x6,
};

struct PlayerCardData
{
	unsigned int lastUpdateTime;
	unsigned int titleIndex;
	unsigned int iconIndex;
	unsigned int nameplateIndex;
	int rank;
	int prestige;
	team_t team;
	char name[32];
	char clanAbbrev[5];
};


enum usercmdButtonBits
{
	CMD_BUTTON_ATTACK = 1 << 0,
	CMD_BUTTON_SPRINT = 1 << 1,
	CMD_BUTTON_MELEE = 1 << 2,
	CMD_BUTTON_ACTIVATE = 1 << 3,
	CMD_BUTTON_RELOAD = 1 << 4,
	CMD_BUTTON_USE_RELOAD = 1 << 5,
	CMD_BUTTON_LEAN_LEFT = 1 << 6,
	CMD_BUTTON_LEAN_RIGHT = 1 << 7,
	CMD_BUTTON_PRONE = 1 << 8,
	CMD_BUTTON_CROUCH = 1 << 9,
	CMD_BUTTON_UP = 1 << 10,
	CMD_BUTTON_ADS = 1 << 11,
	CMD_BUTTON_DOWN = 1 << 12,
	CMD_BUTTON_BREATH = 1 << 13,
	CMD_BUTTON_FRAG = 1 << 14,
	CMD_BUTTON_OFFHAND_SECONDARY = 1 << 15,
	CMD_BUTTON_THROW = 1 << 19,
	CMD_BUTTON_REMOTE = 1 << 20,
};

struct usercmd_s
{
	int serverTime;
	int buttons;
	int angles[3];
	unsigned __int16 weapon;
	unsigned __int16 primaryWeaponForAltMode;
	unsigned __int16 offHandIndex;
	char forwardmove;
	char rightmove;
	float meleeChargeYaw;
	char meleeChargeDist;
	char selectedLoc[2];
	char selectedLocAngle;
	char remoteControlAngles[2];
};


enum trType_t
{
	TR_STATIONARY = 0x0,
	TR_INTERPOLATE = 0x1,
	TR_LINEAR = 0x2,
	TR_LINEAR_STOP = 0x3,
	TR_SINE = 0x4,
	TR_GRAVITY = 0x5,
	TR_LOW_GRAVITY = 0x6,
	TR_ACCELERATE = 0x7,
	TR_DECELERATE = 0x8,
	TR_PHYSICS = 0x9,
	TR_FIRST_RAGDOLL = 0xA,
	TR_RAGDOLL = 0xA,
	TR_RAGDOLL_GRAVITY = 0xB,
	TR_RAGDOLL_INTERPOLATE = 0xC,
	TR_LAST_RAGDOLL = 0xC,
	NUM_TRTYPES = 0xD,
};

struct trajectory_t
{
	trType_t trType;
	int trTime;
	int trDuration;
	float trBase[3];
	float trDelta[3];
};

struct LerpEntityStateTurret
{
	float gunAngles[3];
	int lastBarrelRotChangeTime;
	int lastBarrelRotChangeRate;
	int lastHeatChangeLevel;
	int lastHeatChangeTime;
	bool isBarrelRotating;
	bool isOverheat;
	bool isHeatingUp;
	bool isBeingCarried;
};

union LerpEntityStateTypeUnion
{
	LerpEntityStateTurret turret;
	char pad0[0x24];
};


struct LerpEntityState
{
	int eFlags;
	trajectory_t pos;
	trajectory_t apos;
	LerpEntityStateTypeUnion u;
};


struct clientLinkInfo_t
{
	__int16 parentId;
	char tagName;
	char flags;
};

struct entityState_s
{
	int number;
	int eType;
	LerpEntityState lerp;
	int time2;
	int otherEntityNum;
	int attackerEntityNum;
	int groundEntityNum;
	int loopSound;
	int surfType;
	union
	{
		int brushModel;
		int triggerModel;
		int item;
		int xmodel;
		int primaryLight;
	} index;
	int clientNum;
	int iHeadIcon;
	int iHeadIconTeam;
	int solid;
	unsigned int eventParm;
	int eventSequence;
	int events[4];
	unsigned int eventParms[4];
	unsigned __int16 weapon;
	int legsAnim;
	int torsoAnim;
	union
	{
		int eventParm2;
		int hintString;
		int fxId;
		int helicopterStage;
	} un1;
	union
	{
		int hintType;
		struct
		{
			unsigned __int16 vehicleXModel;
			char weaponModel;
		} __s1;
		int actorFlags;
	} un2;
	clientLinkInfo_t clientLinkInfo;
	unsigned int partBits[6];
	int clientMask[1];
};

struct clientActive_t
{
	bool usingAds;
	int timeoutcount;
	clSnapshot_t snap;
	bool alwaysFalse;
	int serverTime;
	int oldServerTime;
	int oldFrameServerTime;
	int serverTimeDelta;
	int oldSnapServerTime;
	int extrapolatedSnapshot;
	int newSnapshots;
	int serverId;
	char mapname[MAX_QPATH];
	int parseEntitiesIndex;
	int parseClientsIndex;
	int mouseDx[2];
	int mouseDy[2];
	int mouseIndex;
	bool stanceHeld;
	StanceState stance;
	StanceState stancePosition;
	int stanceTime;
	int cgameUserCmdWeapon;
	int cgameUserCmdOffHandIndex;
	float cgameFOVSensitivityScale;
	float cgameMaxPitchSpeed;
	float cgameMaxYawSpeed;
	float cgameKickAngles[3];
	float cgameOrigin[3];
	float cgameVelocity[3];
	float cgameViewangles[3];
	int cgameBobCycle;
	int cgameMovementDir;
	int cgameExtraButtons;
	int cgamePredictedDataServerTime;
	float clViewangles[3];
	usercmd_s cmds[128];
	int cmdNumber;
	ClientArchiveData clientArchive[256];
	int clientArchiveIndex;
	int packetBackupCount;
	int packetBackupMask;
	int parseEntitiesCount;
	int parseClientsCount;
	outPacket_t outPackets[32];
	clSnapshot_t snapshots[32];
	entityState_s parseEntities[19200];
	clientState_s parseClients[576];
	int corruptedTranslationFile;
	char translationVersion[256];
};

struct MaterialTechnique
{
	const char* name;
	unsigned __int16 flags;
	unsigned __int16 passCount;
	MaterialPass passArray[1];
};

struct MaterialTechniqueSet
{
	const char* name;
	char worldVertFormat;
	bool hasBeenUploaded;
	char unused[1];
	MaterialTechniqueSet* remappedTechniqueSet;
	MaterialTechnique* techniques[48];
};

struct GfxImageLoadDefIW3
{
	char levelCount;
	char flags;
	__int16 dimensions[3];
	int format;
	int resourceSize;
	char data[1];
};

struct __declspec(align(4)) GfxImageLoadDef
{
	char levelCount;
	char pad[3];
	int flags;
	int format;
	int resourceSize;
	unsigned char data[1];
};

union GfxTexture
{
	IDirect3DBaseTexture9* basemap;
	IDirect3DTexture9* map;
	IDirect3DVolumeTexture9* volmap;
	IDirect3DCubeTexture9* cubemap;
	GfxImageLoadDef* loadDef;
};

struct Picmip
{
	char platform[2];
};

struct CardMemory
{
	int platform[2];
};

struct GfxImage
{
	GfxTexture texture;
	char mapType;
	TextureSemantic semantic;
	char category;
	bool useSrgbReads;
	Picmip picmip;
	bool noPicmip;
	char track;
	CardMemory cardMemory;
	unsigned __int16 width;
	unsigned __int16 height;
	unsigned __int16 depth;
	bool delayLoadPixels;
	const char* name;
};

struct WaterWritable
{
	float floatTime;
};

struct complex_s
{
	float real;
	float imag;
};

struct water_t
{
	WaterWritable writable;
	complex_s* H0;
	float* wTerm;
	int M;
	int N;
	float Lx;
	float Lz;
	float gravity;
	float windvel;
	float winddir[2];
	float amplitude;
	float codeConstant[4];
	GfxImage* image;
};

union MaterialTextureDefInfo
{
	GfxImage* image;
	water_t* water;
};

struct MaterialTextureDef
{
	unsigned int nameHash;
	char nameStart;
	char nameEnd;
	char samplerState;
	TextureSemantic semantic;
	MaterialTextureDefInfo u;
};

struct MaterialConstantDef
{
	unsigned int nameHash;
	char name[12];
	float literal[4];
};

enum GfxSurfaceStatebitOp0 : unsigned int
{
	GFXS0_SRCBLEND_RGB_SHIFT = 0x0,
	GFXS0_SRCBLEND_RGB_MASK = 0xF,
	GFXS0_DSTBLEND_RGB_SHIFT = 0x4,
	GFXS0_DSTBLEND_RGB_MASK = 0xF0,
	GFXS0_BLENDOP_RGB_SHIFT = 0x8,
	GFXS0_BLENDOP_RGB_MASK = 0x700,
	GFXS0_BLEND_RGB_MASK = 0x7FF,
	GFXS0_ATEST_DISABLE = 0x800,
	GFXS0_ATEST_GT_0 = 0x1000,
	GFXS0_ATEST_LT_128 = 0x2000,
	GFXS0_ATEST_GE_128 = 0x3000,
	GFXS0_ATEST_MASK = 0x3000,
	GFXS0_CULL_SHIFT = 0xE,
	GFXS0_CULL_NONE = 0x4000,
	GFXS0_CULL_BACK = 0x8000,
	GFXS0_CULL_FRONT = 0xC000,
	GFXS0_CULL_MASK = 0xC000,
	GFXS0_SRCBLEND_ALPHA_SHIFT = 0x10,
	GFXS0_SRCBLEND_ALPHA_MASK = 0xF0000,
	GFXS0_DSTBLEND_ALPHA_SHIFT = 0x14,
	GFXS0_DSTBLEND_ALPHA_MASK = 0xF00000,
	GFXS0_BLENDOP_ALPHA_SHIFT = 0x18,
	GFXS0_BLENDOP_ALPHA_MASK = 0x7000000,
	GFXS0_BLEND_ALPHA_MASK = 0x7FF0000,
	GFXS0_COLORWRITE_RGB = 0x8000000,
	GFXS0_COLORWRITE_ALPHA = 0x10000000,
	GFXS0_COLORWRITE_MASK = 0x18000000,
	GFXS0_GAMMAWRITE = 0x40000000,
	GFXS0_POLYMODE_LINE = 0x80000000
};

enum GfxSurfaceStatebitOp1 : unsigned int
{
	GFXS1_DEPTHWRITE = 0x1,
	GFXS1_DEPTHTEST_DISABLE = 0x2,
	GFXS1_DEPTHTEST_SHIFT = 0x2,
	GFXS1_DEPTHTEST_ALWAYS = 0x0,
	GFXS1_DEPTHTEST_LESS = 0x4,
	GFXS1_DEPTHTEST_EQUAL = 0x8,
	GFXS1_DEPTHTEST_LESSEQUAL = 0xC,
	GFXS1_DEPTHTEST_MASK = 0xC,
	GFXS1_POLYGON_OFFSET_SHIFT = 0x4,
	GFXS1_POLYGON_OFFSET_0 = 0x0,
	GFXS1_POLYGON_OFFSET_1 = 0x10,
	GFXS1_POLYGON_OFFSET_2 = 0x20,
	GFXS1_POLYGON_OFFSET_SHADOWMAP = 0x30,
	GFXS1_POLYGON_OFFSET_MASK = 0x30,
	GFXS1_STENCIL_FRONT_ENABLE = 0x40,
	GFXS1_STENCIL_BACK_ENABLE = 0x80,
	GFXS1_STENCIL_MASK = 0xC0,
	GFXS1_STENCIL_FRONT_PASS_SHIFT = 0x8,
	GFXS1_STENCIL_FRONT_FAIL_SHIFT = 0xB,
	GFXS1_STENCIL_FRONT_ZFAIL_SHIFT = 0xE,
	GFXS1_STENCIL_FRONT_FUNC_SHIFT = 0x11,
	GFXS1_STENCIL_FRONT_MASK = 0xFFF00,
	GFXS1_STENCIL_BACK_PASS_SHIFT = 0x14,
	GFXS1_STENCIL_BACK_FAIL_SHIFT = 0x17,
	GFXS1_STENCIL_BACK_ZFAIL_SHIFT = 0x1A,
	GFXS1_STENCIL_BACK_FUNC_SHIFT = 0x1D,
	GFXS1_STENCIL_BACK_MASK = 0xFFF00000,
	GFXS1_STENCILFUNC_FRONTBACK_MASK = 0xE00E0000,
	GFXS1_STENCILOP_FRONTBACK_MASK = 0x1FF1FF00,
};

enum GfxStencilOp
{
	GFXS_STENCILOP_KEEP = 0x0,
	GFXS_STENCILOP_ZERO = 0x1,
	GFXS_STENCILOP_REPLACE = 0x2,
	GFXS_STENCILOP_INCRSAT = 0x3,
	GFXS_STENCILOP_DECRSAT = 0x4,
	GFXS_STENCILOP_INVERT = 0x5,
	GFXS_STENCILOP_INCR = 0x6,
	GFXS_STENCILOP_DECR = 0x7,

	GFXS_STENCILOP_COUNT,
	GFXS_STENCILOP_MASK = 0x7
};

struct GfxStateBits
{
	unsigned int loadBits[2];
};

struct Material
{
	MaterialInfo info;
	char stateBitsEntry[48];
	unsigned char textureCount;
	unsigned char constantCount;
	unsigned char stateBitsCount;
	char stateFlags;
	char cameraRegion;
	MaterialTechniqueSet* techniqueSet;
	MaterialTextureDef* textureTable;
	MaterialConstantDef* constantTable;
	GfxStateBits* stateBitsTable;
};

struct XModelLodInfo
{
	float dist;
	unsigned __int16 numsurfs;
	unsigned __int16 surfIndex;
	XModelSurfs* modelSurfs;
	int partBits[6];
	XSurface* surfs;
	char lod;
	char smcBaseIndexPlusOne;
	char smcSubIndexMask;
	char smcBucket;
};

struct XModelCollTri_s
{
	float plane[4];
	float svec[4];
	float tvec[4];
};

struct XModelCollSurf_s
{
	XModelCollTri_s* collTris;
	int numCollTris;
	Bounds bounds;
	int boneIdx;
	int contents;
	int surfFlags;
};

struct XBoneInfo
{
	Bounds bounds;
	float radiusSquared;
};

struct XModel
{
	const char* name;
	unsigned char numBones;
	unsigned char numRootBones;
	unsigned char numsurfs;
	unsigned char lodRampType;
	float scale;
	unsigned int noScalePartBits[6];
	unsigned __int16* boneNames;
	unsigned char* parentList;
	short* quats;
	float* trans;
	unsigned char* partClassification;
	DObjAnimMat* baseMat;
	Material** materialHandles;
	XModelLodInfo lodInfo[4];
	unsigned char maxLoadedLod;
	unsigned char numLods;
	unsigned char collLod;
	unsigned char flags;
	XModelCollSurf_s* collSurfs;
	int numCollSurfs;
	int contents;
	XBoneInfo* boneInfo;
	float radius;
	Bounds bounds;
	int memUsage;
	bool bad;
	PhysPreset* physPreset;
	PhysCollmap* physCollmap;
};

struct _AILSOUNDINFO
{
	int format;
	const void* data_ptr;
	unsigned int data_len;
	unsigned int rate;
	int bits;
	int channels;
	unsigned int samples;
	unsigned int block_size;
	const void* initial_ptr;
};

struct MssSound
{
	_AILSOUNDINFO info;
	char* data;
};

struct LoadedSound
{
	const char* name;
	MssSound sound;
};

struct StreamFileNameRaw
{
	const char* dir;
	const char* name;
};

union StreamFileInfo
{
	StreamFileNameRaw raw;
};

struct StreamFileName
{
	StreamFileInfo info;
};

struct StreamedSound
{
	StreamFileName filename;
};

union SoundFileRef
{
	LoadedSound* loadSnd;
	StreamedSound streamSnd;
};

enum snd_alias_type_t
{
	SAT_UNKNOWN = 0x0,
	SAT_LOADED = 0x1,
	SAT_STREAMED = 0x2,
	SAT_VOICED = 0x3,
	SAT_COUNT = 0x4,
};

struct snd_volume_info_t
{
	float volume;
	float goalvolume;
	float goalrate;
};

struct snd_channelvolgroup
{
	snd_volume_info_t channelvol[64];
	bool active;
};

struct snd_background_info_t
{
	float goalvolume;
	float goalrate;
};

struct snd_enveffect
{
	int roomtype;
	float drylevel;
	float drygoal;
	float dryrate;
	float wetlevel;
	float wetgoal;
	float wetrate;
	bool active;
};

struct orientation_t
{
	float origin[3];
	float axis[3][3];
};

struct snd_listener
{
	orientation_t orient;
	float velocity;
	int clientNum;
	bool active;
};

struct snd_amplifier
{
	snd_listener* listener;
	int minRadius;
	int maxRadius;
	float falloffExp;
	float minVol;
	float maxVol;
};

struct snd_entchannel_info_t
{
	char name[64];
	int priority;
	bool is3d;
	bool isRestricted;
	bool isPausable;
	int maxVoices;
	int voiceCount;
};

struct snd_entchan_overrides_t
{
	unsigned int isPausable[2];
	float timescaleLerp[64];
};

enum SndFileLoadingState
{
	SFLS_UNLOADED = 0x0,
	SFLS_LOADING = 0x1,
	SFLS_LOADED = 0x2,
};

struct SndFileSpecificChannelInfo
{
	SndFileLoadingState loadingState;
	int srcChannelCount;
	int baserate;
};

union SndEntHandle
{
	struct
	{
		unsigned int entIndex;
	} field;
	int handle;
};

enum SndLengthId
{
	SndLengthNotify_Subtitle = 0x0,
	SndLengthNotify_EntityCustom = 0x1,
	SndLengthNotifyCount = 0x2,
};

struct sndLengthNotifyInfo
{
	SndLengthId id[4];
	void* data[4];
	int count;
};

enum snd_alias_system_t
{
	SASYS_UI = 0x0,
	SASYS_CGAME = 0x1,
	SASYS_GAME = 0x2,
	SASYS_COUNT = 0x3,
};

struct SoundFile
{
	char type;
	char exists;
	SoundFileRef u;
};

struct MSSSpeakerLevels
{
	int speaker;
	int numLevels;
	float levels[2];
};

struct MSSChannelMap
{
	unsigned int speakerCount;
	MSSSpeakerLevels speakers[6];
};

struct SpeakerMap
{
	bool isDefault;
	const char* name;
	MSSChannelMap channelMaps[2][2];
};

union SoundAliasFlags
{
#pragma warning(push)
#pragma warning(disable: 4201)
	struct
	{
		unsigned int looping : 1;		// & 1	/ 0x1			/ 0000 0000 0000 0001
		unsigned int isMaster : 1;		// & 2	/ 0x2			/ 0000 0000 0000 0010
		unsigned int isSlave : 1;		// & 4	/ 0x4			/ 0000 0000 0000 0100
		unsigned int fullDryLevel : 1;	//	& 8	/ 0x8			/ 0000 0000 0000 1000
		unsigned int noWetLevel : 1;	// & 16	/ 0x10			/ 0000 0000 0001 0000
		unsigned int unknown : 1;		// & 32	/ 0x20			/ 0000 0000 0010 0000
		unsigned int unk_is3D : 1;		// & 64	/ 0x40			/ 0000 0000 0100 0000		// CONFIRMED IW4 IW5
		unsigned int type : 2;			// & 384	/ 0x180		/ 0000 0001 1000 0000		// CONFIRMED IW4 IW5
		unsigned int channel : 6;		// & 32256	/ 0x7E00	/ 0111 1110 0000 0000		// CONFIRMED IW4 IW5
	};
#pragma warning(pop)
	unsigned int intValue;
};

struct SndCurve
{
	const char* filename;
	unsigned __int16 knotCount;
	float knots[16][2];
};

struct snd_alias_t
{
	const char* aliasName;
	const char* subtitle;
	const char* secondaryAliasName;
	const char* chainAliasName;
	const char* mixerGroup;
	SoundFile* soundFile;
	int sequence;
	float volMin;
	float volMax;
	float pitchMin;
	float pitchMax;
	float distMin;
	float distMax;
	float velocityMin;
	SoundAliasFlags flags;
	union
	{
		float slavePercentage;
		float masterPercentage;
	} ___u15;
	float probability;
	float lfePercentage;
	float centerPercentage;
	int startDelay;
	SndCurve* volumeFalloffCurve;
	float envelopMin;
	float envelopMax;
	float envelopPercentage;
	SpeakerMap* speakerMap;
};

struct snd_channel_info_t
{
	SndFileSpecificChannelInfo soundFileInfo;
	SndEntHandle sndEnt;
	int entchannel;
	int startDelay;
	int looptime;
	int totalMsec;
	int playbackId;
	sndLengthNotifyInfo lengthNotifyInfo;
	float basevolume;
	float pitch;
	snd_alias_t* alias0;
	snd_alias_t* alias1;
	int saveIndex0;
	int saveIndex1;
	float lerp;
	float org[3];
	float offset[3];
	bool paused;
	bool master;
	float timescaleLerp;
	snd_alias_system_t system;
};

struct snd_local_t
{
	bool Initialized2d;
	bool Initialized3d;
	bool paused;
	int playbackIdCounter;
	unsigned int playback_rate;
	int playback_channels;
	float timescale;
	int pausetime;
	int cpu;
	struct
	{
		char buffer[16384];
		volatile int size;
		bool compress;
	} restore;
	float volume;
	snd_volume_info_t mastervol;
	snd_channelvolgroup channelVolGroups[4];
	snd_channelvolgroup* channelvol;
	snd_background_info_t background[4];
	int ambient_track;
	float slaveLerp;
	float masterPercentage;
	snd_enveffect envEffects[5];
	snd_enveffect* effect;
	snd_listener listeners[2];
	int time;
	int looptime;
	snd_amplifier amplifier;
	snd_entchannel_info_t entchaninfo[64];
	snd_entchan_overrides_t entchanOverrides;
	int entchannel_count;
	snd_channel_info_t chaninfo[52];
	int max_2D_channels;
	int max_3D_channels;
	int max_stream_channels;
};

struct Poly
{
	float(*pts)[3];
	unsigned int ptCount;
};

enum SunShadowPartition
{
	R_SUNSHADOW_NEAR = 0x0,
	R_SUNSHADOW_FAR = 0x1,
	R_SUNSHADOW_PARTITION_COUNT = 0x2,
};

struct snd_alias_list_t
{
	const char* aliasName;
	snd_alias_t* head;
	unsigned int count;
};

struct cStaticModel_s
{
	XModel* xmodel;
	float origin[3];
	float invScaledAxis[3][3];
	Bounds absBounds;
};

struct ClipMaterial
{
	const char* name;
	int surfaceFlags;
	int contents;
};

struct cNode_t
{
	cplane_s* plane;
	__int16 children[2];
};

struct cLeaf_t
{
	unsigned __int16 firstCollAabbIndex;
	unsigned __int16 collAabbCount;
	int brushContents;
	int terrainContents;
	Bounds bounds;
	int leafBrushNode;
};

struct cLeafBrushNodeLeaf_t
{
	unsigned __int16* brushes;
};

struct cLeafBrushNodeChildren_t
{
	float dist;
	float range;
	unsigned __int16 childOffset[2];
};

union cLeafBrushNodeData_t
{
	cLeafBrushNodeLeaf_t leaf;
	cLeafBrushNodeChildren_t children;
};

struct cLeafBrushNode_s
{
	unsigned char axis;
	__int16 leafBrushCount;
	int contents;
	cLeafBrushNodeData_t data;
};

struct CollisionBorder
{
	float distEq[3];
	float zBase;
	float zSlope;
	float start;
	float length;
};

struct CollisionPartition
{
	unsigned char triCount;
	unsigned char borderCount;
	unsigned char firstVertSegment;
	int firstTri;
	CollisionBorder* borders;
};

union CollisionAabbTreeIndex
{
	int firstChildIndex;
	int partitionIndex;
};

struct CollisionAabbTree
{
	float midPoint[3];
	unsigned __int16 materialIndex;
	unsigned __int16 childCount;
	float halfSize[3];
	CollisionAabbTreeIndex u;
};

struct cmodel_t
{
	Bounds bounds;
	float radius;
	cLeaf_t leaf;
};


struct TriggerModel
{
	int contents;
	unsigned __int16 hullCount;
	unsigned __int16 firstHull;
};

struct TriggerHull
{
	Bounds bounds;
	int contents;
	unsigned __int16 slabCount;
	unsigned __int16 firstSlab;
};

struct TriggerSlab
{
	float dir[3];
	float midPoint;
	float halfSize;
};

struct MapTriggers
{
	unsigned int count;
	TriggerModel* models;
	unsigned int hullCount;
	TriggerHull* hulls;
	unsigned int slabCount;
	TriggerSlab* slabs;
};

struct Stage
{
	const char* name;
	float origin[3];
	unsigned __int16 triggerIndex;
	char sunPrimaryLightIndex;
};

struct __declspec(align(4)) MapEnts
{
	const char* name;
	const char* entityString;
	int numEntityChars;
	MapTriggers trigger;
	Stage* stages;
	char stageCount;
};

struct SModelAabbNode
{
	Bounds bounds;
	unsigned __int16 firstChild;
	unsigned __int16 childCount;
};

enum DynEntityType
{
	DYNENT_TYPE_INVALID = 0x0,
	DYNENT_TYPE_CLUTTER = 0x1,
	DYNENT_TYPE_DESTRUCT = 0x2,
	DYNENT_TYPE_COUNT = 0x3,
};

struct GfxPlacement
{
	float quat[4];
	float origin[3];
};

struct FxSpawnDefLooping
{
	int intervalMsec;
	int count;
};

struct FxIntRange
{
	int base;
	int amplitude;
};

struct FxSpawnDefOneShot
{
	FxIntRange count;
};

union FxSpawnDef
{
	FxSpawnDefLooping looping;
	FxSpawnDefOneShot oneShot;
};

struct FxFloatRange
{
	float base;
	float amplitude;
};

struct FxElemAtlas
{
	char behavior;
	char index;
	char fps;
	char loopCount;
	char colIndexBits;
	char rowIndexBits;
	__int16 entryCount;
};

struct FxElemVec3Range
{
	float base[3];
	float amplitude[3];
};

struct FxElemVelStateInFrame
{
	FxElemVec3Range velocity;
	FxElemVec3Range totalDelta;
};

const struct FxElemVelStateSample
{
	FxElemVelStateInFrame local;
	FxElemVelStateInFrame world;
};

struct FxElemVisualState
{
	unsigned char color[4];
	float rotationDelta;
	float rotationTotal;
	float size[2];
	float scale;
};

const struct FxElemVisStateSample
{
	FxElemVisualState base;
	FxElemVisualState amplitude;
};

struct FxElemMarkVisuals
{
	Material* materials[2];
};

union FxEffectDefRef
{
	FxEffectDef* handle;
	const char* name;
};

union FxElemVisuals
{
	const void* anonymous;
	Material* material;
	XModel* model;
	FxEffectDefRef effectDef;
	const char* soundName;
};

union FxElemDefVisuals
{
	FxElemMarkVisuals* markArray;
	FxElemVisuals* array;
	FxElemVisuals instance;
};

struct FxTrailVertex
{
	float pos[2];
	float normal[2];
	float texCoord;
};

struct FxTrailDef
{
	int scrollTimeMsec;
	int repeatDist;
	float invSplitDist;
	float invSplitArcDist;
	float invSplitTime;
	int vertCount;
	FxTrailVertex* verts;
	int indCount;
	unsigned __int16* inds;
};

struct FxSparkFountainDef
{
	float gravity;
	float bounceFrac;
	float bounceRand;
	float sparkSpacing;
	float sparkLength;
	int sparkCount;
	float loopTime;
	float velMin;
	float velMax;
	float velConeFrac;
	float restSpeed;
	float boostTime;
	float boostFactor;
};

union FxElemExtendedDefPtr
{
	FxTrailDef* trailDef;
	FxSparkFountainDef* sparkFountainDef;
	void* unknownDef;
};

const struct FxElemDef
{
	int flags;
	FxSpawnDef spawn;
	FxFloatRange spawnRange;
	FxFloatRange fadeInRange;
	FxFloatRange fadeOutRange;
	float spawnFrustumCullRadius;
	FxIntRange spawnDelayMsec;
	FxIntRange lifeSpanMsec;
	FxFloatRange spawnOrigin[3];
	FxFloatRange spawnOffsetRadius;
	FxFloatRange spawnOffsetHeight;
	FxFloatRange spawnAngles[3];
	FxFloatRange angularVelocity[3];
	FxFloatRange initialRotation;
	FxFloatRange gravity;
	FxFloatRange reflectionFactor;
	FxElemAtlas atlas;
	char elemType;
	char visualCount;
	char velIntervalCount;
	char visStateIntervalCount;
	FxElemVelStateSample* velSamples;
	FxElemVisStateSample* visSamples;
	FxElemDefVisuals visuals;
	Bounds collBounds;
	FxEffectDefRef effectOnImpact;
	FxEffectDefRef effectOnDeath;
	FxEffectDefRef effectEmitted;
	FxFloatRange emitDist;
	FxFloatRange emitDistVariance;
	FxElemExtendedDefPtr extended;
	char sortOrder;
	char lightingFrac;
	char useItemClip;
	char fadeInfo;
};

struct FxEffectDef
{
	const char* name;
	int flags;
	int totalSize;
	int msecLoopingLife;
	int elemDefCountLooping;
	int elemDefCountOneShot;
	int elemDefCountEmission;
	FxElemDef* elemDefs;
};

struct DynEntityDef
{
	DynEntityType type;
	GfxPlacement pose;
	XModel* xModel;
	unsigned __int16 brushModel;
	unsigned __int16 physicsBrushModel;
	FxEffectDef* destroyFx;
	PhysPreset* physPreset;
	int health;
	PhysMass mass;
	int contents;
};

struct DynEntityPose
{
	GfxPlacement pose;
	float radius;
};

struct DynEntityClient
{
	__int64 physObjId;  // x64: pointer-sized (Load_clipMap_t streams 16-byte elements; RUNTIME, zero-filled)
	unsigned __int16 flags;
	unsigned __int16 lightingHandle;
	int health;
};

struct DynEntityColl
{
	unsigned __int16 sector;
	unsigned __int16 nextEntInSector;
	float linkMins[2];
	float linkMaxs[2];
};

#pragma warning(push)
#pragma warning(disable: 4324)
struct __declspec(align(64)) clipMap_t
{
	const char* name;
	int isInUse;
	unsigned int planeCount;
	cplane_s* planes;
	unsigned int numStaticModels;
	cStaticModel_s* staticModelList;
	unsigned int numMaterials;
	ClipMaterial* materials;
	unsigned int numBrushSides;
	cbrushside_t* brushsides;
	unsigned int numBrushEdges;
	unsigned char* brushEdges;
	unsigned int numNodes;
	cNode_t* nodes;
	unsigned int numLeafs;
	cLeaf_t* leafs;
	unsigned int leafbrushNodesCount;
	cLeafBrushNode_s* leafbrushNodes;
	unsigned int numLeafBrushes;
	unsigned __int16* leafbrushes;
	unsigned int numLeafSurfaces;
	unsigned int* leafsurfaces;
	unsigned int vertCount;
	vec3_t* verts;
	unsigned int triCount;
	unsigned __int16* triIndices;
	unsigned char* triEdgeIsWalkable;
	unsigned int borderCount;
	CollisionBorder* borders;
	unsigned int partitionCount;
	CollisionPartition* partitions;
	unsigned int aabbTreeCount;
	CollisionAabbTree* aabbTrees;
	unsigned int numSubModels;
	cmodel_t* cmodels;
	unsigned __int16 numBrushes;
	cbrush_t* brushes;
	Bounds* brushBounds;
	int* brushContents;
	MapEnts* mapEnts;
	unsigned __int16 smodelNodeCount;
	SModelAabbNode* smodelNodes;
	unsigned __int16 dynEntCount[2];
	DynEntityDef* dynEntDefList[2];
	DynEntityPose* dynEntPoseList[2];
	DynEntityClient* dynEntClientList[2];
	DynEntityColl* dynEntCollList[2];
	unsigned int checksum;
	// The x64 loader reads 0x200 bytes (0x1c0 without this). Zero in stock boneyard.
	unsigned char unknown5[0x30];
};
#pragma warning(pop)

struct ComPrimaryLight
{
	char type;
	char canUseShadowMap;
	char exponent;
	char unused;
	float color[3];
	float dir[3];
	float origin[3];
	float radius;
	float cosHalfFovOuter;
	float cosHalfFovInner;
	float cosHalfFovExpanded;
	float rotationLimit;
	float translationLimit;
	const char* defName;
};

struct ComWorld
{
	const char* name;
	int isInUse;
	unsigned int primaryLightCount;
	ComPrimaryLight* primaryLights;
};

enum nodeType
{
	NODE_ERROR = 0x0,
	NODE_PATHNODE = 0x1,
	NODE_COVER_STAND = 0x2,
	NODE_COVER_CROUCH = 0x3,
	NODE_COVER_CROUCH_WINDOW = 0x4,
	NODE_COVER_PRONE = 0x5,
	NODE_COVER_RIGHT = 0x6,
	NODE_COVER_LEFT = 0x7,
	NODE_AMBUSH = 0x8,
	NODE_EXPOSED = 0x9,
	NODE_CONCEALMENT_STAND = 0xA,
	NODE_CONCEALMENT_CROUCH = 0xB,
	NODE_CONCEALMENT_PRONE = 0xC,
	NODE_DOOR = 0xD,
	NODE_DOOR_INTERIOR = 0xE,
	NODE_SCRIPTED = 0xF,
	NODE_NEGOTIATION_BEGIN = 0x10,
	NODE_NEGOTIATION_END = 0x11,
	NODE_TURRET = 0x12,
	NODE_GUARD = 0x13,
	NODE_NUMTYPES = 0x14,
	NODE_DONTLINK = 0x14,
};

enum PathNodeErrorCode
{
	PNERR_NONE = 0x0,
	PNERR_INSOLID = 0x1,
	PNERR_FLOATING = 0x2,
	PNERR_NOLINK = 0x3,
	PNERR_DUPLICATE = 0x4,
	PNERR_NOSTANCE = 0x5,
	PNERR_INVALIDDOOR = 0x6,
	PNERR_NOANGLES = 0x7,
	PNERR_BADPLACEMENT = 0x8,
	NUM_PATH_NODE_ERRORS = 0x9,
};

union $23305223CFD097B6F79557BDD2047E6C
{
	float minUseDistSq;
	PathNodeErrorCode error;
};

struct pathlink_s
{
	float fDist;
	unsigned __int16 nodeNum;
	char disconnectCount;
	char negotiationLink;
	char flags;
	char ubBadPlaceCount[3];
};

struct pathnode_constant_t
{
	nodeType type;
	unsigned __int16 spawnflags;
	unsigned __int16 targetname;
	unsigned __int16 script_linkName;
	unsigned __int16 script_noteworthy;
	unsigned __int16 target;
	unsigned __int16 animscript;
	int animscriptfunc;
	float vOrigin[3];
	float fAngle;
	float forward[2];
	float fRadius;
	$23305223CFD097B6F79557BDD2047E6C ___u12;
	__int16 wOverlapNode[2];
	unsigned __int16 totalLinkCount;
	pathlink_s* Links;
};

struct pathnode_dynamic_t
{
	void* pOwner;
	int iFreeTime;
	int iValidTime[3];
	int dangerousNodeTime[3];
	int inPlayerLOSTime;
	__int16 wLinkCount;
	__int16 wOverlapCount;
	__int16 turretEntNumber;
	char userCount;
	bool hasBadPlaceLink;
};

union $73F238679C0419BE2C31C6559E8604FC
{
	float nodeCost;
	int linkIndex;
};

struct pathnode_transient_t
{
	int iSearchFrame;
	pathnode_t* pNextOpen;
	pathnode_t* pPrevOpen;
	pathnode_t* pParent;
	float fCost;
	float fHeuristic;
	$73F238679C0419BE2C31C6559E8604FC ___u6;
};

struct pathnode_t
{
	pathnode_constant_t constant;
	pathnode_dynamic_t dynamic;
	pathnode_transient_t transient;
};

struct pathbasenode_t
{
	float vOrigin[3];
	unsigned int type;
};

struct pathnode_tree_nodes_t
{
	int nodeCount;
	unsigned __int16* nodes;
};

union pathnode_tree_info_t
{
	pathnode_tree_t* child[2];
	pathnode_tree_nodes_t s;
};

struct pathnode_tree_t
{
	int axis;
	float dist;
	pathnode_tree_info_t u;
};

struct PathData
{
	unsigned int nodeCount;
	pathnode_t* nodes;
	pathbasenode_t* basenodes;
	unsigned int chainNodeCount;
	unsigned __int16* chainNodeForNode;
	unsigned __int16* nodeForChainNode;
	int visBytes;
	char* pathVis;
	int nodeTreeCount;
	pathnode_tree_t* nodeTree;
};

struct VehicleTrackObstacle
{
	float origin[2];
	float radius;
};

struct VehicleTrackSector
{
	float startEdgeDir[2];
	float startEdgeDist;
	float leftEdgeDir[2];
	float leftEdgeDist;
	float rightEdgeDir[2];
	float rightEdgeDist;
	float sectorLength;
	float sectorWidth;
	float totalPriorLength;
	float totalFollowingLength;
	VehicleTrackObstacle* obstacles;
	unsigned int obstacleCount;
};

struct VehicleTrackSegment
{
	const char* targetName;
	VehicleTrackSector* sectors;
	unsigned int sectorCount;
	VehicleTrackSegment** nextBranches;
	unsigned int nextBranchesCount;
	VehicleTrackSegment** prevBranches;
	unsigned int prevBranchesCount;
	float endEdgeDir[2];
	float endEdgeDist;
	float totalLength;
};

struct VehicleTrack
{
	VehicleTrackSegment* segments;
	unsigned int segmentCount;
};

struct G_GlassPiece
{
	unsigned __int16 damageTaken;
	unsigned __int16 collapseTime;
	int lastStateChangeTime;
	char impactDir;
	char impactPos[2];
};

struct G_GlassName
{
	char* nameStr;
	unsigned __int16 name;
	unsigned __int16 pieceCount;
	unsigned __int16* pieceIndices;
};

struct G_GlassData
{
	G_GlassPiece* glassPieces;
	unsigned int pieceCount;
	unsigned __int16 damageToWeaken;
	unsigned __int16 damageToDestroy;
	unsigned int glassNameCount;
	G_GlassName* glassNames;
	char pad[108];
};

struct GameWorldSp
{
	const char* name;
	PathData path;
	VehicleTrack vehicleTrack;
	G_GlassData* g_glassData;
};

struct GameWorldMp
{
	const char* name;
	G_GlassData* g_glassData;
};

struct FxGlassDef
{
	float halfThickness;
	float texVecs[2][2];
	GfxColor color;
	Material* material;
	Material* materialShattered;
	PhysPreset* physPreset;
};

struct FxSpatialFrame
{
	float quat[4];
	float origin[3];
};

struct $E43DBA5037697D705289B74D87E76C70
{
	FxSpatialFrame frame;
	float radius;
};

union FxGlassPiecePlace
{
	$E43DBA5037697D705289B74D87E76C70 __s0;
	unsigned int nextFree;
};

struct FxGlassPieceState
{
	float texCoordOrigin[2];
	unsigned int supportMask;
	unsigned __int16 initIndex;
	unsigned __int16 geoDataStart;
	char defIndex;
	char pad[5];
	char vertCount;
	char holeDataCount;
	char crackDataCount;
	char fanDataCount;
	unsigned __int16 flags;
	float areaX2;
};

struct FxGlassPieceDynamics
{
	int fallTime;
	int physObjId;
	int physJointId;
	float vel[3];
	float avel[3];
};

struct FxGlassVertex
{
	__int16 x;
	__int16 y;
};

struct FxGlassHoleHeader
{
	unsigned __int16 uniqueVertCount;
	char touchVert;
	char pad[1];
};

struct FxGlassCrackHeader
{
	unsigned __int16 uniqueVertCount;
	char beginVertIndex;
	char endVertIndex;
};

union FxGlassGeometryData
{
	FxGlassVertex vert;
	FxGlassHoleHeader hole;
	FxGlassCrackHeader crack;
	char asBytes[4];
	__int16 anonymous[2];
};

struct FxGlassInitPieceState
{
	FxSpatialFrame frame;
	float radius;
	float texCoordOrigin[2];
	unsigned int supportMask;
	float areaX2;
	unsigned char defIndex;
	unsigned char vertCount;
	unsigned char fanDataCount;
	char pad[1];
};

struct FxGlassSystem
{
	int time;
	int prevTime;
	unsigned int defCount;
	unsigned int pieceLimit;
	unsigned int pieceWordCount;
	unsigned int initPieceCount;
	unsigned int cellCount;
	unsigned int activePieceCount;
	unsigned int firstFreePiece;
	unsigned int geoDataLimit;
	unsigned int geoDataCount;
	unsigned int initGeoDataCount;
	FxGlassDef* defs;
	FxGlassPiecePlace* piecePlaces;
	FxGlassPieceState* pieceStates;
	FxGlassPieceDynamics* pieceDynamics;
	FxGlassGeometryData* geoData;
	unsigned int* isInUse;
	unsigned int* cellBits;
	char* visData;
	float(*linkOrg)[3];
	float* halfThickness;
	unsigned __int16* lightingHandles;
	FxGlassInitPieceState* initPieceStates;
	FxGlassGeometryData* initGeoData;
	bool needToCompactData;
	char initCount;
	float effectChanceAccum;
	int lastPieceDeletionTime;
};

struct FxWorld
{
	const char* name;
	FxGlassSystem glassSys;
};

struct __declspec(align(4)) GfxSky
{
	int skySurfCount;
	int* skyStartSurfs;
	GfxImage* skyImage;
	char skySamplerState;
};

struct GfxWorldDpvsPlanes
{
	int cellCount;
	cplane_s* planes;
	unsigned __int16* nodes;
	unsigned int* sceneEntCellBits;
};

struct GfxCellTreeCount
{
	int aabbTreeCount;
};

struct GfxAabbTree
{
	Bounds bounds;
	unsigned __int16 childCount;
	unsigned __int16 surfaceCount;
	unsigned __int16 startSurfIndex;
	unsigned __int16 surfaceCountNoDecal;
	unsigned __int16 startSurfIndexNoDecal;
	unsigned __int16 smodelIndexCount;
	unsigned __int16* smodelIndexes;
	int childrenOffset;
};

struct GfxCellTree
{
	GfxAabbTree* aabbTree;
};

struct GfxPortalWritable
{
	bool isQueued;
	bool isAncestor;
	char recursionDepth;
	char hullPointCount;
	float(*hullPoints)[2];
	GfxPortal* queuedParent;
};

struct DpvsPlane
{
	float coeffs[4];
};

struct DpvsPlanes
{
	DpvsPlane* planes;
	int count;
};

struct DpvsClipPlaneSet
{
	DpvsPlane* planes[16];
	unsigned int count;
};

struct DpvsStaticCellCmd
{
	DpvsPlane* planes;
	GfxAabbTree* tree;
	char planeCount;
	char frustumPlaneCount;
	unsigned __int16 sceneViewType;
};

struct GfxPortal
{
	GfxPortalWritable writable;
	DpvsPlane plane;
	float(*vertices)[3];
	unsigned __int16 cellIndex;
	char vertexCount;
	float hullAxis[2][3];
};

struct GfxCell
{
	Bounds bounds;
	int portalCount;
	GfxPortal* portals;
	char reflectionProbeCount;
	char* reflectionProbes;
};

struct GfxReflectionProbe
{
	float origin[3];
};

struct GfxLightmapArray
{
	GfxImage* primary;
	GfxImage* secondary;
};

struct GfxWorldVertex
{
	float xyz[3];
	float binormalSign;
	GfxColor color;
	float texCoord[2];
	float lmapCoord[2];
	PackedUnitVec normal;
	PackedUnitVec tangent;
};

struct GfxWorldVertexData
{
	GfxWorldVertex* vertices;
	IDirect3DVertexBuffer9* worldVb;
};

struct GfxWorldVertexLayerData
{
	char* data;
	IDirect3DVertexBuffer9* layerVb;
};

struct GfxWorldDraw
{
	unsigned int reflectionProbeCount;
	GfxImage** reflectionProbes;
	GfxReflectionProbe* reflectionProbeOrigins;
	GfxTexture* reflectionProbeTextures;
	int lightmapCount;
	GfxLightmapArray* lightmaps;
	GfxTexture* lightmapPrimaryTextures;
	GfxTexture* lightmapSecondaryTextures;
	GfxImage* lightmapOverridePrimary;
	GfxImage* lightmapOverrideSecondary;
	unsigned int vertexCount;
	GfxWorldVertexData vd;
	unsigned int vertexLayerDataSize;
	GfxWorldVertexLayerData vld;
	unsigned int indexCount;
	unsigned __int16* indices;
};

struct GfxLightGridEntry
{
	unsigned __int16 colorsIndex;
	char primaryLightIndex;
	char needsTrace;
};

struct GfxLightGridColors
{
	unsigned char rgb[56][3];
};

struct GfxLightGrid
{
	bool hasLightRegions;
	unsigned int lastSunPrimaryLightIndex;
	unsigned __int16 mins[3];
	unsigned __int16 maxs[3];
	unsigned int rowAxis;
	unsigned int colAxis;
	unsigned __int16* rowDataStart;
	unsigned int rawRowDataSize;
	char* rawRowData;
	unsigned int entryCount;
	GfxLightGridEntry* entries;
	unsigned int colorCount;
	GfxLightGridColors* colors;
};

struct GfxSModelSurfVisDataHeader
{
	XSurface* surfs;
	unsigned __int16 smodelCount;
	unsigned __int16 smodelIndexes[1];
};

struct GfxBrushModelWritable
{
	Bounds bounds;
};

struct __declspec(align(4)) GfxBrushModel
{
	GfxBrushModelWritable writable;
	Bounds bounds;
	float radius;
	unsigned __int16 surfaceCount;
	unsigned __int16 startSurfIndex;
	unsigned __int16 surfaceCountNoDecal;
};

struct MaterialMemory
{
	Material* material;
	int memory;
};

struct sunflare_t
{
	bool hasValidData;
	Material* spriteMaterial;
	Material* flareMaterial;
	float spriteSize;
	float flareMinSize;
	float flareMinDot;
	float flareMaxSize;
	float flareMaxDot;
	float flareMaxAlpha;
	int flareFadeInTime;
	int flareFadeOutTime;
	float blindMinDot;
	float blindMaxDot;
	float blindMaxDarken;
	int blindFadeInTime;
	int blindFadeOutTime;
	float glareMinDot;
	float glareMaxDot;
	float glareMaxLighten;
	int glareFadeInTime;
	int glareFadeOutTime;
	float sunFxPosition[3];
};

struct XModelDrawInfo
{
	char hasGfxEntIndex;
	char lod;
	unsigned __int16 surfId;
};

struct GfxSceneDynModel
{
	XModelDrawInfo info;
	unsigned __int16 dynEntId;
};

struct BModelDrawInfo
{
	unsigned __int16 surfId;
};

struct GfxSceneDynBrush
{
	BModelDrawInfo info;
	unsigned __int16 dynEntId;
};

struct GfxShadowGeometry
{
	unsigned __int16 surfaceCount;
	unsigned __int16 smodelCount;
	unsigned __int16* sortedSurfIndex;
	unsigned __int16* smodelIndex;
};

struct GfxLightRegionAxis
{
	float dir[3];
	float midPoint;
	float halfSize;
};

struct GfxLightRegionHull
{
	float kdopMidPoint[9];
	float kdopHalfSize[9];
	unsigned int axisCount;
	GfxLightRegionAxis* axis;
};

struct GfxLightRegion
{
	unsigned int hullCount;
	GfxLightRegionHull* hulls;
};

struct GfxStaticModelInst
{
	Bounds bounds;
	float lightingOrigin[3];
};

struct srfTriangles_t
{
	unsigned int vertexLayerData;
	unsigned int firstVertex;
	unsigned __int16 vertexCount;
	unsigned __int16 triCount;
	unsigned int baseIndex;
};

struct GfxSurfaceLightingAndFlagsFields
{
	unsigned char lightmapIndex;
	unsigned char reflectionProbeIndex;
	unsigned char primaryLightIndex;
	unsigned char flags;
};

union GfxSurfaceLightingAndFlags
{
	GfxSurfaceLightingAndFlagsFields fields;
	unsigned int packed;
};

struct GfxSurface
{
	srfTriangles_t tris;
	Material* material;
	GfxSurfaceLightingAndFlags laf;
};

struct GfxSurfaceBounds
{
	Bounds bounds;
};

struct GfxPackedPlacement
{
	float origin[3];
	float axis[3][3];
	float scale;
};

struct GfxStaticModelDrawInst
{
	GfxPackedPlacement placement;
	XModel* model;
	unsigned __int16 cullDist;
	unsigned __int16 lightingHandle;
	unsigned char reflectionProbeIndex;
	unsigned char primaryLightIndex;
	unsigned char flags;
	unsigned char firstMtlSkinIndex;
	GfxColor groundLighting;
	unsigned __int16 cacheId[4];
};

struct GfxWorldDpvsStatic
{
	unsigned int smodelCount;
	unsigned int staticSurfaceCount;
	unsigned int staticSurfaceCountNoDecal;
	unsigned int litOpaqueSurfsBegin;
	unsigned int litOpaqueSurfsEnd;
	unsigned int litTransSurfsBegin;
	unsigned int litTransSurfsEnd;
	unsigned int shadowCasterSurfsBegin;
	unsigned int shadowCasterSurfsEnd;
	unsigned int emissiveSurfsBegin;
	unsigned int emissiveSurfsEnd;
	unsigned int smodelVisDataCount;
	unsigned int surfaceVisDataCount;
	unsigned char* smodelVisData[3];
	unsigned char* surfaceVisData[3];
	unsigned __int16* sortedSurfIndex;
	GfxStaticModelInst* smodelInsts;
	GfxSurface* surfaces;
	GfxSurfaceBounds* surfacesBounds;
	GfxStaticModelDrawInst* smodelDrawInsts;
	GfxDrawSurf* surfaceMaterials;
	unsigned int* surfaceCastsSunShadow;
	volatile int usageCount;
};

struct GfxSModelSurfHeaderFields
{
	char reflectionProbeIndex;
	char sceneLightIndex;
	unsigned __int16 materialSortedIndex : 12;
	unsigned __int16 visDataRefCountLessOne : 4;
};

union GfxSModelSurfHeader
{
	GfxSModelSurfHeaderFields fields;
	unsigned int packed;
	unsigned __int16 array[2];
};

struct GfxSModelSurfVisDataRefFields
{
	unsigned __int16 surfIndex : 4;
	unsigned __int16 visDataIndexPacked : 12;
};

union GfxSModelSurfVisDataRef
{
	GfxSModelSurfVisDataRefFields fields;
	unsigned __int16 packed;
};

struct GfxSModelSurf
{
	GfxSModelSurfHeader header;
	GfxSModelSurfVisDataRef visDataRefs[1];
};

struct GfxSModelSurfDelaySortFields
{
	unsigned __int16 unused;
	GfxSModelSurfVisDataRef visDataRef;
	GfxSModelSurfHeader header;
};

union GfxSModelSurfDelaySort
{
	GfxSModelSurfDelaySortFields fields;
	unsigned __int64 packed;
};

struct GfxSModelSurfBuildList
{
	GfxSModelSurfHeader lastSurfHeader;
	GfxSModelSurf* lastSModelSurf;
	unsigned int visDataRefCount;
	char* surfDataBegin;
	char* surfDataPos;
	char* surfDataEnd;
	GfxSModelSurfHeader minDelaySortKey;
	GfxSModelSurfDelaySort* delaySurfList;
	unsigned int delaySurfCount;
	unsigned int delaySurfLimit;
};

struct GfxSModelSurfDrawData
{
	unsigned int shadowCasterMaterialIndex;
	char* visData;
	unsigned int visDataUsed;
	unsigned int visDataLimit;
	GfxSModelSurfBuildList buildList[4][4];
};

struct GfxWorldDpvsDynamic
{
	unsigned int dynEntClientWordCount[2];
	unsigned int dynEntClientCount[2];
	unsigned int* dynEntCellBits[2];
	char* dynEntVisData[2][3];
};

struct GfxHeroOnlyLight
{
	char type;
	char unused[3];
	float color[3];
	float dir[3];
	float origin[3];
	float radius;
	float cosHalfFovOuter;
	float cosHalfFovInner;
	int exponent;
};

struct __declspec(align(4)) GfxWorld
{
	const char* name;
	const char* baseName;
	int planeCount;
	int nodeCount;
	unsigned int surfaceCount;
	int skyCount;
	GfxSky* skies;
	unsigned int lastSunPrimaryLightIndex;
	unsigned int primaryLightCount;
	unsigned int sortKeyLitDecal;
	unsigned int sortKeyEffectDecal;
	unsigned int sortKeyEffectAuto;
	unsigned int sortKeyDistortion;
	GfxWorldDpvsPlanes dpvsPlanes;
	GfxCellTreeCount* aabbTreeCounts;
	GfxCellTree* aabbTrees;
	GfxCell* cells;
	GfxWorldDraw draw;
	GfxLightGrid lightGrid;
	int modelCount;
	GfxBrushModel* models;
	Bounds bounds;
	unsigned int checksum;
	int materialMemoryCount;
	MaterialMemory* materialMemory;
	sunflare_t sun;
	float outdoorLookupMatrix[4][4];
	GfxImage* outdoorImage;
	unsigned int* cellCasterBits;
	unsigned int* cellHasSunLitSurfsBits;
	GfxSceneDynModel* sceneDynModel;
	GfxSceneDynBrush* sceneDynBrush;
	unsigned int* primaryLightEntityShadowVis;
	unsigned int* primaryLightDynEntShadowVis[2];
	char* nonSunPrimaryLightForModelDynEnt;
	GfxShadowGeometry* shadowGeom;
	GfxLightRegion* lightRegion;
	GfxWorldDpvsStatic dpvs;
	GfxWorldDpvsDynamic dpvsDyn;
	unsigned int mapVtxChecksum;
	unsigned int heroOnlyLightCount;
	GfxHeroOnlyLight* heroOnlyLights;
	char fogTypesAllowed;
};

struct __declspec(align(4)) GfxLightImage
{
	GfxImage* image;
	char samplerState;
};

struct GfxLightDef
{
	const char* name;
	GfxLightImage attenuation;
	int lmapLookupStart;
};

struct Glyph
{
	unsigned __int16 letter;
	char x0;
	char y0;
	char dx;
	char pixelWidth;
	char pixelHeight;
	float s0;
	float t0;
	float s1;
	float t1;
};

struct Font_s
{
	const char* fontName;
	int pixelHeight;
	int glyphCount;
	Material* material;
	Material* glowMaterial;
	Glyph* glyphs;
};

struct rectDef_s
{
	float x;
	float y;
	float w;
	float h;
	char horzAlign;
	char vertAlign;
};


struct windowDef_t
{
	const char* name;
	rectDef_s rect;
	rectDef_s rectClient;
	const char* group;
	int style;
	int border;
	int ownerDraw;
	int ownerDrawFlags;
	float borderSize;
	int staticFlags;
	int dynamicFlags[1];
	int nextTime;
	float foreColor[4];
	float backColor[4];
	float borderColor[4];
	float outlineColor[4];
	float disableColor[4];
	Material* background;
};


enum expDataType
{
	VAL_INT = 0x0,
	VAL_FLOAT = 0x1,
	VAL_STRING = 0x2,
	NUM_INTERNAL_DATATYPES = 0x3,
	VAL_FUNCTION = 0x3,
	NUM_DATATYPES = 0x4,
};

struct ExpressionString
{
	const char* string;
};

union operandInternalDataUnion
{
	int intVal;
	float floatVal;
	ExpressionString stringVal;
	Statement_s* function;
};

struct Operand
{
	expDataType dataType;
	operandInternalDataUnion internals;
};

enum operationEnum
{
	OP_NOOP = 0x0,
	OP_RIGHTPAREN = 0x1,
	OP_MULTIPLY = 0x2,
	OP_DIVIDE = 0x3,
	OP_MODULUS = 0x4,
	OP_ADD = 0x5,
	OP_SUBTRACT = 0x6,
	OP_NOT = 0x7,
	OP_LESSTHAN = 0x8,
	OP_LESSTHANEQUALTO = 0x9,
	OP_GREATERTHAN = 0xA,
	OP_GREATERTHANEQUALTO = 0xB,
	OP_EQUALS = 0xC,
	OP_NOTEQUAL = 0xD,
	OP_AND = 0xE,
	OP_OR = 0xF,
	OP_LEFTPAREN = 0x10,
	OP_COMMA = 0x11,
	OP_BITWISEAND = 0x12,
	OP_BITWISEOR = 0x13,
	OP_BITWISENOT = 0x14,
	OP_BITSHIFTLEFT = 0x15,
	OP_BITSHIFTRIGHT = 0x16,
	OP_STATICDVARINT = 0x17,
	OP_FIRSTFUNCTIONCALL = 0x17,
	OP_STATICDVARBOOL = 0x18,
	OP_STATICDVARFLOAT = 0x19,
	OP_STATICDVARSTRING = 0x1A,
	OP_TOINT = 0x1B,
	OP_TOSTRING = 0x1C,
	OP_TOFLOAT = 0x1D,
	LAST_COMMONLY_CALLED_FUNCTION = 0x1D,
	OP_SIN = 0x1E,
	OP_COS = 0x1F,
	OP_MIN = 0x20,
	OP_MAX = 0x21,
	OP_MILLISECONDS = 0x22,
	OP_DVARINT = 0x23,
	OP_DVARBOOL = 0x24,
	OP_DVARFLOAT = 0x25,
	OP_DVARSTRING = 0x26,
	OP_STAT = 0x27,
	OP_UIACTIVE = 0x28,
	OP_FLASHBANGED = 0x29,
	OP_USINGVEHICLE = 0x2A,
	OP_MISSILECAM = 0x2B,
	OP_SCOPED = 0x2C,
	OP_SCOPEDTHERMAL = 0x2D,
	OP_SCOREBOARDVISIBLE = 0x2E,
	OP_INKILLCAM = 0x2F,
	OP_INKILLCAM_NPC = 0x30,
	OP_PLAYERFIELD = 0x31,
	OP_GET_PLAYER_PERK = 0x32,
	OP_SELECTINGLOCATION = 0x33,
	OP_SELECTINGDIRECTION = 0x34,
	OP_TEAMFIELD = 0x35,
	OP_OTHERTEAMFIELD = 0x36,
	OP_MARINESFIELD = 0x37,
	OP_OPFORFIELD = 0x38,
	OP_MENUISOPEN = 0x39,
	OP_WRITINGDATA = 0x3A,
	OP_INLOBBY = 0x3B,
	OP_INPRIVATEPARTY = 0x3C,
	OP_PRIVATEPARTYHOST = 0x3D,
	OP_PRIVATEPARTYHOSTINLOBBY = 0x3E,
	OP_ALONEINPARTY = 0x3F,
	OP_ADSJAVELIN = 0x40,
	OP_WEAPLOCKBLINK = 0x41,
	OP_WEAPATTACKTOP = 0x42,
	OP_WEAPATTACKDIRECT = 0x43,
	OP_WEAPLOCKING = 0x44,
	OP_WEAPLOCKED = 0x45,
	OP_WEAPLOCKTOOCLOSE = 0x46,
	OP_WEAPLOCKSCREENPOSX = 0x47,
	OP_WEAPLOCKSCREENPOSY = 0x48,
	OP_SECONDSASTIME = 0x49,
	OP_TABLELOOKUP = 0x4A,
	OP_TABLELOOKUPBYROW = 0x4B,
	OP_TABLEGETROWNUM = 0x4C,
	OP_LOCALIZESTRING = 0x4D,
	OP_LOCALVARINT = 0x4E,
	OP_LOCALVARBOOL = 0x4F,
	OP_LOCALVARFLOAT = 0x50,
	OP_LOCALVARSTRING = 0x51,
	OP_TIMELEFT = 0x52,
	OP_SECONDSASCOUNTDOWN = 0x53,
	OP_GAMEMSGWNDACTIVE = 0x54,
	OP_GAMETYPENAME = 0x55,
	OP_GAMETYPE = 0x56,
	OP_GAMETYPEDESCRIPTION = 0x57,
	OP_SCORE = 0x58,
	OP_FRIENDSONLINE = 0x59,
	OP_FOLLOWING = 0x5A,
	OP_SPECTATINGFREE = 0x5B,
	OP_STATRANGEBITSSET = 0x5C,
	OP_KEYBINDING = 0x5D,
	OP_ACTIONSLOTUSABLE = 0x5E,
	OP_HUDFADE = 0x5F,
	OP_MAXPLAYERS = 0x60,
	OP_ACCEPTINGINVITE = 0x61,
	OP_ISINTERMISSION = 0x62,
	OP_GAMEHOST = 0x63,
	OP_PARTYHASMISSINGMAPPACK = 0x64,
	OP_PARTYMISSINGMAPPACKERROR = 0x65,
	OP_ANYNEWMAPPACKS = 0x66,
	OP_AMISELECTED = 0x67,
	OP_PARTYSTATUSSTRING = 0x68,
	OP_ATTACHED_CONTROLLER_COUNT = 0x69,
	OP_IS_SPLIT_SCREEN_ONLINE_POSSIBLE = 0x6A,
	OP_SPLITSCREENPLAYERCOUNT = 0x6B,
	OP_GETPLAYERDATA = 0x6C,
	OP_GETPLAYERDATASPLITSCREEN = 0x6D,
	OP_EXPERIENCE_FOR_LEVEL = 0x6E,
	OP_LEVEL_FOR_EXPERIENCE = 0x6F,
	OP_IS_ITEM_UNLOCKED = 0x70,
	OP_IS_ITEM_UNLOCKEDSPLITSCREEN = 0x71,
	OP_DEBUG_PRINT = 0x72,
	OP_GETPLAYERDATA_ANYBOOLTRUE = 0x73,
	OP_WEAPON_CLASS_NEW = 0x74,
	OP_WEAPONNAME = 0x75,
	OP_ISRELOADING = 0x76,
	OP_SAVE_GAME_AVAILABLE = 0x77,
	OP_UNLOCKED_ITEM_COUNT = 0x78,
	OP_UNLOCKED_ITEM_COUNT_SPLITSCREEN = 0x79,
	OP_UNLOCKED_ITEM = 0x7A,
	OP_UNLOCKED_ITEM_SPLITSCREEN = 0x7B,
	OP_MAIL_SUBJECT = 0x7C,
	OP_MAIL_FROM = 0x7D,
	OP_MAIL_RECEIVED = 0x7E,
	OP_MAIL_BODY = 0x7F,
	OP_MAIL_LOOT_LOCALIZED = 0x80,
	OP_MAIL_GIVES_LOOT = 0x81,
	OP_ANY_NEW_MAIL = 0x82,
	OP_MAIL_TIME_TO_FOLLOWUP = 0x83,
	OP_MAIL_LOOT_TYPE = 0x84,
	OP_MAIL_RAN_LOTTERY = 0x85,
	OP_LOTTERY_LOOT_LOCALIZED = 0x86,
	OP_RADAR_IS_JAMMED = 0x87,
	OP_RADAR_JAM_INTENSITY = 0x88,
	OP_RADAR_IS_ENABLED = 0x89,
	OP_EMP_JAMMED = 0x8A,
	OP_PLAYERADS = 0x8B,
	OP_WEAPON_HEAT_ACTIVE = 0x8C,
	OP_WEAPON_HEAT_VALUE = 0x8D,
	OP_WEAPON_HEAT_OVERHEATED = 0x8E,
	OP_SPLASH_TEXT = 0x8F,
	OP_SPLASH_DESCRIPTION = 0x90,
	OP_SPLASH_MATERIAL = 0x91,
	OP_SPLASH_HAS_ICON = 0x92,
	OP_SPLASH_ROWNUM = 0x93,
	OP_GETFOCUSED_NAME = 0x94,
	OP_GETFOCUSED_X = 0x95,
	OP_GETFOCUSED_Y = 0x96,
	OP_GETFOCUSED_W = 0x97,
	OP_GETFOCUSED_H = 0x98,
	OP_GETITEMDEF_X = 0x99,
	OP_GETITEMDEF_Y = 0x9A,
	OP_GETITEMDEF_W = 0x9B,
	OP_GETITEMDEF_H = 0x9C,
	OP_PLAYLISTFIELD = 0x9D,
	OP_SCOREBOARD_EXTERNALMUTE_NOTICE = 0x9E,
	OP_CLIENT_MATCH_DATA = 0x9F,
	OP_CLIENT_MATCH_DATA_DEF = 0xA0,
	OP_GET_MAP_NAME = 0xA1,
	OP_GET_MAP_IMAGE = 0xA2,
	OP_GET_MAP_CUSTOM = 0xA3,
	OP_GET_MIGRATION_STATUS = 0xA4,
	OP_GET_PLAYERCARD_INFO = 0xA5,
	OP_IS_OFFLINE_PROFILE_SELECTED = 0xA6,
	OP_COOP_PLAYERFIELD = 0xA7,
	OP_IS_COOP = 0xA8,
	OP_GETPARTYSTATUS = 0xA9,
	OP_GETSEARCHPARAMS = 0xAA,
	OP_GETTIMEPLAYED = 0xAB,
	OP_IS_SELECTED_PLAYER_FRIEND = 0xAC,
	OP_GETCHARBYINDEX = 0xAD,
	OP_GETPLAYERPROFILEDATA = 0xAE,
	OP_IS_PROFILE_SIGNED_IN = 0xAF,
	OP_GET_WAIT_POPUP_STATUS = 0xB0,
	NUM_OPERATORS = 0xB1,
};

union entryInternalData
{
	operationEnum op;
	Operand operand;
};

struct expressionEntry
{
	int type;
	entryInternalData data;
};

struct UIFunctionList
{
	int totalFunctions;
	Statement_s** functions;
};

union DvarValue
{
	bool enabled;
	int integer;
	unsigned int unsignedInt;
	float value;
	float vector[4];
	const char* string;
	unsigned char color[4];
};


struct enum_limit
{
	int stringCount;
	const char** strings;
};

struct int_limit
{
	int min;
	int max;
};

struct float_limit
{
	float min;
	float max;
};

union DvarLimits
{
	enum_limit enumeration;
	int_limit integer;
	float_limit value;
	float_limit vector;
};


struct dvar_t
{
	const char* name;
	const char* description;
	unsigned int flags;
	dvar_type type;
	bool modified;
	DvarValue current;
	DvarValue latched;
	DvarValue reset;
	DvarLimits domain;
	bool(__cdecl* domainFunc)(dvar_t*, DvarValue);
	dvar_t* hashNext;
};


struct StaticDvar
{
	dvar_t* dvar;
	char* dvarName;
};

struct StaticDvarList
{
	int numStaticDvars;
	StaticDvar** staticDvars;
};

struct StringList
{
	int totalStrings;
	const char** strings;
};

struct ExpressionSupportingData
{
	UIFunctionList uifunctions;
	StaticDvarList staticDvarList;
	StringList uiStrings;
};

struct Statement_s
{
	int numEntries;
	expressionEntry* entries;
	ExpressionSupportingData* supportingData;
	int lastExecuteTime;
	Operand lastResult;
};

enum EventType
{
	EVENT_UNCONDITIONAL = 0x0,
	EVENT_IF = 0x1,
	EVENT_ELSE = 0x2,
	EVENT_SET_LOCAL_VAR_BOOL = 0x3,
	EVENT_SET_LOCAL_VAR_INT = 0x4,
	EVENT_SET_LOCAL_VAR_FLOAT = 0x5,
	EVENT_SET_LOCAL_VAR_STRING = 0x6,
	EVENT_COUNT = 0x7,
};

struct ConditionalScript
{
	MenuEventHandlerSet* eventHandlerSet;
	Statement_s* eventExpression;
};

struct SetLocalVarData
{
	const char* localVarName;
	Statement_s* expression;
};

union EventData
{
	const char* unconditionalScript;
	ConditionalScript* conditionalScript;
	struct MenuEventHandlerSet* elseScript;
	SetLocalVarData* setLocalVarData;
};

struct __declspec(align(4)) MenuEventHandler
{
	EventData eventData;
	char eventType;
};

struct MenuEventHandlerSet
{
	int eventHandlerCount;
	MenuEventHandler** eventHandlers;
};

struct ItemKeyHandler
{
	int key;
	MenuEventHandlerSet* action;
	ItemKeyHandler* next;
};

struct columnInfo_s
{
	int pos;
	int width;
	int maxChars;
	int alignment;
};

struct listBoxDef_s
{
	int mousePos;
	int startPos[1];
	int endPos[1];
	int drawPadding;
	float elementWidth;
	float elementHeight;
	int elementStyle;
	int numColumns;
	columnInfo_s columnInfo[16];
	MenuEventHandlerSet* onDoubleClick;
	int notselectable;
	int noScrollBars;
	int usePaging;
	float selectBorder[4];
	Material* selectIcon;
};

struct editFieldDef_s
{
	float minVal;
	float maxVal;
	float defVal;
	float range;
	int maxChars;
	int maxCharsGotoNext;
	int maxPaintChars;
	int paintOffset;
};

struct multiDef_s
{
	const char* dvarList[32];
	const char* dvarStr[32];
	float dvarValue[32];
	int count;
	int strDef;
};

struct newsTickerDef_s
{
	int feedId;
	int speed;
	int spacing;
	int lastTime;
	int start;
	int end;
	float x;
};

struct textScrollDef_s
{
	int startTime;
};

union itemDefData_t
{
	listBoxDef_s* listBox;
	editFieldDef_s* editField;
	multiDef_s* multi;
	const char* enumDvarName;
	newsTickerDef_s* ticker;
	textScrollDef_s* scroll;
	void* data;
};

struct ItemFloatExpression
{
	int target;
	Statement_s* expression;
};

enum ItemDefType
{
	ITEM_TYPE_TEXT = 0x0,
	ITEM_TYPE_BUTTON = 0x1,
	ITEM_TYPE_RADIOBUTTON = 0x2,
	ITEM_TYPE_CHECKBOX = 0x3,
	ITEM_TYPE_EDITFIELD = 0x4,
	ITEM_TYPE_COMBO = 0x5,
	ITEM_TYPE_LISTBOX = 0x6,
	ITEM_TYPE_MODEL = 0x7,
	ITEM_TYPE_OWNERDRAW = 0x8,
	ITEM_TYPE_NUMERICFIELD = 0x9,
	ITEM_TYPE_SLIDER = 0xA,
	ITEM_TYPE_YESNO = 0xB,
	ITEM_TYPE_MULTI = 0xC,
	ITEM_TYPE_DVARENUM = 0xD,
	ITEM_TYPE_BIND = 0xE,
	ITEM_TYPE_MENUMODEL = 0xF,
	ITEM_TYPE_VALIDFILEFIELD = 0x10,
	ITEM_TYPE_DECIMALFIELD = 0x11,
	ITEM_TYPE_UPREDITFIELD = 0x12,
	ITEM_TYPE_GAME_MESSAGE_WINDOW = 0x13,
	ITEM_TYPE_NEWS_TICKER = 0x14,
	ITEM_TYPE_TEXT_SCROLL = 0x15,
	ITEM_TYPE_EMAILFIELD = 0x16,
	ITEM_TYPE_PASSWORDFIELD = 0x17,
};

struct itemDef_s
{
	windowDef_t window;
	rectDef_s textRect[1];
	int type;
	int dataType;
	int alignment;
	int fontEnum;
	int textAlignMode;
	float textalignx;
	float textaligny;
	float textscale;
	int textStyle;
	int gameMsgWindowIndex;
	int gameMsgWindowMode;
	const char* text;
	int itemFlags;
	menuDef_t* parent;
	MenuEventHandlerSet* mouseEnterText;
	MenuEventHandlerSet* mouseExitText;
	MenuEventHandlerSet* mouseEnter;
	MenuEventHandlerSet* mouseExit;
	MenuEventHandlerSet* action;
	MenuEventHandlerSet* accept;
	MenuEventHandlerSet* onFocus;
	MenuEventHandlerSet* leaveFocus;
	const char* dvar;
	const char* dvarTest;
	ItemKeyHandler* onKey;
	const char* enableDvar;
	const char* localVar;
	int dvarFlags;
	snd_alias_list_t* focusSound;
	float special;
	int cursorPos[1];
	itemDefData_t typeData;
	int imageTrack;
	int floatExpressionCount;
	ItemFloatExpression* floatExpressions;
	Statement_s* visibleExp;
	Statement_s* disabledExp;
	Statement_s* textExp;
	Statement_s* materialExp;
	float glowColor[4];
	bool decayActive;
	int fxBirthTime;
	int fxLetterTime;
	int fxDecayStartTime;
	int fxDecayDuration;
	int lastSoundPlayedTime;
};

struct menuTransition
{
	int transitionType;
	int targetField;
	int startTime;
	float startVal;
	float endVal;
	float time;
	int endTriggerType;
};

struct menuDef_t
{
	windowDef_t window;
	const char* font;
	int fullScreen;
	int itemCount;
	int fontIndex;
	int cursorItem[1];
	int fadeCycle;
	float fadeClamp;
	float fadeAmount;
	float fadeInAmount;
	float blurRadius;
	MenuEventHandlerSet* onOpen;
	MenuEventHandlerSet* onCloseRequest;
	MenuEventHandlerSet* onClose;
	MenuEventHandlerSet* onESC;
	ItemKeyHandler* onKey;
	Statement_s* visibleExp;
	const char* allowedBinding;
	const char* soundName;
	int imageTrack;
	float focusColor[4];
	Statement_s* rectXExp;
	Statement_s* rectYExp;
	Statement_s* rectWExp;
	Statement_s* rectHExp;
	Statement_s* openSoundExp;
	Statement_s* closeSoundExp;
	itemDef_s** items;
	menuTransition scaleTransition[1];
	menuTransition alphaTransition[1];
	menuTransition xTransition[1];
	menuTransition yTransition[1];
	ExpressionSupportingData* expressionData;
};

struct MenuList
{
	const char* name;
	int menuCount;
	menuDef_t** menus;
};

struct LocalizeEntry
{
	const char* value;
	const char* name;
};

enum weapType_t
{
	WEAPTYPE_BULLET = 0x0,
	WEAPTYPE_GRENADE = 0x1,
	WEAPTYPE_PROJECTILE = 0x2,
	WEAPTYPE_RIOTSHIELD = 0x3,
	WEAPTYPE_NUM = 0x4,
};

enum weapClass_t
{
	WEAPCLASS_RIFLE = 0x0,
	WEAPCLASS_SNIPER = 0x1,
	WEAPCLASS_MG = 0x2,
	WEAPCLASS_SMG = 0x3,
	WEAPCLASS_SPREAD = 0x4,
	WEAPCLASS_PISTOL = 0x5,
	WEAPCLASS_GRENADE = 0x6,
	WEAPCLASS_ROCKETLAUNCHER = 0x7,
	WEAPCLASS_TURRET = 0x8,
	WEAPCLASS_THROWINGKNIFE = 0x9,
	WEAPCLASS_NON_PLAYER = 0xA,
	WEAPCLASS_ITEM = 0xB,
	WEAPCLASS_NUM = 0xC,
};

enum PenetrateType
{
	PENETRATE_TYPE_NONE = 0x0,
	PENETRATE_TYPE_SMALL = 0x1,
	PENETRATE_TYPE_MEDIUM = 0x2,
	PENETRATE_TYPE_LARGE = 0x3,
	PENETRATE_TYPE_COUNT = 0x4,
};

enum weapInventoryType_t
{
	WEAPINVENTORY_PRIMARY = 0x0,
	WEAPINVENTORY_OFFHAND = 0x1,
	WEAPINVENTORY_ITEM = 0x2,
	WEAPINVENTORY_ALTMODE = 0x3,
	WEAPINVENTORY_EXCLUSIVE = 0x4,
	WEAPINVENTORY_SCAVENGER = 0x5,
	WEAPINVENTORYCOUNT = 0x6,
};

enum weapFireType_t
{
	WEAPON_FIRETYPE_FULLAUTO = 0x0,
	WEAPON_FIRETYPE_SINGLESHOT = 0x1,
	WEAPON_FIRETYPE_BURSTFIRE2 = 0x2,
	WEAPON_FIRETYPE_BURSTFIRE3 = 0x3,
	WEAPON_FIRETYPE_BURSTFIRE4 = 0x4,
	WEAPON_FIRETYPE_DOUBLEBARREL = 0x5,
	WEAPON_FIRETYPECOUNT = 0x6,
	WEAPON_FIRETYPE_BURSTFIRE_FIRST = 0x2,
	WEAPON_FIRETYPE_BURSTFIRE_LAST = 0x4,
};
enum weapStance_t
{
	WEAPSTANCE_STAND = 0x0,
	WEAPSTANCE_DUCK = 0x1,
	WEAPSTANCE_PRONE = 0x2,
	WEAPSTANCE_NUM = 0x3,
};

enum activeReticleType_t
{
	VEH_ACTIVE_RETICLE_NONE = 0x0,
	VEH_ACTIVE_RETICLE_PIP_ON_A_STICK = 0x1,
	VEH_ACTIVE_RETICLE_BOUNCING_DIAMOND = 0x2,
	VEH_ACTIVE_RETICLE_COUNT = 0x3,
};

enum weaponIconRatioType_t
{
	WEAPON_ICON_RATIO_1TO1 = 0x0,
	WEAPON_ICON_RATIO_2TO1 = 0x1,
	WEAPON_ICON_RATIO_4TO1 = 0x2,
	WEAPON_ICON_RATIO_COUNT = 0x3,
};

enum ammoCounterClipType_t
{
	AMMO_COUNTER_CLIP_NONE = 0x0,
	AMMO_COUNTER_CLIP_MAGAZINE = 0x1,
	AMMO_COUNTER_CLIP_SHORTMAGAZINE = 0x2,
	AMMO_COUNTER_CLIP_SHOTGUN = 0x3,
	AMMO_COUNTER_CLIP_ROCKET = 0x4,
	AMMO_COUNTER_CLIP_BELTFED = 0x5,
	AMMO_COUNTER_CLIP_ALTWEAPON = 0x6,
	AMMO_COUNTER_CLIP_COUNT = 0x7,
};

enum weapOverlayReticle_t
{
	WEAPOVERLAYRETICLE_NONE = 0x0,
	WEAPOVERLAYRETICLE_CROSSHAIR = 0x1,
	WEAPOVERLAYRETICLE_NUM = 0x2,
};

enum WeapOverlayInteface_t
{
	WEAPOVERLAYINTERFACE_NONE = 0x0,
	WEAPOVERLAYINTERFACE_JAVELIN = 0x1,
	WEAPOVERLAYINTERFACE_TURRETSCOPE = 0x2,
	WEAPOVERLAYINTERFACECOUNT = 0x3,
};

enum weapProjExposion_t
{
	WEAPPROJEXP_GRENADE = 0x0,
	WEAPPROJEXP_ROCKET = 0x1,
	WEAPPROJEXP_FLASHBANG = 0x2,
	WEAPPROJEXP_NONE = 0x3,
	WEAPPROJEXP_DUD = 0x4,
	WEAPPROJEXP_SMOKE = 0x5,
	WEAPPROJEXP_HEAVY = 0x6,
	WEAPPROJEXP_NUM = 0x7,
};

enum WeapStickinessType
{
	WEAPSTICKINESS_NONE = 0x0,
	WEAPSTICKINESS_ALL = 0x1,
	WEAPSTICKINESS_ALL_ORIENT = 0x2,
	WEAPSTICKINESS_GROUND = 0x3,
	WEAPSTICKINESS_GROUND_WITH_YAW = 0x4,
	WEAPSTICKINESS_KNIFE = 0x5,
	WEAPSTICKINESS_COUNT = 0x6,
};

enum guidedMissileType_t
{
	MISSILE_GUIDANCE_NONE = 0x0,
	MISSILE_GUIDANCE_SIDEWINDER = 0x1,
	MISSILE_GUIDANCE_HELLFIRE = 0x2,
	MISSILE_GUIDANCE_JAVELIN = 0x3,
	MISSILE_GUIDANCE_COUNT = 0x4,
};

struct TracerDef
{
	const char* name;
	Material* material;
	unsigned int drawInterval;
	float speed;
	float beamLength;
	float beamWidth;
	float screwRadius;
	float screwDist;
	float colors[5][4];
};

struct NetField
{
	char* name;
	int offset;
	int bits;
	char changeHints;
};

struct __declspec(align(4)) WeaponDef
{
	const char* szOverlayName;
	XModel** gunXModel;
	XModel* handXModel;
	const char** szXAnimsRightHanded;
	const char** szXAnimsLeftHanded;
	const char* szModeName;
	unsigned __int16* notetrackSoundMapKeys;
	unsigned __int16* notetrackSoundMapValues;
	unsigned __int16* notetrackRumbleMapKeys;
	unsigned __int16* notetrackRumbleMapValues;
	int playerAnimType;
	weapType_t weapType;
	weapClass_t weapClass;
	PenetrateType penetrateType;
	weapInventoryType_t inventoryType;
	weapFireType_t fireType;
	OffhandClass offhandClass;
	weapStance_t stance;
	FxEffectDef* viewFlashEffect;
	FxEffectDef* worldFlashEffect;
	snd_alias_list_t* pickupSound;
	snd_alias_list_t* pickupSoundPlayer;
	snd_alias_list_t* ammoPickupSound;
	snd_alias_list_t* ammoPickupSoundPlayer;
	snd_alias_list_t* projectileSound;
	snd_alias_list_t* pullbackSound;
	snd_alias_list_t* pullbackSoundPlayer;
	snd_alias_list_t* fireSound;
	snd_alias_list_t* fireSoundPlayer;
	snd_alias_list_t* fireSoundPlayerAkimbo;
	snd_alias_list_t* fireLoopSound;
	snd_alias_list_t* fireLoopSoundPlayer;
	snd_alias_list_t* fireStopSound;
	snd_alias_list_t* fireStopSoundPlayer;
	snd_alias_list_t* fireLastSound;
	snd_alias_list_t* fireLastSoundPlayer;
	snd_alias_list_t* emptyFireSound;
	snd_alias_list_t* emptyFireSoundPlayer;
	snd_alias_list_t* meleeSwipeSound;
	snd_alias_list_t* meleeSwipeSoundPlayer;
	snd_alias_list_t* meleeHitSound;
	snd_alias_list_t* meleeMissSound;
	snd_alias_list_t* rechamberSound;
	snd_alias_list_t* rechamberSoundPlayer;
	snd_alias_list_t* reloadSound;
	snd_alias_list_t* reloadSoundPlayer;
	snd_alias_list_t* reloadEmptySound;
	snd_alias_list_t* reloadEmptySoundPlayer;
	snd_alias_list_t* reloadStartSound;
	snd_alias_list_t* reloadStartSoundPlayer;
	snd_alias_list_t* reloadEndSound;
	snd_alias_list_t* reloadEndSoundPlayer;
	snd_alias_list_t* detonateSound;
	snd_alias_list_t* detonateSoundPlayer;
	snd_alias_list_t* nightVisionWearSound;
	snd_alias_list_t* nightVisionWearSoundPlayer;
	snd_alias_list_t* nightVisionRemoveSound;
	snd_alias_list_t* nightVisionRemoveSoundPlayer;
	snd_alias_list_t* altSwitchSound;
	snd_alias_list_t* altSwitchSoundPlayer;
	snd_alias_list_t* raiseSound;
	snd_alias_list_t* raiseSoundPlayer;
	snd_alias_list_t* firstRaiseSound;
	snd_alias_list_t* firstRaiseSoundPlayer;
	snd_alias_list_t* putawaySound;
	snd_alias_list_t* putawaySoundPlayer;
	snd_alias_list_t* scanSound;
	snd_alias_list_t** bounceSound;
	FxEffectDef* viewShellEjectEffect;
	FxEffectDef* worldShellEjectEffect;
	FxEffectDef* viewLastShotEjectEffect;
	FxEffectDef* worldLastShotEjectEffect;
	Material* reticleCenter;
	Material* reticleSide;
	int iReticleCenterSize;
	int iReticleSideSize;
	int iReticleMinOfs;
	activeReticleType_t activeReticleType;
	float vStandMove[3];
	float vStandRot[3];
	float strafeMove[3];
	float strafeRot[3];
	float vDuckedOfs[3];
	float vDuckedMove[3];
	float vDuckedRot[3];
	float vProneOfs[3];
	float vProneMove[3];
	float vProneRot[3];
	float fPosMoveRate;
	float fPosProneMoveRate;
	float fStandMoveMinSpeed;
	float fDuckedMoveMinSpeed;
	float fProneMoveMinSpeed;
	float fPosRotRate;
	float fPosProneRotRate;
	float fStandRotMinSpeed;
	float fDuckedRotMinSpeed;
	float fProneRotMinSpeed;
	XModel** worldModel;
	XModel* worldClipModel;
	XModel* rocketModel;
	XModel* knifeModel;
	XModel* worldKnifeModel;
	Material* hudIcon;
	weaponIconRatioType_t hudIconRatio;
	Material* pickupIcon;
	weaponIconRatioType_t pickupIconRatio;
	Material* ammoCounterIcon;
	weaponIconRatioType_t ammoCounterIconRatio;
	ammoCounterClipType_t ammoCounterClip;
	int iStartAmmo;
	const char* szAmmoName;
	int iAmmoIndex;
	const char* szClipName;
	int iClipIndex;
	int iMaxAmmo;
	int shotCount;
	const char* szSharedAmmoCapName;
	int iSharedAmmoCapIndex;
	int iSharedAmmoCap;
	int damage;
	int playerDamage;
	int iMeleeDamage;
	int iDamageType;
	int iFireDelay;
	int iMeleeDelay;
	int meleeChargeDelay;
	int iDetonateDelay;
	int iRechamberTime;
	int rechamberTimeOneHanded;
	int iRechamberBoltTime;
	int iHoldFireTime;
	int iDetonateTime;
	int iMeleeTime;
	int meleeChargeTime;
	int iReloadTime;
	int reloadShowRocketTime;
	int iReloadEmptyTime;
	int iReloadAddTime;
	int iReloadStartTime;
	int iReloadStartAddTime;
	int iReloadEndTime;
	int iDropTime;
	int iRaiseTime;
	int iAltDropTime;
	int quickDropTime;
	int quickRaiseTime;
	int iBreachRaiseTime;
	int iEmptyRaiseTime;
	int iEmptyDropTime;
	int sprintInTime;
	int sprintLoopTime;
	int sprintOutTime;
	int stunnedTimeBegin;
	int stunnedTimeLoop;
	int stunnedTimeEnd;
	int nightVisionWearTime;
	int nightVisionWearTimeFadeOutEnd;
	int nightVisionWearTimePowerUp;
	int nightVisionRemoveTime;
	int nightVisionRemoveTimePowerDown;
	int nightVisionRemoveTimeFadeInStart;
	int fuseTime;
	int aiFuseTime;
	float autoAimRange;
	float aimAssistRange;
	float aimAssistRangeAds;
	float aimPadding;
	float enemyCrosshairRange;
	float moveSpeedScale;
	float adsMoveSpeedScale;
	float sprintDurationScale;
	float fAdsZoomInFrac;
	float fAdsZoomOutFrac;
	Material* overlayMaterial;
	Material* overlayMaterialLowRes;
	Material* overlayMaterialEMP;
	Material* overlayMaterialEMPLowRes;
	weapOverlayReticle_t overlayReticle;
	WeapOverlayInteface_t overlayInterface;
	float overlayWidth;
	float overlayHeight;
	float overlayWidthSplitscreen;
	float overlayHeightSplitscreen;
	float fAdsBobFactor;
	float fAdsViewBobMult;
	float fHipSpreadStandMin;
	float fHipSpreadDuckedMin;
	float fHipSpreadProneMin;
	float hipSpreadStandMax;
	float hipSpreadDuckedMax;
	float hipSpreadProneMax;
	float fHipSpreadDecayRate;
	float fHipSpreadFireAdd;
	float fHipSpreadTurnAdd;
	float fHipSpreadMoveAdd;
	float fHipSpreadDuckedDecay;
	float fHipSpreadProneDecay;
	float fHipReticleSidePos;
	float fAdsIdleAmount;
	float fHipIdleAmount;
	float adsIdleSpeed;
	float hipIdleSpeed;
	float fIdleCrouchFactor;
	float fIdleProneFactor;
	float fGunMaxPitch;
	float fGunMaxYaw;
	float swayMaxAngle;
	float swayLerpSpeed;
	float swayPitchScale;
	float swayYawScale;
	float swayHorizScale;
	float swayVertScale;
	float swayShellShockScale;
	float adsSwayMaxAngle;
	float adsSwayLerpSpeed;
	float adsSwayPitchScale;
	float adsSwayYawScale;
	float adsSwayHorizScale;
	float adsSwayVertScale;
	float adsViewErrorMin;
	float adsViewErrorMax;
	PhysCollmap* physCollmap;
	float dualWieldViewModelOffset;
	weaponIconRatioType_t killIconRatio;
	int iReloadAmmoAdd;
	int iReloadStartAdd;
	int ammoDropStockMin;
	int ammoDropClipPercentMin;
	int ammoDropClipPercentMax;
	int iExplosionRadius;
	int iExplosionRadiusMin;
	int iExplosionInnerDamage;
	int iExplosionOuterDamage;
	float damageConeAngle;
	float bulletExplDmgMult;
	float bulletExplRadiusMult;
	int iProjectileSpeed;
	int iProjectileSpeedUp;
	int iProjectileSpeedForward;
	int iProjectileActivateDist;
	float projLifetime;
	float timeToAccelerate;
	float projectileCurvature;
	XModel* projectileModel;
	weapProjExposion_t projExplosion;
	FxEffectDef* projExplosionEffect;
	FxEffectDef* projDudEffect;
	snd_alias_list_t* projExplosionSound;
	snd_alias_list_t* projDudSound;
	WeapStickinessType stickiness;
	float lowAmmoWarningThreshold;
	float ricochetChance;
	float* parallelBounce;
	float* perpendicularBounce;
	FxEffectDef* projTrailEffect;
	FxEffectDef* projBeaconEffect;
	float vProjectileColor[3];
	guidedMissileType_t guidedMissileType;
	float maxSteeringAccel;
	int projIgnitionDelay;
	FxEffectDef* projIgnitionEffect;
	snd_alias_list_t* projIgnitionSound;
	float fAdsAimPitch;
	float fAdsCrosshairInFrac;
	float fAdsCrosshairOutFrac;
	int adsGunKickReducedKickBullets;
	float adsGunKickReducedKickPercent;
	float fAdsGunKickPitchMin;
	float fAdsGunKickPitchMax;
	float fAdsGunKickYawMin;
	float fAdsGunKickYawMax;
	float fAdsGunKickAccel;
	float fAdsGunKickSpeedMax;
	float fAdsGunKickSpeedDecay;
	float fAdsGunKickStaticDecay;
	float fAdsViewKickPitchMin;
	float fAdsViewKickPitchMax;
	float fAdsViewKickYawMin;
	float fAdsViewKickYawMax;
	float fAdsViewScatterMin;
	float fAdsViewScatterMax;
	float fAdsSpread;
	int hipGunKickReducedKickBullets;
	float hipGunKickReducedKickPercent;
	float fHipGunKickPitchMin;
	float fHipGunKickPitchMax;
	float fHipGunKickYawMin;
	float fHipGunKickYawMax;
	float fHipGunKickAccel;
	float fHipGunKickSpeedMax;
	float fHipGunKickSpeedDecay;
	float fHipGunKickStaticDecay;
	float fHipViewKickPitchMin;
	float fHipViewKickPitchMax;
	float fHipViewKickYawMin;
	float fHipViewKickYawMax;
	float fHipViewScatterMin;
	float fHipViewScatterMax;
	float fightDist;
	float maxDist;
	const char* accuracyGraphName[2];
	float(*originalAccuracyGraphKnots[2])[2];
	unsigned __int16 originalAccuracyGraphKnotCount[2];
	int iPositionReloadTransTime;
	float leftArc;
	float rightArc;
	float topArc;
	float bottomArc;
	float accuracy;
	float aiSpread;
	float playerSpread;
	float minTurnSpeed[2];
	float maxTurnSpeed[2];
	float pitchConvergenceTime;
	float yawConvergenceTime;
	float suppressTime;
	float maxRange;
	float fAnimHorRotateInc;
	float fPlayerPositionDist;
	const char* szUseHintString;
	const char* dropHintString;
	int iUseHintStringIndex;
	int dropHintStringIndex;
	float horizViewJitter;
	float vertViewJitter;
	float scanSpeed;
	float scanAccel;
	int scanPauseTime;
	const char* szScript;
	float fOOPosAnimLength[2];
	int minDamage;
	int minPlayerDamage;
	float fMaxDamageRange;
	float fMinDamageRange;
	float destabilizationRateTime;
	float destabilizationCurvatureMax;
	int destabilizeDistance;
	float* locationDamageMultipliers;
	const char* fireRumble;
	const char* meleeImpactRumble;
	TracerDef* tracerType;
	float turretScopeZoomRate;
	float turretScopeZoomMin;
	float turretScopeZoomMax;
	float turretOverheatUpRate;
	float turretOverheatDownRate;
	float turretOverheatPenalty;
	snd_alias_list_t* turretOverheatSound;
	FxEffectDef* turretOverheatEffect;
	const char* turretBarrelSpinRumble;
	float turretBarrelSpinSpeed;
	float turretBarrelSpinUpTime;
	float turretBarrelSpinDownTime;
	snd_alias_list_t* turretBarrelSpinMaxSnd;
	snd_alias_list_t* turretBarrelSpinUpSnd[4];
	snd_alias_list_t* turretBarrelSpinDownSnd[4];
	snd_alias_list_t* missileConeSoundAlias;
	snd_alias_list_t* missileConeSoundAliasAtBase;
	float missileConeSoundRadiusAtTop;
	float missileConeSoundRadiusAtBase;
	float missileConeSoundHeight;
	float missileConeSoundOriginOffset;
	float missileConeSoundVolumescaleAtCore;
	float missileConeSoundVolumescaleAtEdge;
	float missileConeSoundVolumescaleCoreSize;
	float missileConeSoundPitchAtTop;
	float missileConeSoundPitchAtBottom;
	float missileConeSoundPitchTopSize;
	float missileConeSoundPitchBottomSize;
	float missileConeSoundCrossfadeTopSize;
	float missileConeSoundCrossfadeBottomSize;
	bool sharedAmmo;
	bool lockonSupported;
	bool requireLockonToFire;
	bool bigExplosion;
	bool noAdsWhenMagEmpty;
	bool avoidDropCleanup;
	bool inheritsPerks;
	bool crosshairColorChange;
	bool bRifleBullet;
	bool armorPiercing;
	bool bBoltAction;
	bool aimDownSight;
	bool bRechamberWhileAds;
	bool bBulletExplosiveDamage;
	bool bCookOffHold;
	bool bClipOnly;
	bool noAmmoPickup;
	bool adsFireOnly;
	bool cancelAutoHolsterWhenEmpty;
	bool disableSwitchToWhenEmpty;
	bool suppressAmmoReserveDisplay;
	bool laserSightDuringNightvision;
	bool markableViewmodel;
	bool noDualWield;
	bool flipKillIcon;
	bool bNoPartialReload;
	bool bSegmentedReload;
	bool blocksProne;
	bool silenced;
	bool isRollingGrenade;
	bool projExplosionEffectForceNormalUp;
	bool bProjImpactExplode;
	bool stickToPlayers;
	bool hasDetonator;
	bool disableFiring;
	bool timedDetonation;
	bool rotate;
	bool holdButtonToThrow;
	bool freezeMovementWhenFiring;
	bool thermalScope;
	bool altModeSameWeapon;
	bool turretBarrelSpinEnabled;
	bool missileConeSoundEnabled;
	bool missileConeSoundPitchshiftEnabled;
	bool missileConeSoundCrossfadeEnabled;
	bool offhandHoldIsCancelable;
};

enum ImpactType
{
	IMPACT_TYPE_NONE = 0x0,
	IMPACT_TYPE_BULLET_SMALL = 0x1,
	IMPACT_TYPE_BULLET_LARGE = 0x2,
	IMPACT_TYPE_BULLET_AP = 0x3,
	IMPACT_TYPE_BULLET_EXPLODE = 0x4,
	IMPACT_TYPE_SHOTGUN = 0x5,
	IMPACT_TYPE_SHOTGUN_EXPLODE = 0x6,
	IMPACT_TYPE_GRENADE_BOUNCE = 0x7,
	IMPACT_TYPE_GRENADE_EXPLODE = 0x8,
	IMPACT_TYPE_ROCKET_EXPLODE = 0x9,
	IMPACT_TYPE_PROJECTILE_DUD = 0xA,
	IMPACT_TYPE_COUNT = 0xB,
};

struct WeaponCompleteDef
{
	const char* szInternalName;
	WeaponDef* weapDef;
	const char* szDisplayName;
	unsigned __int16* hideTags;
	const char** szXAnims;
	float fAdsZoomFov;
	int iAdsTransInTime;
	int iAdsTransOutTime;
	int iClipSize;
	ImpactType impactType;
	int iFireTime;
	weaponIconRatioType_t dpadIconRatio;
	float penetrateMultiplier;
	float fAdsViewKickCenterSpeed;
	float fHipViewKickCenterSpeed;
	const char* szAltWeaponName;
	unsigned int altWeaponIndex;
	int iAltRaiseTime;
	Material* killIcon;
	Material* dpadIcon;
	int fireAnimLength;
	int iFirstRaiseTime;
	int ammoDropStockMax;
	float adsDofStart;
	float adsDofEnd;
	unsigned __int16 accuracyGraphKnotCount[2];
	float(*accuracyGraphKnots[2])[2];
	bool motionTracker;
	bool enhanced;
	bool dpadIconShowsAmmo;
};

struct SndDriverGlobals
{
	const char* name;
};

struct FxImpactEntry
{
	FxEffectDef* nonflesh[31];
	FxEffectDef* flesh[4];
};

struct FxImpactTable
{
	const char* name;
	FxImpactEntry* table;
};

struct RawFile
{
	const char* name;
	int compressedLen;
	int len;
	const char* buffer;
};

struct StringTableCell
{
	const char* string;
	int hash;
};

struct StringTable
{
	const char* name;
	int columnCount;
	int rowCount;
	StringTableCell* values;
};

enum LbColType
{
	LBCOL_TYPE_NUMBER = 0x0,
	LBCOL_TYPE_TIME = 0x1,
	LBCOL_TYPE_LEVELXP = 0x2,
	LBCOL_TYPE_PRESTIGE = 0x3,
	LBCOL_TYPE_BIGNUMBER = 0x4,
	LBCOL_TYPE_PERCENT = 0x5,
	LBCOL_TYPE_COUNT = 0x6,
};

enum LbAggType
{
	LBAGG_TYPE_MIN = 0x0,
	LBAGG_TYPE_MAX = 0x1,
	LBAGG_TYPE_SUM = 0x2,
	LBAGG_TYPE_LAST = 0x3,
	LBAGG_TYPE_COUNT = 0x4,
};

struct LbColumnDef
{
	const char* name;
	int id;
	int propertyId;
	bool hidden;
	const char* statName;
	LbColType type;
	int precision;
	LbAggType agg;
};

struct LeaderboardDef
{
	const char* name;
	int id;
	int columnCount;
	int xpColId;
	int prestigeColId;
	LbColumnDef* columns;
};

struct __declspec(align(4)) StructuredDataEnumEntry
{
	const char* string;
	unsigned __int16 index;
};

struct StructuredDataEnum
{
	int entryCount;
	int reservedEntryCount;
	StructuredDataEnumEntry* entries;
};

enum LookupResultDataType
{
	LOOKUP_RESULT_INT = 0x0,
	LOOKUP_RESULT_BOOL = 0x1,
	LOOKUP_RESULT_STRING = 0x2,
	LOOKUP_RESULT_FLOAT = 0x3,
	LOOKUP_RESULT_SHORT = 0x4,
};

enum LookupState
{
	LOOKUP_IN_PROGRESS = 0x0,
	LOOKUP_FINISHED = 0x1,
	LOOKUP_ERROR = 0x2,
};

enum LookupError
{
	LOOKUP_ERROR_NONE = 0x0,
	LOOKUP_ERROR_WRONG_DATA_TYPE = 0x1,
	LOOKUP_ERROR_INDEX_OUTSIDE_BOUNDS = 0x2,
	LOOKUP_ERROR_INVALID_STRUCT_PROPERTY = 0x3,
	LOOKUP_ERROR_INVALID_ENUM_VALUE = 0x4,
	LOOKUP_ERROR_COUNT = 0x5,
};

enum StructuredDataTypeCategory
{
	DATA_INT = 0x0,
	DATA_BYTE = 0x1,
	DATA_BOOL = 0x2,
	DATA_STRING = 0x3,
	DATA_ENUM = 0x4,
	DATA_STRUCT = 0x5,
	DATA_INDEXED_ARRAY = 0x6,
	DATA_ENUM_ARRAY = 0x7,
	DATA_FLOAT = 0x8,
	DATA_SHORT = 0x9,
	DATA_COUNT = 0xA,
};

union StructuredDataTypeUnion
{
	unsigned int stringDataLength;
	int enumIndex;
	int structIndex;
	int indexedArrayIndex;
	int enumedArrayIndex;
};

struct StructuredDataType
{
	StructuredDataTypeCategory type;
	StructuredDataTypeUnion u;
};

struct StructuredDataStructProperty
{
	const char* name;
	StructuredDataType type;
	unsigned int offset;
};

struct StructuredDataStruct
{
	int propertyCount;
	StructuredDataStructProperty* properties;
	int size;
	unsigned int bitOffset;
};

struct StructuredDataIndexedArray
{
	int arraySize;
	StructuredDataType elementType;
	unsigned int elementSize;
};

struct StructuredDataEnumedArray
{
	int enumIndex;
	StructuredDataType elementType;
	unsigned int elementSize;
};

struct StructuredDataDef
{
	int version;
	unsigned int formatChecksum;
	int enumCount;
	StructuredDataEnum* enums;
	int structCount;
	StructuredDataStruct* structs;
	int indexedArrayCount;
	StructuredDataIndexedArray* indexedArrays;
	int enumedArrayCount;
	StructuredDataEnumedArray* enumedArrays;
	StructuredDataType rootType;
	unsigned int size;
};


struct StructuredDataLookup
{
	StructuredDataDef* def;
	StructuredDataType* type;
	unsigned int offset;
	LookupError error;
};

struct StructuredDataDefSet
{
	const char* name;
	unsigned int defCount;
	StructuredDataDef* defs;
};

struct StructuredDataBuffer
{
	char* data;
	unsigned int size;
};

enum VehicleType
{
	VEH_WHEELS_4 = 0x0,
	VEH_TANK = 0x1,
	VEH_PLANE = 0x2,
	VEH_BOAT = 0x3,
	VEH_ARTILLERY = 0x4,
	VEH_HELICOPTER = 0x5,
	VEH_SNOWMOBILE = 0x6,
	VEH_TYPE_COUNT = 0x7,
};

enum VehicleAxleType
{
	VEH_AXLE_FRONT = 0x0,
	VEH_AXLE_REAR = 0x1,
	VEH_AXLE_ALL = 0x2,
	VEH_AXLE_COUNT = 0x3,
};

struct VehiclePhysDef
{
	int physicsEnabled;
	const char* physPresetName;
	PhysPreset* physPreset;
	const char* accelGraphName;
	VehicleAxleType steeringAxle;
	VehicleAxleType powerAxle;
	VehicleAxleType brakingAxle;
	float topSpeed;
	float reverseSpeed;
	float maxVelocity;
	float maxPitch;
	float maxRoll;
	float suspensionTravelFront;
	float suspensionTravelRear;
	float suspensionStrengthFront;
	float suspensionDampingFront;
	float suspensionStrengthRear;
	float suspensionDampingRear;
	float frictionBraking;
	float frictionCoasting;
	float frictionTopSpeed;
	float frictionSide;
	float frictionSideRear;
	float velocityDependentSlip;
	float rollStability;
	float rollResistance;
	float pitchResistance;
	float yawResistance;
	float uprightStrengthPitch;
	float uprightStrengthRoll;
	float targetAirPitch;
	float airYawTorque;
	float airPitchTorque;
	float minimumMomentumForCollision;
	float collisionLaunchForceScale;
	float wreckedMassScale;
	float wreckedBodyFriction;
	float minimumJoltForNotify;
	float slipThresholdFront;
	float slipThresholdRear;
	float slipFricScaleFront;
	float slipFricScaleRear;
	float slipFricRateFront;
	float slipFricRateRear;
	float slipYawTorque;
};

struct VehicleDef
{
	const char* name;
	VehicleType type;
	const char* useHintString;
	int health;
	int quadBarrel;
	float texScrollScale;
	float topSpeed;
	float accel;
	float rotRate;
	float rotAccel;
	float maxBodyPitch;
	float maxBodyRoll;
	float fakeBodyAccelPitch;
	float fakeBodyAccelRoll;
	float fakeBodyVelPitch;
	float fakeBodyVelRoll;
	float fakeBodySideVelPitch;
	float fakeBodyPitchStrength;
	float fakeBodyRollStrength;
	float fakeBodyPitchDampening;
	float fakeBodyRollDampening;
	float fakeBodyBoatRockingAmplitude;
	float fakeBodyBoatRockingPeriod;
	float fakeBodyBoatRockingRotationPeriod;
	float fakeBodyBoatRockingFadeoutSpeed;
	float boatBouncingMinForce;
	float boatBouncingMaxForce;
	float boatBouncingRate;
	float boatBouncingFadeinSpeed;
	float boatBouncingFadeoutSteeringAngle;
	float collisionDamage;
	float collisionSpeed;
	float killcamOffset[3];
	int playerProtected;
	int bulletDamage;
	int armorPiercingDamage;
	int grenadeDamage;
	int projectileDamage;
	int projectileSplashDamage;
	int heavyExplosiveDamage;
	VehiclePhysDef vehPhysDef;
	float boostDuration;
	float boostRechargeTime;
	float boostAcceleration;
	float suspensionTravel;
	float maxSteeringAngle;
	float steeringLerp;
	float minSteeringScale;
	float minSteeringSpeed;
	int camLookEnabled;
	float camLerp;
	float camPitchInfluence;
	float camRollInfluence;
	float camFovIncrease;
	float camFovOffset;
	float camFovSpeed;
	const char* turretWeaponName;
	WeaponCompleteDef* turretWeapon;
	float turretHorizSpanLeft;
	float turretHorizSpanRight;
	float turretVertSpanUp;
	float turretVertSpanDown;
	float turretRotRate;
	snd_alias_list_t* turretSpinSnd;
	snd_alias_list_t* turretStopSnd;
	int trophyEnabled;
	float trophyRadius;
	float trophyInactiveRadius;
	int trophyAmmoCount;
	float trophyReloadTime;
	unsigned __int16 trophyTags[4];
	Material* compassFriendlyIcon;
	Material* compassEnemyIcon;
	int compassIconWidth;
	int compassIconHeight;
	snd_alias_list_t* idleLowSnd;
	snd_alias_list_t* idleHighSnd;
	snd_alias_list_t* engineLowSnd;
	snd_alias_list_t* engineHighSnd;
	float engineSndSpeed;
	snd_alias_list_t* engineStartUpSnd;
	int engineStartUpLength;
	snd_alias_list_t* engineShutdownSnd;
	snd_alias_list_t* engineIdleSnd;
	snd_alias_list_t* engineSustainSnd;
	snd_alias_list_t* engineRampUpSnd;
	int engineRampUpLength;
	snd_alias_list_t* engineRampDownSnd;
	int engineRampDownLength;
	snd_alias_list_t* suspensionSoftSnd;
	float suspensionSoftCompression;
	snd_alias_list_t* suspensionHardSnd;
	float suspensionHardCompression;
	snd_alias_list_t* collisionSnd;
	float collisionBlendSpeed;
	snd_alias_list_t* speedSnd;
	float speedSndBlendSpeed;
	const char* surfaceSndPrefix;
	snd_alias_list_t* surfaceSnds[31];
	float surfaceSndBlendSpeed;
	float slideVolume;
	float slideBlendSpeed;
	float inAirPitch;
};

struct AddonMapEnts
{
	const char* name;
	char* entityString;
	int numEntityChars;
	MapTriggers trigger;
};

union XAssetHeader
{
	void* data;
	PhysPreset* physPreset;
	PhysCollmap* physCollmap;
	XAnimParts* parts;
	XModelSurfs* modelSurfs;
	XModel* model;
	Material* material;
	MaterialPixelShader* pixelShader;
	MaterialVertexShader* vertexShader;
	MaterialVertexDeclaration* vertexDecl;
	MaterialTechniqueSet* techniqueSet;
	GfxImage* image;
	snd_alias_list_t* sound;
	SndCurve* sndCurve;
	LoadedSound* loadSnd;
	clipMap_t* clipMap;
	ComWorld* comWorld;
	GameWorldSp* gameWorldSp;
	GameWorldMp* gameWorldMp;
	MapEnts* mapEnts;
	FxWorld* fxWorld;
	GfxWorld* gfxWorld;
	GfxLightDef* lightDef;
	Font_s* font;
	MenuList* menuList;
	menuDef_t* menu;
	LocalizeEntry* localize;
	WeaponCompleteDef* weapon;
	SndDriverGlobals* sndDriverGlobals;
	FxEffectDef* fx;
	FxImpactTable* impactFx;
	RawFile* rawfile;
	StringTable* stringTable;
	LeaderboardDef* leaderboardDef;
	StructuredDataDefSet* structuredDataDefSet;
	TracerDef* tracerDef;
	VehicleDef* vehDef;
	AddonMapEnts* addonMapEnts;
};

struct weaponParms
{
	float forward[3];
	float right[3];
	float up[3];
	float muzzleTrace[3];
	float gunForward[3];
	unsigned int weaponIndex;
	const WeaponDef* weapDef;
	const WeaponCompleteDef* weapCompleteDef;
};

struct XAsset
{
	XAssetType type;
	XAssetHeader header;
};

struct XBlock
{
	char* data;
	unsigned int size;
};

struct XAssetEntry
{
	XAsset asset;
	char zoneIndex;
	volatile char inuseMask;
	bool printedMissingAsset;
	unsigned __int16 nextHash;
	unsigned __int16 nextOverride;
};

enum XFileLanguage : unsigned char
{
	XLANG_NONE = 0x00,
	XLANG_ENGLISH = 0x01,
	XLANG_FRENCH = 0x02,
	XLANG_GERMAN = 0x03,
	XLANG_ITALIAN = 0x04,
	XLANG_SPANISH = 0x05,
	XLANG_BRITISH = 0x06,
	XLANG_RUSSIAN = 0x07,
	XLANG_POLISH = 0x08,
	XLANG_KOREAN = 0x09,
	XLANG_TAIWANESE = 0x0A,
	XLANG_JAPANESE = 0x0B,
	XLANG_CHINESE = 0x0C,
	XLANG_THAI = 0x0D,
	XLANG_LEET = 0x0E, // Wat?
	XLANG_CZECH = 0x0F,
};

#pragma pack(push, 1)
struct XFileHeader
{
	unsigned __int64 magic;
	unsigned int version;
	XFileLanguage language;
	DWORD highDateTime;
	DWORD lowDateTime;
};
#pragma pack(pop)

enum XFILE_BLOCK_TYPES
{
	XFILE_BLOCK_TEMP = 0x0,
	XFILE_BLOCK_PHYSICAL = 0x1,
	XFILE_BLOCK_RUNTIME = 0x2,
	XFILE_BLOCK_VIRTUAL = 0x3,
	XFILE_BLOCK_LARGE = 0x4,

	// Those are probably incorrect
	XFILE_BLOCK_CALLBACK,
	XFILE_BLOCK_VERTEX,
	XFILE_BLOCK_INDEX,

	MAX_XFILE_COUNT,

	XFILE_BLOCK_INVALID = -1
};

struct XFile
{
	unsigned int size;
	unsigned int externalSize;
	unsigned int blockSize[MAX_XFILE_COUNT];
};

struct ScriptStringList
{
	int count;
	const char** strings;
};

struct XAssetList
{
	ScriptStringList stringList;
	int assetCount;
	XAsset* assets;
};

struct ZoneHeader
{
	XFile xFile;
	XAssetList assetList;
};

struct XZoneMemory
{
	XBlock blocks[MAX_XFILE_COUNT];
	char* lockedVertexData;
	char* lockedIndexData;
	void* vertexBuffer;
	void* indexBuffer;
};

struct XZone
{
	int unk;
	char name[64];
	int flags;
	int allocType;
	XZoneMemory mem;
	int fileSize;
	char modZone;
};


// ---- x64 layout guards, validated against real zone bytes ----
static_assert(sizeof(XFile) == 0x28, "XFile must be 0x28 so stringCount lands at +40");
static_assert(offsetof(XAssetList, assetCount) == 16 && offsetof(XAssetList, assets) == 24, "XAssetList");
static_assert(sizeof(Material) == 0x78 && offsetof(Material, textureTable) == 0x60, "Material");
static_assert(sizeof(MaterialTextureDef) == 0x10, "MaterialTextureDef");
static_assert(sizeof(GfxImage) == 0x28, "GfxImage");
static_assert(sizeof(MapEnts) == 0x58 && offsetof(MapEnts, entityString) == 8 &&
              offsetof(MapEnts, numEntityChars) == 16, "MapEnts");
static_assert(sizeof(cplane_s) == 0x14 && sizeof(cbrush_t) == 0x30 && sizeof(cbrushside_t) == 0x10 &&
              sizeof(cmodel_t) == 0x44 && sizeof(cLeaf_t) == 0x28 && sizeof(cNode_t) == 0x10 &&
              sizeof(cStaticModel_s) == 0x50, "clip collision sub-structs");
// Load_clipMap_t (0x1242b0) reads 0x200 bytes; every offset below is the one it dereferences.
static_assert(sizeof(clipMap_t) == 0x200, "clipMap_t (Load_clipMap_t reads 0x200)");
static_assert(offsetof(clipMap_t, planes) == 16 && offsetof(clipMap_t, brushsides) == 64 &&
              offsetof(clipMap_t, leafbrushNodes) == 128 && offsetof(clipMap_t, leafbrushes) == 144 &&
              offsetof(clipMap_t, triEdgeIsWalkable) == 200 && offsetof(clipMap_t, cmodels) == 264 &&
              offsetof(clipMap_t, numBrushes) == 272 && offsetof(clipMap_t, brushes) == 280 &&
              offsetof(clipMap_t, mapEnts) == 304 && offsetof(clipMap_t, smodelNodes) == 320 &&
              offsetof(clipMap_t, dynEntCount) == 328 && offsetof(clipMap_t, dynEntCollList) == 384 &&
              offsetof(clipMap_t, checksum) == 400, "clipMap_t field offsets (Load_clipMap_t)");
// Array element sizes Load_clipMap_t streams (count * size).
static_assert(sizeof(ClipMaterial) == 16, "ClipMaterial (Load_clipMap_t element size)");
static_assert(sizeof(cLeafBrushNode_s) == 24, "cLeafBrushNode_s (Load_clipMap_t element size)");
static_assert(sizeof(Bounds) == 24, "Bounds (Load_clipMap_t element size)");
static_assert(sizeof(CollisionBorder) == 28, "CollisionBorder (Load_clipMap_t element size)");
static_assert(sizeof(CollisionPartition) == 16, "CollisionPartition (Load_clipMap_t element size)");
static_assert(sizeof(CollisionAabbTree) == 32, "CollisionAabbTree (Load_clipMap_t element size)");
static_assert(sizeof(SModelAabbNode) == 28, "SModelAabbNode (Load_clipMap_t element size)");
static_assert(sizeof(DynEntityPose) == 32, "DynEntityPose (Load_clipMap_t element size)");
static_assert(sizeof(DynEntityClient) == 16, "DynEntityClient (Load_clipMap_t element size)");
static_assert(sizeof(DynEntityColl) == 20, "DynEntityColl (Load_clipMap_t element size)");
static_assert(sizeof(ComWorld) == 0x18 && sizeof(GameWorldMp) == 0x10 && sizeof(FxWorld) == 0xb0, "worlds");
static_assert(sizeof(GfxWorld) == 0x3b0, "GfxWorld");
