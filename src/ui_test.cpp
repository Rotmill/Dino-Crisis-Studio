// "Test in game" (10.10.2026, player: "the studio starts the game right in the room I work on, with the mod"). The newest
// quicksave of the game is the start (a copy: a quicksave made during the test does not touch the player's); the game then
// walks through a door into the picked room - the place, turn and scene of a door that leads there, from the door records of
// all rooms (the game: DC_TEST_ROOM / DC_TEST_AT / DC_TEST_SCENE, main.cpp).
#include "ui.h"

#include <windows.h>

#include "i18n.h"
#include "placement.h"
#include "script.h"   // load_exe

namespace dc {

void App::door_in_start() {
    if (door_in_job_ || door_in_ready_.load() || !disc_.is_open()) return;
    const fs::path img = disc_.image_path;
    door_in_job_ = std::make_unique<std::thread>([this, img] {
        Disc d;                                   // its own handle on the image: the UI keeps reading with disc_
        std::string err;
        std::map<std::string, DoorIn> m;
        if (d.open(img, err)) {
            load_exe(d);
            for (auto& f : d.files) {
                if (f.cat != "rooms") continue;
                FileContent c;
                if (!analyze(d, f, c) || !c.is_room) continue;
                const json j = room_doors_json(c);
                if (!j.contains("doors")) continue;
                for (auto& e : j["doors"]) {
                    const std::string to = upper(e.value("to", std::string()));
                    if (to.size() != 5 || to == f.stem) continue;
                    DoorIn di;
                    di.x = e.value("x", 0); di.y = e.value("y", 0); di.z = e.value("z", 0);
                    di.rot = e.value("rotation", 0); di.scene = e.value("scene", 0);
                    di.locked = e.value("lock", 0) != 0;
                    di.from = f.stem;
                    auto it = m.find(to);
                    if (it == m.end() || (it->second.locked && !di.locked)) m[to] = di;   // an open door rather than a locked one
                }
            }
        }
        std::lock_guard<std::mutex> lk(door_in_mu_);
        door_in_ = std::move(m);
        door_in_ready_ = true;
    });
}

const DiscFile* App::test_room() const {
    return (sel_.f && sel_.f->cat == "rooms") ? sel_.f : nullptr;
}

static fs::path newest_quicksave(const fs::path& dir) {
    std::error_code ec;
    fs::path best;
    fs::file_time_type bt{};
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string n = upper(path_u8(e.path().filename()));
        if (n.compare(0, 9, "QUICKSAVE") != 0 || n.size() < 5 || n.compare(n.size() - 5, 5, ".DCSS") != 0) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bt) { best = e.path(); bt = t; }
    }
    return best;
}

void App::test_in_game() {
    const DiscFile* f = test_room();
    if (!f) return;
    if (!door_in_ready_.load()) {
        door_in_start();
        status_ = T("Finding the doors of the rooms... press Test in game again in a moment",
                    "Ищу двери комнат... нажми Test in game ещё раз через миг");
        return;
    }
    DoorIn di;
    {
        std::lock_guard<std::mutex> lk(door_in_mu_);
        auto it = door_in_.find(f->stem);
        if (it == door_in_.end()) { status_ = std::string(T("No door leads into ", "Ни одна дверь не ведёт в ")) + f->stem; return; }
        di = it->second;
    }
    std::error_code ec;
    const fs::path game = mods_root().parent_path();
    const fs::path exe = game / "DinoCrisisME.exe";
    if (!fs::is_regular_file(exe, ec)) {
        status_ = T("No DinoCrisisME.exe next to the mods folder", "Рядом с папкой mods нет DinoCrisisME.exe");
        return;
    }
    const fs::path data = game / "data";
    const fs::path best = newest_quicksave(data);
    if (best.empty()) {
        status_ = T("Quicksave once in the game (F10): the test starts from it", "Сохранись в игре один раз (F10): проба начинается с него");
        return;
    }
    const fs::path copy = data / "studio_test.dcss";
    fs::copy_file(best, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) { status_ = "cannot copy " + path_u8(best); return; }
    {   // its room story (what the scripts of the rooms hide or show) goes with it
        const fs::path st = fs::path(best.wstring() + L".story"), sc = fs::path(copy.wstring() + L".story");
        if (fs::is_regular_file(st, ec)) fs::copy_file(st, sc, fs::copy_options::overwrite_existing, ec);
        else fs::remove(sc, ec);
    }
    // the game's environment: ours on top of the studio's
    std::vector<std::pair<std::wstring, std::wstring>> add = {
        {L"DC_SS_FILE", copy.wstring()},
        {L"DC_TEST_ROOM", u8path(f->stem).wstring()},
        {L"DC_TEST_AT", std::to_wstring(di.x) + L"," + std::to_wstring(di.y) + L"," + std::to_wstring(di.z) + L"," + std::to_wstring(di.rot)},
        {L"DC_TEST_SCENE", std::to_wstring(di.scene)},
        {L"DC_BOOT_SCREENS", L"0"},
    };
    std::wstring env;
    {
        wchar_t* blk = GetEnvironmentStringsW();
        for (const wchar_t* p = blk; p && *p; p += wcslen(p) + 1) {
            const std::wstring kv = p;
            const size_t eq = kv.find(L'=', 1);
            bool ours = false;
            for (auto& a : add) if (eq != std::wstring::npos && !_wcsicmp(kv.substr(0, eq).c_str(), a.first.c_str())) ours = true;
            if (!ours) { env += kv; env.push_back(L'\0'); }
        }
        if (blk) FreeEnvironmentStringsW(blk);
        for (auto& a : add) { env += a.first + L"=" + a.second; env.push_back(L'\0'); }
        env.push_back(L'\0');
    }
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.wstring().c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, &env[0],
                        game.wstring().c_str(), &si, &pi)) {
        status_ = "cannot start " + path_u8(exe);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    status_ = strfmt(T("The game starts in %s (through the door from %s)", "Игра запускается в %s (через дверь из %s)"), f->stem.c_str(),
                     di.from.c_str());
}

}  // namespace dc
