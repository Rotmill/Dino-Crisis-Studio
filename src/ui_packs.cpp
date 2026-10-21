// The mod packages window (10.10.2026, player: "mods as packages: each mod in its own folder, a manager in the studio - on,
// off, order; a zip for itch.io and the like; maximally simple, no extra text"). The game reads the packages itself
// (dc_mods.c); here: which are on and in what order (mods\load_order.txt), which one the studio works in (Convert all and
// the room editors write there), a new package, a zip of one and a downloaded zip back into mods.
#include "ui.h"

#include <imgui.h>

#include <SDL_syswm.h>

#include "i18n.h"
#include "packs.h"

#ifdef _WIN32
#include <shobjidl.h>
#endif

namespace dc {

static HWND hwnd_pk(SDL_Window* w) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (w && SDL_GetWindowWMInfo(w, &info)) return info.info.win.window;
    return nullptr;
}

// a zip to install (open) or the zip to write (save)
static std::string zip_dialog(SDL_Window* w, bool save, const std::string& name, const fs::path& start) {
    std::string out;
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    COMDLG_FILTERSPEC spec[] = {{L"Zip (*.zip)", L"*.zip"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"zip");
    dlg->SetTitle(save ? L"Build the mod as a zip" : L"Install a mod from a zip");
    if (save && !name.empty()) dlg->SetFileName(u8path(name).wstring().c_str());
    if (!start.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(hwnd_pk(w)))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = path_u8(fs::path(p));
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

fs::path App::work_root() const {
    const fs::path m = mods_root();
    if (set_.work.empty()) return m;
    std::error_code ec;
    const fs::path p = m / u8path(set_.work);
    return fs::is_regular_file(p / "mod.json", ec) ? p : m;
}

std::string App::work_label() const {
    if (set_.work.empty()) return "mods";
    const int i = packs_.find(set_.work);
    return "mods: " + (i >= 0 ? packs_.packs[(size_t)i].name : set_.work);
}

void App::packs_rescan() {
    packs_.scan(mods_root());
    if (!set_.work.empty() && packs_.find(set_.work) < 0) { set_.work.clear(); save_settings(); }
}

void App::packs_order_changed() {
    std::string err;
    if (!packs_.save_order(err)) packs_msg_ = err;
    packs_rescan();
}

void App::win_packs() {
    if (packs_.mods != mods_root()) packs_rescan();
    ImGui::SetNextWindowSize(ImVec2(660, 470), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(T("Mod packages", "Пакеты модов"), &show_packs_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    const float fh = ImGui::GetFrameHeight();
    const ImVec4 dim(0.62f, 0.62f, 0.62f, 1);
    const ImVec4 warn(0.95f, 0.62f, 0.30f, 1);
    // the list: the own files of mods first (always on), then the packages in the game's order
    const float list_h = ImGui::GetContentRegionAvail().y - fh * 5.6f;
    ImGui::BeginChild("##packs", ImVec2(0, std::max(fh * 4, list_h)), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##pt", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, fh + 4);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("order", ImGuiTableColumnFlags_WidthFixed, fh * 2 + 8);
        ImGui::TableSetupColumn("conf", ImGuiTableColumnFlags_WidthFixed, 44);
        // the mods folder itself
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Selectable(T("mods (own files)", "mods (свои файлы)"), set_.work.empty(), ImGuiSelectableFlags_SpanAllColumns)) {
            set_.work.clear();
            save_settings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Files put straight into mods. The packages below go on top of them.",
                                      "Файлы прямо в mods. Пакеты ниже ложатся поверх них."));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(dim, "%d", packs_.own_files);
        int move_from = -1, move_to = -1;
        bool order_dirty = false;                 // applied after the list: a rescan rebuilds packs_.packs
        for (size_t i = 0; i < packs_.packs.size(); i++) {
            ModPack& p = packs_.packs[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Checkbox("##on", &p.on)) order_dirty = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.on ? T("On", "Включён") : T("Off", "Выключен"));
            ImGui::TableSetColumnIndex(1);
            std::string label = p.name;
            if (!p.version.empty()) label += "   " + p.version;
            if (!p.on) ImGui::PushStyleColor(ImGuiCol_Text, dim);
            if (ImGui::Selectable(label.c_str(), set_.work == p.dir, ImGuiSelectableFlags_AllowOverlap)) {
                set_.work = p.dir;
                save_settings();
            }
            if (!p.on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && (!p.author.empty() || !p.description.empty()))
                ImGui::SetTooltip("%s%s%s", p.author.c_str(), (!p.author.empty() && !p.description.empty()) ? "\n" : "",
                                  p.description.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(dim, "%d", p.files);
            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move_from = (int)i; move_to = (int)i - 1; }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 4);
            ImGui::BeginDisabled(i + 1 == packs_.packs.size());
            if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) { move_from = (int)i; move_to = (int)i + 1; }
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(4);
            const size_t nc = p.wins.size() + p.loses.size();
            if (p.on && nc) {
                ImGui::TextColored(warn, "! %d", (int)nc);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(T("The same files as another package. The lower one in the list wins.",
                                             "Те же файлы, что у другого пакета. Берётся у того, кто ниже в списке."));
                    int k = 0;
                    for (auto& s : p.wins) if (k++ < 12) ImGui::TextColored(dim, "%s", s.c_str());
                    for (auto& s : p.loses) if (k++ < 12) ImGui::TextColored(warn, "%s", s.c_str());
                    if (k > 12) ImGui::TextColored(dim, "... %d", k - 12);
                    ImGui::EndTooltip();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (move_from >= 0 && move_to >= 0) {
            std::swap(packs_.packs[(size_t)move_from], packs_.packs[(size_t)move_to]);
            order_dirty = true;
        }
        if (order_dirty) packs_order_changed();
    }
    if (packs_.packs.empty()) ImGui::TextColored(dim, "%s", T("No packages yet.", "Пакетов пока нет."));
    ImGui::EndChild();
    // the package the studio works in: its card
    const int wi = packs_.find(set_.work);
    if (wi >= 0) {
        ModPack& p = packs_.packs[(size_t)wi];
        static char nm[128], ver[32], au[96], ds[512];
        static std::string for_dir;
        if (for_dir != p.dir) {
            for_dir = p.dir;
            snprintf(nm, sizeof nm, "%s", p.name.c_str());
            snprintf(ver, sizeof ver, "%s", p.version.c_str());
            snprintf(au, sizeof au, "%s", p.author.c_str());
            snprintf(ds, sizeof ds, "%s", p.description.c_str());
        }
        bool ch = false;
        const float lw = ImGui::CalcTextSize("Description").x + 12;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(T("Name", "Имя")); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 150);
        ch |= ImGui::InputText("##nm", nm, sizeof nm);
        ImGui::SameLine(); ImGui::TextUnformatted(T("Version", "Версия")); ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ch |= ImGui::InputText("##ver", ver, sizeof ver);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(T("Author", "Автор")); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        ch |= ImGui::InputText("##au", au, sizeof au);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(T("Description", "Описание")); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        ch |= ImGui::InputText("##ds", ds, sizeof ds);
        if (ch) {
            p.name = nm; p.version = ver; p.author = au; p.description = ds;
            std::string err;
            if (!pack_save_info(packs_.mods, p, err)) packs_msg_ = err;
        }
    } else {
        ImGui::Dummy(ImVec2(0, fh * 0.5f));
        ImGui::TextColored(dim, "%s", T("The studio writes into the highlighted line: Convert all and the room editors.",
                                       "Студия пишет в выделенную строку: Convert all и правка комнат."));
        ImGui::Dummy(ImVec2(0, fh * 2.0f));
    }
    // buttons
    ImGui::Separator();
    if (ImGui::Button(T("New", "Новый"))) { new_pack_name_[0] = 0; ImGui::OpenPopup("##newpack"); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A new empty package; the studio works in it", "Новый пустой пакет; студия работает в нём"));
    ImGui::SameLine();
    if (ImGui::Button(T("Install .zip...", "Установить .zip..."))) {
        const std::string z = zip_dialog(win_, false, "", fs::path());
        if (!z.empty()) {
            std::string dir, err;
            bool exists = false;
            if (pack_install_zip(u8path(z), mods_root(), false, dir, exists, err)) {
                packs_rescan();
                packs_order_changed();
                packs_msg_ = std::string(T("Installed: ", "Установлен: ")) + dir;
            } else if (exists) {
                ask_replace_zip_ = z;
                ask_replace_dir_ = dir;
                ImGui::OpenPopup("##replace");
            } else packs_msg_ = err;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(wi < 0);
    if (ImGui::Button(T("Build .zip...", "Собрать .zip..."))) {
        const ModPack& p = packs_.packs[(size_t)wi];
        std::string base = p.name.empty() ? p.dir : p.name;
        if (!p.version.empty()) base += " " + p.version;
        for (char& c : base) if (strchr("<>:\"/\\|?*", c)) c = '_';
        const std::string z = zip_dialog(win_, true, base + ".zip", mods_root().parent_path());
        if (!z.empty()) {
            std::string err;
            int n = 0;
            if (pack_build_zip(mods_root(), p.dir, u8path(z), err, &n)) packs_msg_ = strfmt("%s %s (%d)", T("Built", "Собран"), z.c_str(), n);
            else packs_msg_ = err;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", T("The highlighted package as a zip: unpacked into mods, it is a package again",
                                  "Выделенный пакет в zip: распакованный в mods, он снова пакет"));
    ImGui::SameLine();
    if (ImGui::Button(T("Open folder", "Открыть папку"))) {
        std::error_code ec;
        fs::create_directories(work_root(), ec);
        open_in_explorer(work_root());
    }
    if (!packs_msg_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(warn, "%s", packs_msg_.c_str());
    }
    // a new package: its name
    if (ImGui::BeginPopup("##newpack")) {
        ImGui::TextUnformatted(T("Name", "Имя"));
        ImGui::SetNextItemWidth(320);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##nn", new_pack_name_, sizeof new_pack_name_, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button(T("Create", "Создать")) || enter) && new_pack_name_[0]) {
            std::string dir, err;
            if (pack_create(mods_root(), new_pack_name_, dir, err)) {
                packs_rescan();
                packs_order_changed();
                set_.work = dir;
                save_settings();
                packs_msg_.clear();
            } else packs_msg_ = err;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##replace")) {
        ImGui::Text(T("%s is in mods already. Replace it?", "%s уже есть в mods. Заменить?"), ask_replace_dir_.c_str());
        if (ImGui::Button(T("Replace", "Заменить"))) {
            std::string dir, err;
            bool exists = false;
            if (pack_install_zip(u8path(ask_replace_zip_), mods_root(), true, dir, exists, err)) {
                packs_rescan();
                packs_order_changed();
                packs_msg_ = std::string(T("Installed: ", "Установлен: ")) + dir;
            } else packs_msg_ = err;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel", "Отмена"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

}  // namespace dc
