// The Asset Manager window (View > Asset Manager, F4): the map's zone_source folder by kind with
// details, cross-references, problems, a weapon-file editor and file operations. The indexing,
// analysis and file operations are in assetmanager.{h,cpp}; this file is only the UI.
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include "assetpanel.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "zone/zonebuild.h"

namespace fs = std::filesystem;
using assetman::Asset;
using assetman::Issue;
using assetman::Kind;

namespace {

bool containsNoCase(const std::string& text, const char* needle) {
    if (!*needle) return true;
    auto it = std::search(text.begin(), text.end(), needle, needle + std::strlen(needle),
                          [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    return it != text.end();
}

bool iequals(const std::string& a, const std::string& b) { return zsrc::lower(a) == zsrc::lower(b); }

// ImGui text input on a std::string (grows as you type).
int resizeCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}
bool inputString(const char* id, std::string& s, ImGuiInputTextFlags flags = 0) {
    return ImGui::InputText(id, s.data(), s.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, resizeCallback, &s);
}
bool inputMultiline(const char* id, std::string& s, const ImVec2& size) {
    return ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, resizeCallback, &s);
}

ImVec4 levelColor(Issue::Level l) {
    switch (l) {
        case Issue::Level::Error: return {1.0f, 0.38f, 0.33f, 1};
        case Issue::Level::Warning: return {1.0f, 0.78f, 0.30f, 1};
        default: return {0.55f, 0.70f, 0.90f, 1};
    }
}
const char* levelName(Issue::Level l) {
    return l == Issue::Level::Error ? "error" : l == Issue::Level::Warning ? "warning" : "info";
}

std::string humanBytes(uintmax_t b) {
    char buf[32];
    if (b >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024.0));
    else if (b >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
    else std::snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(b));
    return buf;
}

void showInExplorer(const fs::path& p) {
    const std::string args = "/select,\"" + p.string() + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void openExternally(const fs::path& p) {
    // Weapon files have no extension: open those (and anything without a handler) in Notepad.
    if (p.extension().empty() ||
        reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", p.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteA(nullptr, "open", "notepad.exe", ("\"" + p.string() + "\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// Moves files to the Recycle Bin (undoable from Explorer).
bool recycle(const std::vector<fs::path>& files) {
    std::wstring list;
    for (const fs::path& f : files) list += f.wstring() + L'\0';
    list += L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

weapondef::RefKind refFor(Kind k) {
    switch (k) {
        case Kind::XModel: return weapondef::RefKind::Model;
        case Kind::XAnim: return weapondef::RefKind::Anim;
        case Kind::Effect: return weapondef::RefKind::Fx;
        default: return weapondef::RefKind::None;
    }
}

// Gameplay keys worth having on one page, with what they mean.
struct StatKey {
    const char* key;
    const char* label;
    const char* help;
};
const StatKey kStatKeys[] = {
    {"displayName", "Display name", "Localized string or plain text shown in the HUD"},
    {"weaponType", "Weapon type", "bullet, grenade, projectile, riotshield"},
    {"weaponClass", "Class", "rifle, mg, smg, spread, pistol, grenade, rocketlauncher, ..."},
    {"inventoryType", "Inventory", "primary, offhand, item, altmode, exclusive"},
    {"fireType", "Fire type", "Full Auto, Single Shot, 2-Round Burst, ..."},
    {"playerAnimType", "Player anim type", "third-person animation set (autorifle, pistol, smg, ...)"},
    {"damage", "Damage", "damage to AI/players at close range"},
    {"minDamage", "Min damage", "damage at and beyond minDamageRange"},
    {"playerDamage", "Player damage (SP)", "damage AI deal to the player with it (SP), percent"},
    {"maxDamageRange", "Full-damage range", "units; damage falls off from here..."},
    {"minDamageRange", "Min-damage range", "...to minDamage here"},
    {"meleeDamage", "Melee damage", ""},
    {"fireTime", "Fire time (s)", "seconds between shots; RPM = 60 / fireTime"},
    {"clipSize", "Magazine", "rounds per magazine"},
    {"startAmmo", "Start ammo", "reserve when picked up / given"},
    {"maxAmmo", "Max ammo", "reserve cap"},
    {"shotCount", "Pellets", "bullets per shot (shotguns)"},
    {"reloadTime", "Reload (s)", ""},
    {"reloadEmptyTime", "Reload empty (s)", ""},
    {"reloadAddTime", "Reload add (s)", "when the new rounds count (partial reload)"},
    {"raiseTime", "Raise (s)", ""},
    {"dropTime", "Drop (s)", ""},
    {"firstRaiseTime", "First raise (s)", ""},
    {"adsZoomFov", "ADS FOV", "field of view while aiming"},
    {"adsTransInTime", "ADS in (s)", ""},
    {"adsTransOutTime", "ADS out (s)", ""},
    {"hipSpreadStandMin", "Hip spread min", "degrees"},
    {"hipSpreadMax", "Hip spread max", "degrees"},
    {"moveSpeedScale", "Move speed", "1 = normal"},
    {"adsMoveSpeedScale", "ADS move speed", "1 = normal"},
    {"penetrateType", "Penetration", "none, small, medium, large"},
};

// The enum names a key accepts (nullptr = free text).
const char* const* enumNamesFor(const std::string& key) {
    for (const weapondef::Field& f : weapondef::fields())
        if (f.kind == weapondef::Kind::Enum && iequals(key, f.key)) return f.enumNames;
    return nullptr;
}

}  // namespace

// ---- state ----

void AssetPanel::setRoot(const std::string& zoneSourceDir, const char* noRootMessage) {
    if (noRootMessage) noRoot = noRootMessage;
    if (zoneSourceDir == rootDir && (!rootDir.empty() || index.root.empty())) return;
    // A different folder: unsaved weapon edits belong to the old one.
    if (weaponDirty) saveOrDiscard();
    weaponDirty = false;
    weaponEditName.clear();
    selName.clear();
    showProblems = false;
    rootDir = zoneSourceDir;
    analysis.reset();
    rescan();
}

bool AssetPanel::saveOrDiscard() {
    if (!weaponDirty) return true;
    const int r = MessageBoxA(host.window(), ("Save changes to weapon " + weaponEditName + "?").c_str(), "Asset Manager",
                              MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES)
        if (const Asset* w = index.find(Kind::Weapon, weaponEditName)) {
            std::string err;
            if (!assetman::saveWeapon(*w, weaponEdit, err)) {
                MessageBoxA(host.window(), err.c_str(), "Save failed", MB_OK | MB_ICONERROR);
                return false;
            }
            host.log("Saved weapon " + weaponEditName);
        }
    weaponDirty = false;
    weaponEditName.clear();
    return true;
}

void AssetPanel::rescan() {
    index.scan(rootDir);
    index.root = rootDir;
    if (job.valid()) {  // one at a time: run again when this one finishes
        rescanPending = true;
        return;
    }
    job = std::async(std::launch::async, [idx = index] { return std::make_shared<assetman::Analysis>(assetman::analyse(idx)); });
}

void AssetPanel::poll() {
    if (job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        analysis = job.get();
        if (rescanPending) {
            rescanPending = false;
            job = std::async(std::launch::async, [idx = index] { return std::make_shared<assetman::Analysis>(assetman::analyse(idx)); });
        }
    }
}

void AssetPanel::afterChange(bool previewToo) {
    rescan();
    if (previewToo) host.reloadPreview();
}

bool AssetPanel::selectAsset(Kind k, const std::string& name) {
    if (k == selKind && name == selName && !showProblems) return true;
    if (weaponDirty && weaponEditName != name && !saveOrDiscard()) return false;
    selKind = k;
    selName = name;
    showProblems = false;
    return true;
}

void AssetPanel::select(const std::string& name) {
    if (job.valid()) job.wait();
    poll();
    if (job.valid()) job.wait();  // a pending rescan
    poll();
    for (const Asset& a : index.assets)
        if (a.name == name) {
            selectAsset(a.kind, a.name);
            break;
        }
}

// ---- window ----

void AssetPanel::draw(const char* title, bool* open) {
    ImGui::SetNextWindowSize({1040, 660}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }
    if (rootDir.empty()) {
        ImGui::TextWrapped("%s", noRoot.c_str());
        ImGui::End();
        return;
    }
    // Toolbar.
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##afilter", "filter by name", filter, sizeof filter);
    ImGui::SameLine();
    if (ImGui::Button("Import...")) importFiles();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copy files into zone_source, sorted by type:\n"
                          "  .xmodel_export -> xmodels (with its _lodN files and the textures beside it -> images)\n"
                          "  .xanim_export -> xanims, .png/.tga -> images, .efx -> fx, .gsc -> scripts,\n"
                          "  files without an extension -> weapons (IW4 weapon files)");
    ImGui::SameLine();
    if (ImGui::Button("New weapon...")) {
        popup = Popup::NewWeapon;
        popupName[0] = 0;
        popupError.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) afterChange(true);
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
        std::error_code ec;
        fs::create_directories(rootDir, ec);
        ShellExecuteA(host.window(), "open", rootDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (job.valid()) ImGui::TextDisabled("analysing...");
    else if (analysis) {
        size_t e = 0, w = 0;
        for (const Issue& i : analysis->issues) e += i.level == Issue::Level::Error, w += i.level == Issue::Level::Warning;
        ImGui::TextDisabled("%zu assets  |  %zu errors, %zu warnings", index.assets.size(), e, w);
    }

    if (ImGui::BeginTable("##assetsplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthFixed, 280);
        ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("##atree")) drawTree();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("##adetails")) {
            if (showProblems) drawProblems();
            else drawDetails();
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    drawPopups();
    ImGui::End();
}

void AssetPanel::drawTree() {
    const assetman::Analysis* an = analysis.get();
    {
        size_t e = 0, w = 0;
        if (an)
            for (const Issue& i : an->issues) e += i.level == Issue::Level::Error, w += i.level == Issue::Level::Warning;
        const std::string label = "Problems (" + std::to_string(e) + " errors, " + std::to_string(w) + " warnings)";
        if (e || w) ImGui::PushStyleColor(ImGuiCol_Text, levelColor(e ? Issue::Level::Error : Issue::Level::Warning));
        if (ImGui::Selectable(label.c_str(), showProblems) && selectAsset(selKind, selName)) showProblems = true;
        if (e || w) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    const Asset* pending = nullptr;  // context-menu action target (acted on after the loop)
    enum { None, Rename, Duplicate, Delete } action = None;
    for (int ki = 0; ki < assetman::kKindCount; ki++) {
        const Kind k = static_cast<Kind>(ki);
        const size_t n = index.count(k);
        if (!n) continue;
        const std::string head = std::string(assetman::kindName(k)) + " (" + std::to_string(n) + ")";
        const bool open = ImGui::TreeNodeEx(assetman::kindName(k),
                                            k == Kind::Weapon || k == Kind::XModel || k == Kind::XAnim ? ImGuiTreeNodeFlags_DefaultOpen : 0,
                                            "%s", head.c_str());
        if (!open) continue;
        for (const Asset& a : index.assets) {
            if (a.kind != k || !containsNoCase(a.name, filter)) continue;
            ImGui::PushID(&a);
            const size_t errs = an ? an->issuesFor(k, a.name, Issue::Level::Error) : 0;
            const size_t warns = an ? an->issuesFor(k, a.name, Issue::Level::Warning) : 0;
            const bool sel = !showProblems && selKind == k && selName == a.name;
            if (errs || warns) ImGui::PushStyleColor(ImGuiCol_Text, levelColor(errs ? Issue::Level::Error : Issue::Level::Warning));
            const std::string label = (k == Kind::Weapon && weaponDirty && weaponEditName == a.name ? a.name + " *" : a.name);
            if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectAsset(k, a.name);
                if (k == Kind::XModel && host.isEditor() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) host.placeModel(a.name);
            }
            if (errs || warns) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && (errs || warns)) ImGui::SetTooltip("%zu error(s), %zu warning(s)", errs, warns);
            if (k == Kind::XModel && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("IW4R_XMODEL", a.name.c_str(), a.name.size() + 1);
                ImGui::Text("%s", a.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (k == Kind::XModel && host.isEditor() && ImGui::MenuItem("Place at camera")) host.placeModel(a.name);
                if (ImGui::MenuItem("Rename...")) pending = &a, action = Rename;
                if (k == Kind::Weapon && ImGui::MenuItem("Duplicate...")) pending = &a, action = Duplicate;
                if (ImGui::MenuItem("Show in Explorer")) showInExplorer(a.path);
                if (ImGui::MenuItem("Open in external editor")) openExternally(a.path);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete...")) pending = &a, action = Delete;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (pending) {
        const Asset a = *pending;  // a copy: the index may be rebuilt below
        if (selectAsset(a.kind, a.name)) {
            if (action == Delete) deleteAsset(a);
            else {
                popup = action == Rename ? Popup::Rename : Popup::Duplicate;
                std::string start = a.name;
                if (a.kind == Kind::Image || a.kind == Kind::Effect || a.kind == Kind::Script)
                    start = fs::path(a.name).stem().string();
                if (action == Duplicate) start += "_copy";
                strncpy_s(popupName, start.c_str(), _TRUNCATE);
                popupError.clear();
            }
        }
    }
}

void AssetPanel::drawProblems() {
    const assetman::Analysis* an = analysis.get();
    if (!an) {
        ImGui::TextDisabled("analysing...");
        return;
    }
    ImGui::Checkbox("Show info", &showInfo);
    ImGui::SameLine();
    ImGui::TextDisabled("(click an asset to open it)");
    if (!ImGui::BeginTable("##problems", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("asset", ImGuiTableColumnFlags_WidthFixed, 230);
    ImGui::TableSetupColumn("problem", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    int row = 0;
    Kind goKind{};
    std::string goName;
    for (const Issue& i : an->issues) {
        if (i.level == Issue::Level::Info && !showInfo) continue;
        ImGui::PushID(row++);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(levelColor(i.level), "%s", levelName(i.level));
        ImGui::TableNextColumn();
        const std::string label = std::string(assetman::kindName(i.kind)).substr(0, 1) + "  " + i.asset;
        if (ImGui::Selectable(label.c_str())) goKind = i.kind, goName = i.asset;
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", i.text.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!goName.empty()) selectAsset(goKind, goName);
}

void AssetPanel::drawDetails() {
    const Asset* a = selName.empty() ? nullptr : index.find(selKind, selName);
    if (!a) {
        ImGui::TextDisabled("Select an asset on the left. Right-click one for rename / duplicate / delete;");
        if (host.isEditor()) ImGui::TextDisabled("drag an xmodel into a view (or double-click it) to place it.");
        if (index.assets.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("This map's zone_source is empty or missing (%s). Use Import... to add exported models, "
                               "anims, textures and weapon files, or New weapon... to start a weapon.",
                               rootDir.c_str());
        }
        return;
    }
    const assetman::Analysis* an = analysis.get();
    // Header.
    ImGui::Text("%s", a->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s  |  %s", assetman::kindName(a->kind), humanBytes(a->bytes).c_str());
    ImGui::TextDisabled("%s", a->path.string().c_str());
    if (ImGui::Button("Show in Explorer")) showInExplorer(a->path);
    ImGui::SameLine();
    if (ImGui::Button("Open externally")) openExternally(a->path);
    ImGui::SameLine();
    if (ImGui::Button("Rename...")) {
        popup = Popup::Rename;
        std::string start = a->kind == Kind::Image || a->kind == Kind::Effect || a->kind == Kind::Script ? fs::path(a->name).stem().string() : a->name;
        strncpy_s(popupName, start.c_str(), _TRUNCATE);
        popupError.clear();
    }
    if (a->kind == Kind::Weapon) {
        ImGui::SameLine();
        if (ImGui::Button("Duplicate...")) {
            popup = Popup::Duplicate;
            strncpy_s(popupName, (a->name + "_copy").c_str(), _TRUNCATE);
            popupError.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete...")) {
        const Asset copy = *a;
        deleteAsset(copy);
        return;
    }
    // Problems for this asset.
    if (an)
        for (const Issue& i : an->issues)
            if (i.kind == a->kind && i.asset == a->name) {
                ImGui::PushStyleColor(ImGuiCol_Text, levelColor(i.level));
                ImGui::Bullet();
                ImGui::TextWrapped("%s", i.text.c_str());
                ImGui::PopStyleColor();
            }
    ImGui::Separator();

    // Used by (weapons/models from the analysis; map entities for models).
    auto usedBy = [&] {
        std::vector<std::string> users;
        if (an)
            if (auto it = an->usedBy.find({a->kind, a->name}); it != an->usedBy.end()) users = it->second;
        if (a->kind == Kind::XModel) {
            const int ents = host.entitiesUsingModel(a->name);
            if (ents) users.push_back(std::to_string(ents) + " entit" + (ents == 1 ? "y" : "ies") + " in this map");
        }
        if (ImGui::CollapsingHeader(("Used by (" + std::to_string(users.size()) + ")").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (users.empty()) ImGui::TextDisabled("nothing references it");
            for (const std::string& u : users) ImGui::BulletText("%s", u.c_str());
        }
    };

    switch (a->kind) {
        case Kind::Weapon: drawWeaponEditor(*a); break;
        case Kind::XModel: {
            const assetman::ModelInfo* mi = nullptr;
            if (an)
                if (auto it = an->models.find(a->name); it != an->models.end()) mi = &it->second;
            const float spin = static_cast<float>(std::fmod(ImGui::GetTime() * 40.0, 360.0));
            if (ID3D11ShaderResourceView* thumb = host.modelThumbnail(a->name, 200, 35.0f + spin)) {
                ImGui::Image((ImTextureID)(intptr_t)thumb, {200, 200});
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            if (host.isEditor()) {
                if (ImGui::Button("Place at camera")) host.placeModel(a->name);
                ImGui::BeginDisabled(host.selectedEntityCount() == 0);
                if (ImGui::Button("Use on selected entities")) host.useModelOnSelection(a->name);
                ImGui::EndDisabled();
            }
            if (!mi) ImGui::TextDisabled("analysing...");
            else if (mi->ok) {
                ImGui::Text("%zu triangles, %zu vertices", mi->tris, mi->verts);
                ImGui::Text("%zu bones, %zu materials", mi->bones.size(), mi->materials.size());
                ImGui::Text("size %.1f x %.1f x %.1f", mi->maxs[0] - mi->mins[0], mi->maxs[1] - mi->mins[1], mi->maxs[2] - mi->mins[2]);
                ImGui::Text("LODs: %zu extra", a->lods.size());
                for (const fs::path& l : a->lods) ImGui::BulletText("%s", l.filename().string().c_str());
            }
            ImGui::EndGroup();
            if (mi && mi->ok) {
                if (ImGui::CollapsingHeader(("Materials (" + std::to_string(mi->materials.size()) + ")").c_str(),
                                            mi->materials.size() <= 8 ? ImGuiTreeNodeFlags_DefaultOpen : 0) &&
                    ImGui::BeginTable("##mats", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("material");
                    ImGui::TableSetupColumn("exported texture");
                    ImGui::TableSetupColumn("embedded file");
                    ImGui::TableHeadersRow();
                    for (const auto& m : mi->materials) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(m.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(m.texture.empty() ? "-" : fs::path(m.texture).filename().string().c_str());
                        ImGui::TableNextColumn();
                        if (m.image.empty()) ImGui::TextColored(levelColor(Issue::Level::Warning), "not found");
                        else {
                            const std::string rel = m.image.parent_path().filename().string() + "/" + m.image.filename().string();
                            if (ImGui::Selectable(rel.c_str())) selectAsset(Kind::Image, zsrc::lower(m.image.filename().string()));
                        }
                    }
                    ImGui::EndTable();
                }
                if (ImGui::CollapsingHeader(("Bones (" + std::to_string(mi->bones.size()) + ")").c_str())) {
                    ImGui::Indent();
                    for (size_t b = 0; b < mi->bones.size(); b++) ImGui::Text("%3zu  %s", b, mi->bones[b].c_str());
                    ImGui::Unindent();
                }
            }
            usedBy();
            break;
        }
        case Kind::XAnim: {
            const assetman::AnimInfo* ai = nullptr;
            if (an)
                if (auto it = an->anims.find(a->name); it != an->anims.end()) ai = &it->second;
            if (!ai) {
                ImGui::TextDisabled("analysing...");
                break;
            }
            if (ai->ok) {
                ImGui::Text("%d frames at %.0f fps = %.2f s%s", ai->frames, ai->framerate,
                            ai->framerate > 0 ? std::max(0, ai->frames - 1) / ai->framerate : 0.0f, ai->looping ? "  |  looping" : "");
                ImGui::TextDisabled("Looping comes from the name: ..._loop or ..._idle.");
                ImGui::Text("%zu animated parts", ai->parts.size());
                if (!ai->model.empty()) {
                    ImGui::Text("Skeleton from model:");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ai->model.c_str())) selectAsset(Kind::XModel, ai->model);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%zu of %zu parts match)", ai->modelBones, ai->parts.size());
                }
                if (ImGui::CollapsingHeader(("Notetracks (" + std::to_string(ai->notes.size()) + ")").c_str(),
                                            ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ai->notes.empty()) ImGui::TextDisabled("none (the compiler adds \"end\")");
                    for (const auto& [f, n] : ai->notes)
                        ImGui::BulletText("frame %d (%.2f s)  %s", f, ai->framerate > 0 ? f / ai->framerate : 0.0f, n.c_str());
                }
                if (ImGui::CollapsingHeader(("Parts (" + std::to_string(ai->parts.size()) + ")").c_str())) {
                    ImGui::Indent();
                    for (const std::string& p : ai->parts) ImGui::TextUnformatted(p.c_str());
                    ImGui::Unindent();
                }
            }
            // Wire it into a weapon slot.
            if (index.count(Kind::Weapon) && ImGui::CollapsingHeader("Use in weapon")) {
                int& weaponIdx = assignWeapon;
                int& slot = assignSlot;
                std::vector<std::string> weapons = index.names(Kind::Weapon);
                weaponIdx = std::clamp(weaponIdx, 0, static_cast<int>(weapons.size()) - 1);
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("Weapon", weapons[weaponIdx].c_str())) {
                    for (int i = 0; i < static_cast<int>(weapons.size()); i++)
                        if (ImGui::Selectable(weapons[i].c_str(), i == weaponIdx)) weaponIdx = i;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("Slot", weapondef::animKey(slot))) {
                    for (int i = 1; i < weapondef::kAnimCount; i++)
                        if (ImGui::Selectable(weapondef::animKey(i), i == slot)) slot = i;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Assign")) {
                    const std::string wname = weapons[weaponIdx];
                    const std::string anim = a->name;
                    const Asset* w = index.find(Kind::Weapon, wname);
                    if (w && (weaponEditName == wname || selectAsset(Kind::Weapon, wname))) {
                        // into the weapon editor (unsaved until Save)
                        if (weaponEditName != wname) {
                            std::ifstream in(w->path, std::ios::binary);
                            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                            std::string err;
                            weaponEdit = {};
                            if (weaponEdit.parse(text, err)) weaponEditName = wname;
                        }
                        if (weaponEditName == wname) {
                            weaponEdit.set(weapondef::animKey(slot), anim);
                            weaponDirty = true;
                        }
                        selKind = Kind::Weapon;
                        selName = wname;
                    }
                }
            }
            usedBy();
            break;
        }
        case Kind::Image:
        case Kind::Sky: {
            const assetman::ImageInfo* ii = nullptr;
            if (an)
                if (auto it = an->images.find(a->name); it != an->images.end()) ii = &it->second;
            if (ii && ii->ok) {
                ImGui::Text("%d x %d%s", ii->width, ii->height, ii->alpha ? "  |  has alpha" : "");
            }
            const float w = std::min(ImGui::GetContentRegionAvail().x, 512.0f);
            float h = w;
            if (ii && ii->ok && ii->width > 0) h = w * ii->height / ii->width;
            if (ID3D11ShaderResourceView* tex = host.imageTexture(a->path.string())) ImGui::Image((ImTextureID)(intptr_t)tex, {w, h});
            if (a->kind == Kind::Image) usedBy();
            break;
        }
        case Kind::Effect: {
            ImGui::TextWrapped("Effect \"%s\": place it with an fx_origin entity (fx key) or name it in a weapon (viewFlashEffect, ...).",
                               a->name.c_str());
            usedBy();
            break;
        }
        default:
            ImGui::TextDisabled("Open it externally to edit.");
            break;
    }
}

// ---- weapon editor ----

void AssetPanel::drawWeaponEditor(const Asset& a) {
    const assetman::Analysis* an = analysis.get();
    if (weaponEditName != a.name) {
        std::string text, err;
        {
            std::ifstream in(a.path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        weaponEdit = {};
        if (!weaponEdit.parse(text, err)) {
            ImGui::TextColored(levelColor(Issue::Level::Error), "%s", err.c_str());
            return;
        }
        weaponEditName = a.name;
        weaponDirty = false;
    }
    ImGui::BeginDisabled(!weaponDirty);
    if (ImGui::Button("Save") || (weaponDirty && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
        std::string err;
        if (assetman::saveWeapon(a, weaponEdit, err)) {
            weaponDirty = false;
            host.log("Saved weapon " + a.name);
            rescan();
        } else {
            MessageBoxA(host.window(), err.c_str(), "Save failed", MB_OK | MB_ICONERROR);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) weaponEditName.clear();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (weaponDirty) ImGui::TextColored(levelColor(Issue::Level::Warning), "unsaved changes");
    else ImGui::TextDisabled("%zu keys", weaponEdit.kv.size());

    if (!ImGui::BeginTabBar("##wtabs")) return;
    const std::vector<std::string> modelNames = index.names(Kind::XModel), animNames = index.names(Kind::XAnim),
                                   fxNames = index.names(Kind::Effect);
    // One reference row: key, value (typed or picked from zone_source), and where it resolves.
    auto refRow = [&](const std::string& key, weapondef::RefKind rk) {
        ImGui::PushID(key.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(key.c_str());
        ImGui::TableNextColumn();
        const std::string* cur = weaponEdit.get(key);
        std::string v = cur ? *cur : "";
        const std::vector<std::string>& pick = rk == weapondef::RefKind::Model ? modelNames
                                               : rk == weapondef::RefKind::Anim ? animNames
                                                                                : fxNames;
        ImGui::SetNextItemWidth(-30);
        if (inputString("##v", v)) weaponEdit.set(key, v), weaponDirty = true;
        ImGui::SameLine(0, 2);
        if (ImGui::BeginCombo("##pick", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("(none)")) weaponEdit.set(key, ""), weaponDirty = true;
            for (const std::string& n : pick)
                if (ImGui::Selectable(n.c_str(), iequals(n, v))) weaponEdit.set(key, n), weaponDirty = true;
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        v = weaponEdit.get(key) ? *weaponEdit.get(key) : "";
        if (v.empty()) ImGui::TextDisabled("-");
        else {
            std::string res;
            if (rk == weapondef::RefKind::Model) {
                res = zsrc::resolve(v, modelNames);
                if (res.empty() && iequals(key, "handModel"))
                    for (const std::string& m : modelNames)
                        if (m.rfind("viewhands", 0) == 0) {
                            res = m;
                            break;
                        }
            } else if (rk == weapondef::RefKind::Anim) res = zsrc::resolve(v, animNames);
            else if (rk == weapondef::RefKind::Fx && std::find(fxNames.begin(), fxNames.end(), zsrc::lower(v)) != fxNames.end())
                res = zsrc::lower(v);
            if (!res.empty()) {
                const Kind k = rk == weapondef::RefKind::Model ? Kind::XModel : rk == weapondef::RefKind::Anim ? Kind::XAnim : Kind::Effect;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.45f, 0.85f, 0.45f, 1});
                if (ImGui::Selectable(("zone_source: " + res).c_str())) selectAsset(k, res);
                ImGui::PopStyleColor();
            } else if (rk == weapondef::RefKind::Anim) {
                ImGui::TextColored(levelColor(Issue::Level::Error), "missing (left empty)");
            } else {
                ImGui::TextColored(levelColor(Issue::Level::Warning), "by name (stock)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Not in zone_source: the game looks it up by name. It works only if a loaded zone\n"
                                      "has it (common stock assets do); a missing model shows the default model.");
            }
        }
        ImGui::PopID();
    };
    auto refTable = [&](const char* id, auto rows) {
        if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) return;
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("resolves to", ImGuiTableColumnFlags_WidthFixed, 230);
        ImGui::TableHeadersRow();
        rows();
        ImGui::EndTable();
    };

    if (ImGui::BeginTabItem("Models & anims")) {
        ImGui::TextDisabled("Pick from zone_source with the arrow, or type a stock name. Green = compiled into the map.");
        if (ImGui::CollapsingHeader("Models", ImGuiTreeNodeFlags_DefaultOpen))
            refTable("##wmodels", [&] {
                for (const char* k : {"gunModel", "handModel", "worldModel", "knifeModel", "worldKnifeModel", "worldClipModel",
                                      "rocketModel", "projectileModel"})
                    if (weaponEdit.get(k) || !std::strcmp(k, "gunModel") || !std::strcmp(k, "handModel") || !std::strcmp(k, "worldModel"))
                        refRow(k, weapondef::RefKind::Model);
                for (const auto& [k, v] : weaponEdit.kv)  // gunModel2.. / worldModel2.. in use
                    if (!v.empty() && weapondef::refKind(k) == weapondef::RefKind::Model && k.size() > 0 &&
                        std::isdigit(static_cast<unsigned char>(k.back())))
                        refRow(k, weapondef::RefKind::Model);
            });
        if (ImGui::CollapsingHeader("Animations (all 37 slots)", ImGuiTreeNodeFlags_DefaultOpen))
            refTable("##wanims", [&] {
                for (int s = 1; s < weapondef::kAnimCount; s++) refRow(weapondef::animKey(s), weapondef::RefKind::Anim);
            });
        if (ImGui::CollapsingHeader("Effects"))
            refTable("##wfx", [&] {
                for (const char* k : {"viewFlashEffect", "worldFlashEffect", "viewShellEjectEffect", "worldShellEjectEffect",
                                      "viewLastShotEjectEffect", "worldLastShotEjectEffect", "projExplosionEffect", "projTrailEffect"})
                    refRow(k, weapondef::RefKind::Fx);
            });
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Stats")) {
        auto num = [&](const char* k) {
            const std::string* v = weaponEdit.get(k);
            return v ? std::strtod(v->c_str(), nullptr) : 0.0;
        };
        const double ft = num("fireTime");
        ImGui::Text("%.0f RPM  |  %.0f -> %.0f damage over %.0f..%.0f units  |  %.0f / %.0f rounds", ft > 0 ? 60.0 / ft : 0.0,
                    num("damage"), num("minDamage"), num("maxDamageRange"), num("minDamageRange"), num("clipSize"), num("maxAmmo"));
        if (ImGui::BeginTable("##wstats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("stat", ImGuiTableColumnFlags_WidthFixed, 170);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            for (const StatKey& sk : kStatKeys) {
                ImGui::PushID(sk.key);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sk.label);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", sk.key, *sk.help ? "\n" : "", sk.help);
                ImGui::TableNextColumn();
                const std::string* cur = weaponEdit.get(sk.key);
                std::string v = cur ? *cur : "";
                ImGui::SetNextItemWidth(-1);
                if (const char* const* names = enumNamesFor(sk.key)) {
                    if (ImGui::BeginCombo("##e", v.empty() ? names[0] : v.c_str())) {
                        for (int e = 0; names[e]; e++)
                            if (ImGui::Selectable(names[e], iequals(v, names[e]))) weaponEdit.set(sk.key, names[e]), weaponDirty = true;
                        ImGui::EndCombo();
                    }
                } else if (inputString("##v", v)) {
                    weaponEdit.set(sk.key, v);
                    weaponDirty = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("All keys")) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##kfilter", "filter keys / values", weaponKeyFilter, sizeof weaponKeyFilter);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##newkey", weaponNewKey[0] ? weaponNewKey : "add key...", ImGuiComboFlags_HeightLarge)) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##nk", "key name", weaponNewKey, sizeof weaponNewKey);
            for (const weapondef::Field& f : weapondef::fields())
                if (!weaponEdit.get(f.key) && containsNoCase(f.key, weaponNewKey) && ImGui::Selectable(f.key))
                    strncpy_s(weaponNewKey, f.key, _TRUNCATE);
            for (int s = 1; s < weapondef::kAnimCount; s++)
                if (!weaponEdit.get(weapondef::animKey(s)) && containsNoCase(weapondef::animKey(s), weaponNewKey) &&
                    ImGui::Selectable(weapondef::animKey(s)))
                    strncpy_s(weaponNewKey, weapondef::animKey(s), _TRUNCATE);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##nv", "value", weaponNewValue, sizeof weaponNewValue);
        ImGui::SameLine();
        ImGui::BeginDisabled(!weaponNewKey[0]);
        if (ImGui::Button("Add")) {
            weaponEdit.set(weaponNewKey, weaponNewValue);
            weaponDirty = true;
            weaponNewKey[0] = weaponNewValue[0] = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginTable("##wkeys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 22);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            std::vector<int> rows;
            for (int i = 0; i < static_cast<int>(weaponEdit.kv.size()); i++)
                if (containsNoCase(weaponEdit.kv[i].first, weaponKeyFilter) || containsNoCase(weaponEdit.kv[i].second, weaponKeyFilter))
                    rows.push_back(i);
            int erase = -1;
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(rows.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                    auto& [k, v] = weaponEdit.kv[rows[r]];
                    ImGui::PushID(rows[r]);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(k.c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1);
                    const bool multi = v.find('\n') != std::string::npos;
                    if (multi ? inputMultiline("##v", v, {-1, ImGui::GetTextLineHeight() * 4}) : inputString("##v", v)) weaponDirty = true;
                    if (const char* const* names = enumNamesFor(k); names && ImGui::IsItemHovered()) {
                        std::string list;
                        for (int e = 0; names[e]; e++) list += std::string(e ? ", " : "") + names[e];
                        ImGui::SetTooltip("one of: %s", list.c_str());
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("x")) erase = rows[r];
                    ImGui::PopID();
                }
            ImGui::EndTable();
            if (erase >= 0) {
                weaponEdit.kv.erase(weaponEdit.kv.begin() + erase);
                weaponDirty = true;
            }
        }
        ImGui::EndTabItem();
    }

    if (an)
        if (auto it = an->weapons.find(a.name); it != an->weapons.end() && !it->second.warnings.empty() &&
                                                ImGui::BeginTabItem(("Compiler notes (" + std::to_string(it->second.warnings.size()) + ")").c_str())) {
            ImGui::TextDisabled("From the last save, as the map compiler reads the file:");
            for (const std::string& w : it->second.warnings) ImGui::BulletText("%s", w.c_str());
            ImGui::EndTabItem();
        }
    ImGui::EndTabBar();
}

// ---- file operations ----

void AssetPanel::drawPopups() {
    static const char* const titles[] = {"", "Rename asset", "Duplicate weapon", "New weapon"};
    if (popup != Popup::None && !ImGui::IsPopupOpen(titles[static_cast<int>(popup)]))
        ImGui::OpenPopup(titles[static_cast<int>(popup)]);
    for (int p = 1; p <= 3; p++) {
        if (!ImGui::BeginPopupModal(titles[p], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        const Popup kind = static_cast<Popup>(p);
        const Asset* a = index.find(selKind, selName);
        if (kind != Popup::NewWeapon && a) ImGui::Text("%s  %s", assetman::kindName(a->kind), a->name.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("New name", popupName, sizeof popupName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("lowercase letters, digits and _");
        int& gun = newGun;
        int& world = newWorld;
        const std::vector<std::string> models = index.names(Kind::XModel);
        if (kind == Popup::NewWeapon) {
            auto modelCombo = [&](const char* label, int& idx) {
                if (ImGui::BeginCombo(label, idx >= 0 && idx < static_cast<int>(models.size()) ? models[idx].c_str() : "(none)")) {
                    if (ImGui::Selectable("(none)", idx < 0)) idx = -1;
                    for (int i = 0; i < static_cast<int>(models.size()); i++)
                        if (ImGui::Selectable(models[i].c_str(), i == idx)) idx = i;
                    ImGui::EndCombo();
                }
            };
            modelCombo("View model", gun);
            modelCombo("World model", world);
            ImGui::TextDisabled("Starts from a full-auto rifle (AK-74u stats); set anims in the weapon's tabs.");
        }
        if (kind == Popup::Rename && a) {
            if (a->kind == Kind::XModel || a->kind == Kind::XAnim || a->kind == Kind::Effect)
                ImGui::Checkbox(a->kind == Kind::XModel ? "Update weapon files and map entities that use it" : "Update weapon files that use it",
                                &updateRefs);
            if (a->kind == Kind::Image) ImGui::TextColored(levelColor(Issue::Level::Warning), "Models find textures by file name: ones using it lose it.");
            if (a->kind == Kind::Weapon) ImGui::TextDisabled("Scripts that give the weapon by name aren't changed.");
        }
        if (!popupError.empty()) ImGui::TextColored(levelColor(Issue::Level::Error), "%s", popupError.c_str());
        bool ok = ImGui::Button("OK", {100, 0}) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            popup = Popup::None;
            ImGui::CloseCurrentPopup();
            ok = false;
        }
        if (ok) {
            std::string err;
            const std::string name = popupName;
            bool done = false, preview = false;
            if (kind == Popup::NewWeapon) {
                done = assetman::newWeapon(rootDir, name, gun >= 0 && gun < static_cast<int>(models.size()) ? models[gun] : "",
                                           world >= 0 && world < static_cast<int>(models.size()) ? models[world] : "", err);
                if (done) host.log("Created weapon " + name);
            } else if (a && kind == Popup::Duplicate) {
                done = assetman::duplicateWeapon(index, *a, name, err);
                if (done) host.log("Duplicated weapon " + a->name + " as " + name);
            } else if (a && kind == Popup::Rename) {
                if (a->kind == Kind::Weapon && weaponDirty) {
                    assetman::saveWeapon(*a, weaponEdit, err);
                    weaponDirty = false;
                }
                std::vector<fs::path> changed;
                const std::string old = a->name;
                const Kind k = a->kind;
                done = assetman::renameAsset(index, *a, name, updateRefs, changed, err);
                if (done) {
                    host.log("Renamed " + old + " -> " + name + (changed.empty() ? "" : " (" + std::to_string(changed.size()) + " weapon file(s) updated)"));
                    if (k == Kind::XModel && updateRefs) host.renameEntityModels(old, name);
                    preview = k == Kind::XModel || k == Kind::Image;
                    weaponEditName.clear();
                }
            }
            if (done) {
                Kind k = kind == Popup::Rename && a ? a->kind : Kind::Weapon;
                std::string sel = name;
                if (kind == Popup::Rename && a && a->kind == Kind::Image) sel = name + zsrc::lower(a->path.extension().string());
                if (kind == Popup::Rename && a && (a->kind == Kind::Effect || a->kind == Kind::Script)) {
                    const size_t slash = a->name.rfind('/');
                    sel = (slash == std::string::npos ? "" : a->name.substr(0, slash + 1)) + name +
                          (a->kind == Kind::Script ? zsrc::lower(a->path.extension().string()) : "");
                }
                afterChange(preview);
                selKind = k;
                selName = sel;
                showProblems = false;
                popup = Popup::None;
                ImGui::CloseCurrentPopup();
            } else {
                popupError = err;
            }
        }
        ImGui::EndPopup();
    }
}

void AssetPanel::importFiles() {
    std::vector<char> buf(64 * 1024, 0);
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = host.window();
    ofn.lpstrFilter =
        "zone_source assets\0*.xmodel_export;*.xanim_export;*.png;*.tga;*.efx;*.gsc;*.csc;*.atr;*.\0"
        "Models (*.xmodel_export)\0*.xmodel_export\0Anims (*.xanim_export)\0*.xanim_export\0"
        "Textures (*.png, *.tga)\0*.png;*.tga\0Effects (*.efx)\0*.efx\0All files (weapon files have no extension)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = "Import into zone_source";
    ofn.Flags = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn)) return;
    std::vector<fs::path> files;
    const char* p = buf.data();
    const std::string first = p;
    p += first.size() + 1;
    if (!*p) files.push_back(first);  // one file: the full path
    else
        for (; *p; p += std::strlen(p) + 1) files.push_back(fs::path(first) / p);
    int ok = 0;
    bool preview = false;
    std::string errors;
    std::string lastName;
    Kind lastKind = Kind::Other;
    for (const fs::path& f : files) {
        std::vector<fs::path> copied;
        std::string err;
        if (assetman::importFile(rootDir, f, copied, err)) {
            ok++;
            for (const fs::path& c : copied) host.log("Imported " + c.string());
            lastKind = assetman::importKind(f);
            lastName = lastKind == Kind::Image ? zsrc::lower(f.filename().string())
                       : lastKind == Kind::Weapon ? zsrc::lower(f.filename().string())
                                                  : zsrc::assetName(f);
            preview |= lastKind == Kind::XModel || lastKind == Kind::Image;
        } else {
            errors += err + "\n";
            host.log("Import failed: " + err);
        }
    }
    if (ok) {
        afterChange(preview);
        if (selectAsset(lastKind, lastName)) {}
    }
    if (!errors.empty()) MessageBoxA(host.window(), errors.c_str(), "Some files weren't imported", MB_OK | MB_ICONWARNING);
}

void AssetPanel::deleteAsset(const Asset& a) {
    std::vector<fs::path> files{a.path};
    files.insert(files.end(), a.lods.begin(), a.lods.end());
    std::string msg = "Move " + a.name + " to the Recycle Bin?";
    if (a.lods.size()) msg += "\n(with its " + std::to_string(a.lods.size()) + " LOD file(s))";
    if (analysis)
        if (auto it = analysis->usedBy.find({a.kind, a.name}); it != analysis->usedBy.end() && !it->second.empty())
            msg += "\n\nIt is used by " + std::to_string(it->second.size()) + " other asset(s), e.g. " + it->second[0] + ".";
    if (MessageBoxA(host.window(), msg.c_str(), "Delete asset", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
    if (!recycle(files)) {
        MessageBoxA(host.window(), ("Couldn't delete " + a.path.string()).c_str(), "Delete failed", MB_OK | MB_ICONERROR);
        return;
    }
    host.log("Deleted " + a.path.string() + " (Recycle Bin)");
    if (a.kind == Kind::Weapon && weaponEditName == a.name) weaponEditName.clear(), weaponDirty = false;
    selName.clear();
    afterChange(a.kind == Kind::XModel || a.kind == Kind::Image);
}
