#pragma once
#include <cstddef>
#include <string>
#include "document.h"

// Imports a compiled map from a fastfile into an editable Document:
//   - entities from MapEnts (the entity lump)
//   - brushes rebuilt from clipMap_t (bounds + side planes), all into worldspawn
//   - the render geometry from GfxWorld (every surface's triangles, with its material name) as a
//     read-only RefMesh, so terrain, patches and detail meshes that aren't brushes still show
//
// Zones iw4-radiant compiled load fully through the loader simulator. Stock map zones contain
// asset types the simulator doesn't walk (models, sounds, fx...), so the world assets are found
// in the zone by their structure and loaded on their own (zone::simulatePartial), anchored so
// back-references resolve: GfxWorld at its plane array (whose position clipMap references), and
// clipMap at its brushside array (whose position its brushes reference).
//
// Spec Ops "overlay" zones (so_intel_boneyard.ff etc.) hold no geometry, only a standalone
// maps/<mission>.mapents with the entities the mission adds; they import as the base map zone
// beside them (found from the file name) plus those entities.
//
// Both unsigned (IWffu100) and signed (IWffs100) zones import: the install's singleplayer and
// Spec Ops maps and the retail multiplayer maps. Signed zones decompress to the same payload
// (fastfile.cpp strips the authentication blocks) and load through the same stock-zone path.
namespace mapimport {

struct ImportStats {
    size_t entities = 0;
    size_t brushes = 0, brushesSkipped = 0;
    size_t surfaces = 0, surfacesTextured = 0, triangles = 0;
    bool partial = false;  // stock zone, loaded per asset
    std::string notes;     // human-readable details (what was skipped and why)
};

bool importFastfile(const std::string& ffPath, Document& doc, ImportStats& stats, std::string& error);

// Parses an entity lump ({ "key" "value" ... } blocks) into entities; the first becomes worldspawn.
bool parseEntityString(const std::string& text, std::vector<Entity>& out, std::string& error);

}  // namespace mapimport
