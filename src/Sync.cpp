#include "Sync.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <sys/statvfs.h>

#include "Config.hpp"
#include "Http.hpp"
#include "main.hpp"

#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "playlistcore/shared/PlaylistCore.hpp"
#include "songcore/shared/SongCore.hpp"

#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsViewController.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "GlobalNamespace/BeatmapLevelsModel.hpp"
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelFilteringNavigationController.hpp"
#include "GlobalNamespace/LevelSelectionNavigationController.hpp"
#include "GlobalNamespace/PauseController.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/SelectLevelCategoryViewController.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TableView.hpp"
#include "System/Collections/Generic/HashSet_1.hpp"
#include "System/Collections/Generic/IReadOnlyList_1.hpp"
#include "System/Threading/CancellationTokenSource.hpp"
#include "UnityEngine/Application.hpp"
#include "UnityEngine/Resources.hpp"

namespace StudioSync {
    namespace {
        constexpr std::string_view FILE_PREFIX = "StudioSync_";
        constexpr size_t MAX_PLAYLIST_BYTES = 2500000;
        constexpr int LINK_POLL_SECONDS = 5;
        constexpr int LINK_GIVE_UP_SECONDS = 11 * 60;
        constexpr size_t BACKUPS_PER_PLAYLIST = 3;
        constexpr int64_t KEEP_DELETED_SECONDS = 30 * 86400;  // deleted playlists stay restorable this long
        constexpr size_t MAX_DELETED_LISTED = 30;
        constexpr int STATS_EVERY_SECONDS = 30 * 60;  // headset storage for Studio, at most this often unless something changed

        using Http::Response;
        using SyncedMap = std::map<std::string, int64_t>;

        // A Studio-hosted playlist this headset should have.
        struct Item {
            std::string id, title, url;
            int64_t updated = 0;
        };
        // An edit Studio sent back for one of the headset's own playlists.
        struct Write {
            std::string id, file, url;
            bool create = false;  // a new playlist Studio made for this headset
            int64_t restore = 0;  // put back the backup taken at this time instead of downloading
            std::string savedAs;  // set when a new playlist had to take another name
            bool remove = false;  // delete it (it's kept as a backup Studio can restore)
            bool songs = false;   // with remove: also delete its songs no other playlist uses
        };
        // One sync from start to finish; each step hands it to the next.
        struct Run {
            std::string token;
            std::vector<Item> items;
            std::vector<Write> writes;
            std::vector<std::string> edited, created, restored, deleted;
            std::vector<std::string> touched;  // playlist paths written this sync, checked for missing songs
            std::vector<std::string> need;
            std::set<std::string> dropSongs;  // songs of playlists deleted with "also delete its songs"
            // Studio's "My order" for the player's own playlists (file names, top first) and when it was saved
            int64_t orderT = 0;
            std::vector<std::string> orderFiles;
            std::string cleanupId;  // a song clean up Studio asked for
            // Get missing songs Studio asked for: for these playlists, or every one
            std::string missingId;
            std::vector<std::string> missingFiles;
            bool missingAll = false;
            bool changed = false, reloaded = false, reordered = false;
            int written = 0, removed = 0, failed = 0, shared = 0, total = 0, songsDeleted = 0;
        };
        using RunPtr = std::shared_ptr<Run>;

        std::string pendingToken, code, status;
        std::chrono::steady_clock::time_point linkStarted, lastSync{};
        bool syncing = false, downloading = false, cleaning = false;
        // Sync now (or a Sharing change) asked for while a sync, song download or clean up was running.
        bool syncAgain = false;
        // An automatic sync (game start, main menu, opening the tab) skipped while songs downloaded:
        // it runs when they finish, so changes waiting in Studio don't sit until the next one.
        bool missedSync = false;
        Summary summary;
        LooseSongs loose;
        std::function<void()> listener;
        // Headset storage for Studio: worked out on a worker thread now and then, more often after songs change.
        bool statsRunning = false, statsDirty = true;
        std::chrono::steady_clock::time_point statsAt{};
        // The song clean up Studio asked for that's running now.
        std::string cleanupRunning;
        // The Get missing songs Studio asked for that's running now.
        std::string missingRunning;
        // What's running that takes a while, for the progress line in the menu.
        Activity activity;
        // The minute check in the menu: where Studio's change counter stood at the last sync (-1 = not known yet),
        // and whether this Worker answers the check at all.
        int64_t knownChg = -1;
        bool pingOff = false, menuChecks = false;

        void Notify() {
            if (listener)
                listener();
        }
        // A song is being played. Work Studio asks for that reads every playlist or deletes songs waits for the menu.
        bool Playing() {
            for (auto p : UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::PauseController*>())
                if (p && p->get_isActiveAndEnabled())
                    return true;
            return false;
        }
        void SetStatus(std::string text) {
            status = std::move(text);
            Notify();
        }

        SyncedMap LoadSynced() {
            SyncedMap out;
            for (auto const& entry : getConfig().Synced.GetValue()) {
                auto eq = entry.find('=');
                if (eq != std::string::npos)
                    out[entry.substr(0, eq)] = std::strtoll(entry.c_str() + eq + 1, nullptr, 10);
            }
            return out;
        }
        void SaveSynced(SyncedMap const& map) {
            std::vector<std::string> out;
            for (auto const& [id, updated] : map)
                out.push_back(id + "=" + std::to_string(updated));
            getConfig().Synced.SetValue(out);
        }

        std::string Server() {
            auto s = getConfig().Server.GetValue();
            while (!s.empty() && s.back() == '/')
                s.pop_back();
            return s;
        }

        // A random 43-character token the Worker only ever stores as a SHA-256 hash.
        std::string NewToken() {
            static constexpr char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::random_device rd;
            std::string t;
            for (int i = 0; i < 43; i++)
                t += abc[rd() % 64];
            return t;
        }

        void Post(std::string const& url, std::string const& token, std::string body, std::function<void(Response)> done) {
            Http::Request(url, token, std::move(body), std::move(done));
        }
        void Get(std::string const& url, std::string const& token, std::function<void(Response)> done) {
            Http::Request(url, token, std::nullopt, std::move(done));
        }

        bool ParseText(std::string const& text, rapidjson::Document& doc) {
            doc.Parse(text.c_str());
            return !doc.HasParseError() && doc.IsObject();
        }
        bool ParseJson(Response const& r, rapidjson::Document& doc) {
            return r.responseData && ParseText(*r.responseData, doc);
        }
        bool IsPlaylist(rapidjson::Document const& doc) {
            return doc.IsObject() && doc.HasMember("songs") && doc["songs"].IsArray();
        }
        std::string GetString(rapidjson::Value const& v, char const* key) {
            if (!v.IsObject())
                return "";
            auto it = v.FindMember(key);
            return it != v.MemberEnd() && it->value.IsString() ? it->value.GetString() : "";
        }
        std::string ToJson(rapidjson::Value const& v) {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> w(sb);
            v.Accept(w);
            return sb.GetString();
        }
        // Maps in a playlist, counted the way Studio counts them: each difficulty of a song is a map,
        // a song without difficulties counts once.
        int MapCount(rapidjson::Document const& doc) {
            std::set<std::string> keys;
            auto lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                return s;
            };
            for (auto const& song : doc["songs"].GetArray()) {
                auto hash = lower(GetString(song, "hash"));
                if (hash.empty())
                    continue;
                bool any = false;
                auto ds = song.FindMember("difficulties");
                if (ds != song.MemberEnd() && ds->value.IsArray())
                    for (auto const& d : ds->value.GetArray()) {
                        if (!d.IsObject())
                            continue;
                        auto mode = GetString(d, "characteristic");
                        keys.insert(hash + "|" + lower(mode.empty() ? "Standard" : mode) + "|" + lower(GetString(d, "name")));
                        any = true;
                    }
                if (!any)
                    keys.insert(hash + "|*");
            }
            return (int) keys.size();
        }

        bool ValidId(std::string const& id) {
            if (id.size() < 6 || id.size() > 16)
                return false;
            return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char) c); });
        }
        // A plain file name inside the playlist folder, never a path.
        bool ValidFile(std::string const& f) {
            if (f.empty() || f.size() > 200 || f == "." || f == ".." || f.find_first_of("/\\") != std::string::npos || f.find('\0') != std::string::npos)
                return false;
            auto lower = f;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            return lower.ends_with(".bplist") || lower.ends_with(".json");
        }

        std::string PlaylistsDir() {
            return getDataDir(std::string_view("PlaylistManager")) + "Playlists";
        }
        std::string PathIn(std::string const& file) {
            return (std::filesystem::path(PlaylistsDir()) / file).string();
        }
        std::string PathFor(std::string const& id) {
            return PathIn(std::string(FILE_PREFIX) + id + ".bplist");
        }

        std::string UrlEncode(std::string const& s) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : s) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    out += (char) c;
                else {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
            return out;
        }

        // Change detection only, so a fast non-cryptographic hash is enough.
        std::string Fingerprint(std::string const& text) {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : text) {
                h ^= c;
                h *= 1099511628211ULL;
            }
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%016llx-%zx", (unsigned long long) h, text.size());
            return buf;
        }

        std::string LocalTimeNow() {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_r(&t, &tm);
            char buf[16];
            std::strftime(buf, sizeof(buf), "%I:%M %p", &tm);
            std::string s = buf;
            if (s.starts_with("0"))
                s.erase(0, 1);
            return s;
        }

        // Every playlist file this mod wrote for Studio-hosted playlists, keyed by playlist id.
        std::map<std::string, std::string> OurFiles() {
            std::map<std::string, std::string> files;
            std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(PlaylistsDir(), ec)) {
                auto name = entry.path().filename().string();
                if (!entry.is_regular_file() || !name.starts_with(FILE_PREFIX) || !name.ends_with(".bplist"))
                    continue;
                auto id = name.substr(FILE_PREFIX.size(), name.size() - FILE_PREFIX.size() - 7);
                if (ValidId(id))
                    files[id] = entry.path().string();
            }
            return files;
        }

        void RemoveFile(std::string const& path) {
            if (auto playlist = PlaylistCore::GetPlaylist(path))
                PlaylistCore::DeletePlaylist(playlist);
            else
                std::filesystem::remove(path);
        }

        void ForgetLink(std::string const& why) {
            getConfig().Token.SetValue("");
            SaveSynced({});
            getConfig().OrderApplied.SetValue(0);
            summary = {};
            SetStatus(why);
        }

        // ---- backups and merging for edits sent back from Studio ----

        std::filesystem::path BackupDir() {
            return std::filesystem::path(getDataDir(std::string_view("StudioSync"))) / "backups";
        }

        void Backup(std::string const& file, std::string const& original) {
            std::error_code ec;
            auto dir = BackupDir();
            std::filesystem::create_directories(dir, ec);
            writefile((dir / (file + "." + std::to_string(std::time(nullptr)) + ".bak")).string(), original);
            // Keep only the newest few backups of each playlist.
            std::vector<std::filesystem::path> mine;
            for (auto const& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().filename().string().starts_with(file + "."))
                    mine.push_back(e.path());
            std::sort(mine.begin(), mine.end());
            while (mine.size() > BACKUPS_PER_PLAYLIST) {
                std::filesystem::remove(mine.front(), ec);
                mine.erase(mine.begin());
            }
        }

        struct BackupInfo {
            int64_t time = 0;
            int count = -1, maps = -1;
            std::string title, path;
        };
        // Every playlist's backups, newest first, keyed by playlist file name.
        std::map<std::string, std::vector<BackupInfo>> AllBackups() {
            std::map<std::string, std::vector<BackupInfo>> out;
            std::error_code ec;
            for (auto const& e : std::filesystem::directory_iterator(BackupDir(), ec)) {
                auto name = e.path().filename().string();
                if (!e.is_regular_file() || !name.ends_with(".bak"))
                    continue;
                auto stem = name.substr(0, name.size() - 4);
                auto dot = stem.rfind('.');
                if (dot == std::string::npos || dot + 1 >= stem.size())
                    continue;
                auto digits = stem.substr(dot + 1);
                if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }) || digits.size() > 12)
                    continue;
                BackupInfo info;
                info.time = std::strtoll(digits.c_str(), nullptr, 10);
                rapidjson::Document doc;
                std::error_code sec;
                info.path = e.path().string();
                if (e.file_size(sec) <= MAX_PLAYLIST_BYTES && ParseText(readfile(info.path), doc) && IsPlaylist(doc)) {
                    info.count = (int) doc["songs"].Size();
                    info.maps = MapCount(doc);
                    info.title = GetString(doc, "playlistTitle");
                }
                out[stem.substr(0, dot)].push_back(info);
            }
            for (auto& [file, list] : out)
                std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.time > b.time; });
            return out;
        }

        // "Poodles.bplist" is taken: "Poodles (2).bplist", then (3) and so on.
        std::string FreeName(std::string const& file) {
            auto dot = file.rfind('.');
            auto stem = file.substr(0, dot), ext = file.substr(dot);
            std::error_code ec;
            for (int n = 2; n < 1000; n++) {
                auto name = stem + " (" + std::to_string(n) + ")" + ext;
                if (!std::filesystem::exists(PathIn(name), ec))
                    return name;
            }
            return "";
        }

        void Ack(RunPtr run, std::string const& id, std::string const& savedAs = "") {
            std::string body = "{}";
            if (!savedAs.empty()) {
                rapidjson::Document d(rapidjson::kObjectType);
                d.AddMember("file", rapidjson::Value(savedAs.c_str(), d.GetAllocator()), d.GetAllocator());
                body = ToJson(d);
            }
            Post(Server() + "/link/outbox/" + id + "/ack", run->token, body, [](Response) {});
        }

        std::string HashOf(rapidjson::Value const& song) {
            auto h = GetString(song, "hash");
            std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
            return h;
        }

        // Studio's copy wins for title, author, description, songs and a new cover.
        // Everything else in the headset's file (customData, PlaylistManager settings,
        // extra per-song fields) is kept.
        std::string Merge(std::string const& original, rapidjson::Document& edit) {
            rapidjson::Document out;
            if (!ParseText(original, out))
                return ToJson(edit);
            auto& a = out.GetAllocator();
            auto set = [&](char const* key, rapidjson::Value const& v) {
                rapidjson::Value copy(v, a);
                if (out.HasMember(key))
                    out[key] = copy;
                else
                    out.AddMember(rapidjson::Value(key, a), copy, a);
            };
            for (auto key : {"playlistTitle", "playlistAuthor", "playlistDescription"})
                if (edit.HasMember(key) && edit[key].IsString())
                    set(key, edit[key]);
            if (edit.HasMember("image") && edit["image"].IsString() && edit["image"].GetStringLength() > 0) {
                set("image", edit["image"]);
                // PlaylistManager shows "imageString" before "image", so Studio's cover replaces both.
                if (out.HasMember("imageString"))
                    out.RemoveMember("imageString");
            }

            std::map<std::string, rapidjson::Value const*> before;
            if (out.HasMember("songs") && out["songs"].IsArray())
                for (auto const& s : out["songs"].GetArray())
                    if (s.IsObject() && !HashOf(s).empty())
                        before.emplace(HashOf(s), &s);
            rapidjson::Value songs(rapidjson::kArrayType);
            for (auto const& s : edit["songs"].GetArray()) {
                if (!s.IsObject())
                    continue;
                auto old = before.find(HashOf(s));
                rapidjson::Value song(old != before.end() ? *old->second : s, a);
                if (old != before.end()) {
                    for (auto key : {"songName", "levelAuthorName", "difficulties"}) {
                        if (!s.HasMember(key))
                            continue;
                        rapidjson::Value v(s[key], a);
                        if (song.HasMember(key))
                            song[key] = v;
                        else
                            song.AddMember(rapidjson::Value(key, a), v, a);
                    }
                }
                songs.PushBack(song, a);
            }
            if (out.HasMember("songs"))
                out["songs"] = songs;
            else
                out.AddMember("songs", songs, a);
            return ToJson(out);
        }

        bool Excluded(std::string const& file) {
            auto const& list = getConfig().Excluded.GetValue();
            return std::find(list.begin(), list.end(), file) != list.end();
        }

        bool InList(std::vector<std::string> const& list, std::string const& file) {
            return std::find(list.begin(), list.end(), file) != list.end();
        }
        // Auto-download for one playlist, without telling the tab (Studio's changes come in mid-sync).
        void SetAuto(std::string const& file, bool on) {
            auto off = getConfig().AutoOff.GetValue(), yes = getConfig().AutoOn.GetValue();
            off.erase(std::remove(off.begin(), off.end(), file), off.end());
            yes.erase(std::remove(yes.begin(), yes.end(), file), yes.end());
            if (on != getConfig().AutoDownload.GetValue())
                (on ? yes : off).push_back(file);
            getConfig().AutoOff.SetValue(off);
            getConfig().AutoOn.SetValue(yes);
        }

        // ---- songs: which playlists use them, favorites, BeatSaver, deleting ----

        std::string Lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }
        bool IsHash(std::string_view h) {
            return h.size() == 40 && std::all_of(h.begin(), h.end(), [](unsigned char c) { return std::isxdigit(c); });
        }
        // "custom_level_ABC...123" (and its " WIP" form) to "abc...123"; "" for the game's own levels.
        std::string HashOfLevelId(std::string_view id) {
            constexpr std::string_view prefix = "custom_level_";
            if (id.size() < prefix.size() + 40 || Lower(std::string(id.substr(0, prefix.size()))) != prefix)
                return "";
            auto h = id.substr(prefix.size(), 40);
            return IsHash(h) ? Lower(std::string(h)) : "";
        }
        // Song hashes in a playlist's text without building a document: every "hash": "<40 hex>".
        // Covers are base64, which never contains a quote, so they can't produce a false match.
        std::set<std::string> ScanHashes(std::string const& text) {
            std::set<std::string> out;
            size_t pos = 0;
            auto skipSpace = [&]() {
                while (pos < text.size() && std::isspace((unsigned char) text[pos]))
                    pos++;
            };
            while ((pos = text.find("\"hash\"", pos)) != std::string::npos) {
                pos += 6;
                skipSpace();
                if (pos >= text.size() || text[pos] != ':')
                    continue;
                pos++;
                skipSpace();
                if (pos >= text.size() || text[pos] != '"')
                    continue;
                auto end = text.find('"', pos + 1);
                if (end == std::string::npos)
                    break;
                auto h = std::string_view(text).substr(pos + 1, end - pos - 1);
                if (IsHash(h))
                    out.insert(Lower(std::string(h)));
                pos = end + 1;
            }
            return out;
        }
        bool IsPlaylistFile(std::string const& name) {
            auto l = Lower(name);
            return l.ends_with(".bplist") || l.ends_with(".json");
        }
        // Every song used by a playlist on the headset: the player's own (shared or not) and Studio's
        // synced ones.
        std::set<std::string> SongsInPlaylists() {
            std::set<std::string> out;
            std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(PlaylistsDir(), ec)) {
                auto name = entry.path().filename().string();
                if (entry.is_regular_file() && IsPlaylistFile(name))
                    out.merge(ScanHashes(readfile(entry.path().string())));
            }
            return out;
        }

        using Favorites = System::Collections::Generic::HashSet_1<StringW>;
        // The player's in-game favorites, or null before the game has loaded player data.
        // Nothing deletes songs without them.
        Favorites* GetFavorites() {
            for (auto model : UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::PlayerDataModel*>())
                if (model && model->get_playerData())
                    return model->get_playerData()->get_favoritesLevelIds();
            return nullptr;
        }
        bool IsWip(SongCore::SongLoader::CustomBeatmapLevel* level) {
            return std::string_view(level->customLevelPath).find("CustomWIPLevels") != std::string_view::npos;
        }
        // A downloaded song (not a WIP map, not a favorite) that may be deleted, else null.
        SongCore::SongLoader::CustomBeatmapLevel* Deletable(std::string const& hash, Favorites* favorites) {
            auto level = SongCore::API::Loading::GetLevelByHash(hash);
            if (!level || IsWip(level) || (favorites && favorites->Contains(level->levelID)))
                return nullptr;
            return level;
        }

        // What BeatSaver knows about a song: that it's still there (so a deleted copy can be downloaded
        // again), and its difficulties for the "Removed songs" list.
        struct BeatSaverSong {
            std::string key, name, mapper;
            std::vector<std::pair<std::string, std::string>> diffs;  // characteristic, difficulty
        };
        using BeatSaverMap = std::map<std::string, BeatSaverSong>;
        struct BeatSaverCheck {
            std::vector<std::string> hashes;
            size_t i = 0;
            BeatSaverMap found;
            std::function<void(int)> progress;
            std::function<void(bool, BeatSaverMap)> done;
        };
        void ReadBeatSaverMap(rapidjson::Value const& map, std::string const& hash, BeatSaverMap& found) {
            if (!map.IsObject() || !IsHash(hash))
                return;
            BeatSaverSong s;
            s.key = GetString(map, "id");
            auto meta = map.FindMember("metadata");
            if (meta != map.MemberEnd() && meta->value.IsObject()) {
                s.name = GetString(meta->value, "songName");
                s.mapper = GetString(meta->value, "levelAuthorName");
            }
            if (s.name.empty())
                s.name = GetString(map, "name");
            auto versions = map.FindMember("versions");
            if (versions != map.MemberEnd() && versions->value.IsArray())
                for (auto const& v : versions->value.GetArray()) {
                    if (Lower(GetString(v, "hash")) != hash)
                        continue;
                    auto diffs = v.FindMember("diffs");
                    if (diffs != v.MemberEnd() && diffs->value.IsArray())
                        for (auto const& d : diffs->value.GetArray())
                            if (!GetString(d, "difficulty").empty())
                                s.diffs.emplace_back(GetString(d, "characteristic"), GetString(d, "difficulty"));
                }
            found[hash] = std::move(s);
        }
        void CheckNext(std::shared_ptr<BeatSaverCheck> c) {
            if (c->i >= c->hashes.size()) {
                c->done(true, std::move(c->found));
                return;
            }
            size_t n = std::min<size_t>(50, c->hashes.size() - c->i);
            std::string list;
            for (size_t k = 0; k < n; k++)
                list += (k ? "," : "") + c->hashes[c->i + k];
            Get("https://api.beatsaver.com/maps/hash/" + list, "", [c, n](Response r) {
                // 404: none of these are on BeatSaver anymore.
                if (r.httpCode != 404) {
                    rapidjson::Document doc;
                    if (!r.IsSuccessful() || !ParseJson(r, doc)) {
                        c->done(false, {});
                        return;
                    }
                    if (doc.HasMember("versions"))  // one song comes back as the map itself
                        ReadBeatSaverMap(doc, c->hashes[c->i], c->found);
                    else
                        for (auto const& m : doc.GetObject())
                            ReadBeatSaverMap(m.value, Lower(m.name.GetString()), c->found);
                }
                c->i += n;
                c->progress((int) c->i);
                CheckNext(c);
            });
        }
        // Calls done(ok, songs BeatSaver has) on the main thread; ok is false if BeatSaver couldn't be reached.
        void CheckBeatSaver(std::vector<std::string> hashes, std::function<void(int)> progress, std::function<void(bool, BeatSaverMap)> done) {
            auto c = std::make_shared<BeatSaverCheck>();
            c->hashes = std::move(hashes);
            c->progress = std::move(progress);
            c->done = std::move(done);
            CheckNext(c);
        }

        // ---- song select's playlist list ----
        // Song select builds its playlist list when it opens and keeps it. PlaylistCore's reload swaps in new
        // playlists underneath without touching that list, so playlists a sync added or deleted only showed
        // after a trip to the main menu. Rebuild it the way SongCore does after a song refresh, keeping the
        // open playlist, the selected song and the scroll spot.
        std::vector<UnityW<GlobalNamespace::LevelSelectionNavigationController>> songSelects;
        bool listStale = false, listWaiting = false;

        void RebuildList(GlobalNamespace::LevelSelectionNavigationController* select) {
            using Category = GlobalNamespace::SelectLevelCategoryViewController::LevelCategory;
            auto nav = select->_levelFilteringNavigationController;
            if (!nav)
                return;
            auto collection = select->_levelCollectionNavigationController;
            auto levels = collection ? collection->_levelCollectionViewController : nullptr;
            auto table = levels ? levels->_levelCollectionTableView : nullptr;
            auto level = table ? table->_selectedBeatmapLevel : nullptr;
            std::string levelId = level ? std::string(level->levelID) : "";
            auto tableView = table ? table->_tableView : nullptr;
            auto scroll = tableView ? tableView->_scrollView : nullptr;
            float position = scroll ? scroll->position : 0;

            auto category = nav->selectedLevelCategory;
            if (category == Category::MusicPacks || category == Category::CustomSongs) {
                if (auto pack = nav->selectedBeatmapLevelPack) {
                    // Reopen the same playlist if it's still there; a deleted one falls back to the first.
                    if (nav->_beatmapLevelsModel && nav->_beatmapLevelsModel->GetLevelPack(pack->packID))
                        nav->_levelPackIdToBeSelectedAfterPresent = pack->packID;
                    else
                        position = 0;
                }
            }
            nav->_customLevelPacks = nullptr;
            if (nav->_annotatedBeatmapLevelCollectionsViewController)
                nav->_annotatedBeatmapLevelCollectionsViewController->ShowLoading();
            if (collection)
                collection->ShowLoading();
            nav->UpdateCustomSongs();

            UnityW<GlobalNamespace::LevelSelectionNavigationController> keep = select;
            BSML::MainThreadScheduler::ScheduleUntil(
                [keep]() { return !keep || !keep->_levelFilteringNavigationController || keep->_levelFilteringNavigationController->_cancellationTokenSource == nullptr; },
                [keep, levelId, position]() {
                    if (!keep || !keep->_levelFilteringNavigationController)
                        return;
                    auto collection = keep->_levelCollectionNavigationController;
                    auto levels = collection ? collection->_levelCollectionViewController : nullptr;
                    auto table = levels ? levels->_levelCollectionTableView : nullptr;
                    auto model = keep->_levelFilteringNavigationController->_beatmapLevelsModel;
                    auto level = (model && !levelId.empty()) ? model->GetBeatmapLevel(levelId) : nullptr;
                    if (level && table)
                        table->SelectLevel(level);
                    auto tableView = table ? table->_tableView : nullptr;
                    auto scroll = tableView ? tableView->_scrollView : nullptr;
                    if (scroll)
                        scroll->ScrollTo(position, false);
                });
        }

        // Rebuild now if song select is showing. If it's only hidden (a song is playing, or the results
        // screen is up), rebuild when it comes back. Opening it from the main menu builds a fresh list anyway.
        void CheckList() {
            if (!listStale)
                return;
            bool coming = false;
            for (auto& s : songSelects) {
                if (!s)
                    continue;
                if (s->get_isActivated() && s->get_isActiveAndEnabled()) {
                    listStale = false;
                    RebuildList(s.ptr());
                    return;
                }
                coming = coming || s->get_isInViewControllerHierarchy();
            }
            if (!coming) {
                listStale = false;
                return;
            }
            if (listWaiting)
                return;
            listWaiting = true;
            BSML::MainThreadScheduler::ScheduleAfterTime(1.0f, []() {
                listWaiting = false;
                CheckList();
            });
        }

        // PlaylistCore picks up the playlist files on disk, then song select shows them.
        void ReloadPlaylists() {
            PlaylistCore::ReloadPlaylists(false);
            songSelects.clear();
            for (auto s : UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::LevelSelectionNavigationController*>())
                if (s)
                    songSelects.emplace_back(s);
            listStale = true;
            CheckList();
        }

        // Delete song folders one at a time through SongCore (it deletes on a worker thread), then rescan
        // once. done(n) gets how many folders are gone.
        void DeleteSongFolders(std::shared_ptr<std::vector<std::string>> paths, size_t i, std::function<void(int)> progress, std::function<void(int)> done) {
            if (i >= paths->size()) {
                int gone = 0;
                std::error_code ec;
                for (auto const& p : *paths)
                    if (!std::filesystem::exists(p, ec))
                        gone++;
                auto refresh = SongCore::API::Loading::RefreshSongs(false);
                BSML::MainThreadScheduler::ScheduleUntil(
                    [refresh]() { return refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready; },
                    [done, gone]() {
                        ReloadPlaylists();
                        done(gone);
                    });
                return;
            }
            auto job = std::make_shared<std::future<void>>(SongCore::API::Loading::DeleteSong(std::filesystem::path((*paths)[i])));
            BSML::MainThreadScheduler::ScheduleUntil(
                [job]() {
                    if (!job->valid())
                        return true;
                    auto st = job->wait_for(std::chrono::seconds(0));
                    if (st == std::future_status::deferred)
                        job->wait();  // not started on its own thread: run it here
                    return st != std::future_status::timeout;
                },
                [paths, i, progress, done]() {
                    progress((int) i + 1);
                    DeleteSongFolders(paths, i + 1, progress, done);
                });
        }

        // Adds up folder sizes on a worker thread; done runs on the main thread.
        void SizeOf(std::vector<std::string> paths, std::function<void(int64_t)> done) {
            std::thread([paths = std::move(paths), done = std::move(done)]() {
                int64_t total = 0;
                for (auto const& p : paths) {
                    std::error_code ec;
                    for (auto it = std::filesystem::recursive_directory_iterator(p, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                        std::error_code fec;
                        if (it->is_regular_file(fec))
                            total += (int64_t) it->file_size(fec);
                    }
                }
                BSML::MainThreadScheduler::Schedule([done, total]() { done(total); });
            }).detach();
        }

        // What Clean up deletes, saved as a playlist backup: Studio lists it as "Removed songs" under
        // Recently deleted, and restoring it writes the playlist back so the songs download again.
        void SaveRemovedList(std::vector<std::string> const& hashes, BeatSaverMap const& info) {
            static constexpr char const* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_r(&t, &tm);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M", &tm);
            std::string day = std::string(months[tm.tm_mon % 12]) + " " + std::to_string(tm.tm_mday);

            rapidjson::Document doc(rapidjson::kObjectType);
            auto& a = doc.GetAllocator();
            doc.AddMember("playlistTitle", rapidjson::Value(("Removed songs (" + day + ")").c_str(), a), a);
            doc.AddMember("playlistAuthor", rapidjson::Value("Studio Sync", a), a);
            doc.AddMember("playlistDescription", rapidjson::Value("Songs Clean up deleted because no playlist used them. Restore this in Playlist Studio to download them again.", a), a);
            rapidjson::Value songs(rapidjson::kArrayType);
            for (auto const& h : hashes) {
                rapidjson::Value s(rapidjson::kObjectType);
                s.AddMember("hash", rapidjson::Value(h.c_str(), a), a);
                if (auto it = info.find(h); it != info.end()) {
                    auto const& b = it->second;
                    if (!b.key.empty())
                        s.AddMember("key", rapidjson::Value(b.key.c_str(), a), a);
                    if (!b.name.empty())
                        s.AddMember("songName", rapidjson::Value(b.name.c_str(), a), a);
                    if (!b.mapper.empty())
                        s.AddMember("levelAuthorName", rapidjson::Value(b.mapper.c_str(), a), a);
                    if (!b.diffs.empty()) {
                        rapidjson::Value ds(rapidjson::kArrayType);
                        for (auto const& [characteristic, difficulty] : b.diffs) {
                            rapidjson::Value d(rapidjson::kObjectType);
                            d.AddMember("characteristic", rapidjson::Value((characteristic.empty() ? std::string("Standard") : characteristic).c_str(), a), a);
                            d.AddMember("name", rapidjson::Value(difficulty.c_str(), a), a);
                            ds.PushBack(d, a);
                        }
                        s.AddMember("difficulties", ds, a);
                    }
                }
                songs.PushBack(s, a);
            }
            doc.AddMember("songs", songs, a);
            Backup(std::string("Removed songs ") + stamp + ".bplist", ToJson(doc));
        }

        // Clean up songs: what the last scan found to delete. A new scan bumps looseScan, so a slow BeatSaver
        // answer from an older one is dropped.
        std::vector<std::string> looseHashes, loosePaths;
        BeatSaverMap looseInfo;
        int looseScan = 0;

        // ---- the sync pipeline: edits -> Studio playlists -> inventory -> finish ----

        void ApplyItems(RunPtr run);
        void SendInventory(RunPtr run);
        void RefreshStats(bool force);
        void StudioCleanup(std::string id, std::string token);
        void StudioGetMissing(std::string id, std::vector<std::string> files, bool all, std::string token);

        // ---- downloading songs a synced playlist needs ----
        // PlaylistCore does the BeatSaver lookups and installs (the same code as PlaylistManager's
        // download button); SongCore then rescans so the new maps show up in the playlist.

        struct DownloadJob {
            std::vector<std::string> paths;
            std::string base;  // the sync's own status, kept in front of the download progress
            size_t i = 0;
            int waits = 0, wanted = 0;
        };
        using JobPtr = std::shared_ptr<DownloadJob>;

        int MissingIn(std::vector<std::string> const& paths) {
            int n = 0;
            for (auto const& path : paths)
                if (auto p = PlaylistCore::GetPlaylist(path); p && p->playlistCS)
                    n += PlaylistCore::PlaylistHasMissingSongs(p);
            return n;
        }

        // Run a sync that was asked for (or skipped) mid-sync, once this one, its song downloads and any
        // clean up are done.
        void RunQueued() {
            if (!(syncAgain || missedSync) || syncing || downloading || cleaning)
                return;
            syncAgain = missedSync = false;
            BSML::MainThreadScheduler::ScheduleAfterTime(0.5f, []() { SyncNow(); });
        }

        void DownloadsDone(JobPtr job) {
            if (job->wanted == 0) {
                activity = {};
                downloading = false;
                if (job->base.empty())
                    SetStatus("Every playlist already has all its songs.");
                else
                    Notify();
                CountMissingSongs();
                RefreshStats(false);
                RunQueued();
                return;
            }
            SetStatus((job->base.empty() ? std::string("Getting missing songs") : job->base) + ". Loading new songs...");
            auto refresh = SongCore::API::Loading::RefreshSongs(false);
            // Wait for SongCore's rescan, then give PlaylistCore a moment to rebuild the playlists.
            BSML::MainThreadScheduler::ScheduleUntil(
                [refresh]() { return refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready; },
                [job]() {
                    BSML::MainThreadScheduler::ScheduleAfterTime(1.5f, [job]() {
                        int left = std::min(job->wanted, MissingIn(job->paths));
                        summary.downloadedLast = job->wanted - left;
                        summary.notFoundLast = left;
                        std::string text = (job->base.empty() ? "" : job->base + ", ") + std::to_string(summary.downloadedLast) + " song" + (summary.downloadedLast == 1 ? "" : "s") + " downloaded";
                        if (left)
                            text += ". " + std::to_string(left) + " couldn't be found on BeatSaver";
                        activity = {};
                        downloading = false;
                        SetStatus(text);
                        CountMissingSongs();
                        RefreshStats(true);
                        RunQueued();
                    });
                });
        }

        void DownloadNext(JobPtr job) {
            if (job->i >= job->paths.size()) {
                DownloadsDone(job);
                return;
            }
            auto playlist = PlaylistCore::GetPlaylist(job->paths[job->i]);
            if (!playlist || !playlist->playlistCS) {
                // A playlist written this sync can take a moment to load; skip it after ~10 seconds.
                if (++job->waits <= 10) {
                    BSML::MainThreadScheduler::ScheduleAfterTime(1.0f, [job]() { DownloadNext(job); });
                    return;
                }
                job->waits = 0;
                job->i++;
                DownloadNext(job);
                return;
            }
            job->waits = 0;
            int missing = PlaylistCore::PlaylistHasMissingSongs(playlist);
            if (missing <= 0) {
                job->i++;
                DownloadNext(job);
                return;
            }
            job->wanted += missing;
            std::string name = playlist->name;
            activity.kind = Activity::Kind::Downloading;
            activity.playlist = name;
            activity.done = 0;
            activity.total = missing;
            activity.list = (int) job->i + 1;
            activity.lists = (int) job->paths.size();
            Notify();
            PlaylistCore::DownloadMissingSongsFromPlaylist(
                playlist,
                [job, playlist]() {
                    PlaylistCore::MarkPlaylistForReload(playlist);
                    job->i++;
                    DownloadNext(job);
                },
                [job, name](int total, int done) {
                    activity.done = done;
                    activity.total = total;
                    SetStatus("Downloading songs for " + name + ": " + std::to_string(done) + " of " + std::to_string(total));
                });
        }

        void StartDownloads(std::vector<std::string> paths, std::string base) {
            if (downloading)
                return;
            std::sort(paths.begin(), paths.end());
            paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
            auto job = std::make_shared<DownloadJob>();
            job->paths = std::move(paths);
            job->base = std::move(base);
            downloading = true;
            Notify();
            // Let PlaylistCore finish loading what this sync just wrote.
            BSML::MainThreadScheduler::ScheduleAfterTime(1.0f, [job]() { DownloadNext(job); });
        }

        void Finish(RunPtr run) {
            syncing = false;
            if (run->changed && !run->reloaded)
                ReloadPlaylists();
            summary.fromStudio.clear();
            for (auto const& it : run->items)
                summary.fromStudio.push_back(it.title.empty() ? it.id : it.title);
            summary.editedLast = run->edited;
            summary.addedLast = run->created;
            summary.restoredLast = run->restored;
            summary.deletedLast = run->deleted;
            summary.shared = run->shared;
            summary.total = run->total;
            summary.lastSync = LocalTimeNow();
            std::string text = "Up to date";
            if (run->written || run->removed || run->reordered || !run->edited.empty() || !run->created.empty() || !run->restored.empty() || !run->deleted.empty()) {
                text = "Synced";
                if (!run->created.empty())
                    text += ", " + std::to_string(run->created.size()) + " added";
                if (run->written)
                    text += ", " + std::to_string(run->written) + " downloaded";
                if (!run->edited.empty())
                    text += ", " + std::to_string(run->edited.size()) + " edited";
                if (!run->restored.empty())
                    text += ", " + std::to_string(run->restored.size()) + " restored";
                if (!run->deleted.empty())
                    text += ", " + std::to_string(run->deleted.size()) + " deleted" + (run->songsDeleted ? " with " + std::to_string(run->songsDeleted) + " song" + (run->songsDeleted == 1 ? "" : "s") : "");
                if (run->removed)
                    text += ", " + std::to_string(run->removed) + " removed";
                if (run->reordered)
                    text += ", playlist order from Studio";
            }
            if (run->failed)
                text += ". " + std::to_string(run->failed) + " failed, will retry.";
            summary.downloadedLast = summary.notFoundLast = 0;
            summary.songsDeletedLast = run->songsDeleted;
            SetStatus(text);
            // Songs download only for playlists whose auto-download is on.
            std::vector<std::string> wanted;
            for (auto const& path : run->touched)
                if (PlaylistAutoDownload(std::filesystem::path(path).filename().string()))
                    wanted.push_back(path);
            if (!wanted.empty())
                StartDownloads(wanted, text);
            // Count once PlaylistCore has picked up what this sync wrote.
            BSML::MainThreadScheduler::ScheduleAfterTime(3.0f, []() { CountMissingSongs(); });
            // A sync that changed playlists changes what they're missing, which Studio shows per playlist.
            RefreshStats(run->changed);
            if (!run->cleanupId.empty())
                StudioCleanup(run->cleanupId, run->token);
            if (!run->missingId.empty())
                StudioGetMissing(run->missingId, run->missingFiles, run->missingAll, run->token);
            RunQueued();
        }

        void ApplyWrite(RunPtr run, size_t i) {
            if (i >= run->writes.size()) {
                ApplyItems(run);
                return;
            }
            auto w = run->writes[i];
            std::error_code ec;
            if (!ValidFile(w.file) || w.file.starts_with(FILE_PREFIX) || w.url != Server() + "/link/outbox/" + w.id) {
                run->failed++;
                ApplyWrite(run, i + 1);
                return;
            }
            bool exists = std::filesystem::exists(PathIn(w.file), ec);
            if (w.remove) {
                // Deleting never loses anything: the playlist becomes a backup Studio can restore for 30 days.
                auto path = PathIn(w.file);
                if (exists && !Excluded(w.file)) {
                    auto text = readfile(path);
                    Backup(w.file, text);
                    RemoveFile(path);
                    if (std::filesystem::exists(path, ec))
                        std::filesystem::remove(path, ec);
                    if (!std::filesystem::exists(path, ec)) {
                        rapidjson::Document doc;
                        auto title = ParseText(text, doc) ? GetString(doc, "playlistTitle") : "";
                        run->deleted.push_back(title.empty() ? w.file : title);
                        run->changed = true;
                        // Its songs go at the end of the sync, once every write is on disk (see DropSongs).
                        if (w.songs)
                            run->dropSongs.merge(ScanHashes(text));
                    } else {
                        run->failed++;
                        ApplyWrite(run, i + 1);
                        return;
                    }
                }
                Ack(run, w.id);
                ApplyWrite(run, i + 1);
                return;
            }
            if (!w.create && !exists && w.restore == 0) {
                // The playlist was deleted on the headset; nothing to edit or restore.
                logger.warn("Studio sent a change for {} but it's gone; dropping it", w.file);
                Ack(run, w.id);
                ApplyWrite(run, i + 1);
                return;
            }
            if (w.restore > 0) {
                auto bak = BackupDir() / (w.file + "." + std::to_string(w.restore) + ".bak");
                auto text = readfile(bak.string());
                rapidjson::Document doc;
                if (text.empty() || !ParseText(text, doc) || !IsPlaylist(doc)) {
                    logger.warn("Backup of {} from {} is gone; dropping the restore", w.file, w.restore);
                    Ack(run, w.id);
                    ApplyWrite(run, i + 1);
                    return;
                }
                auto path = PathIn(w.file);
                // The version being replaced becomes a backup too, so a restore can be undone.
                // A deleted playlist has nothing to replace; restoring just brings it back.
                if (exists)
                    Backup(w.file, readfile(path));
                if (writefile(path, text)) {
                    if (auto playlist = PlaylistCore::GetPlaylist(path))
                        PlaylistCore::MarkPlaylistForReload(playlist);
                    auto title = GetString(doc, "playlistTitle");
                    run->restored.push_back(title.empty() ? w.file : title);
                    run->touched.push_back(path);
                    run->changed = true;
                    Ack(run, w.id);
                } else
                    run->failed++;
                ApplyWrite(run, i + 1);
                return;
            }
            // A new playlist never overwrites one that's there: it takes the next free "(2)" name.
            if (w.create && exists) {
                auto name = FreeName(w.file);
                if (name.empty()) {
                    run->failed++;
                    ApplyWrite(run, i + 1);
                    return;
                }
                logger.info("{} already exists; saving Studio's new playlist as {}", w.file, name);
                w.savedAs = name;
            }
            auto path = PathIn(w.savedAs.empty() ? w.file : w.savedAs);
            Get(w.url, run->token, [run, i, w, path](Response r) {
                rapidjson::Document edit;
                bool ok = false;
                if (r.IsSuccessful() && r.responseData->size() <= MAX_PLAYLIST_BYTES && ParseJson(r, edit) && IsPlaylist(edit)) {
                    std::string text;
                    if (w.create)
                        text = ToJson(edit);
                    else {
                        auto original = readfile(path);
                        text = Merge(original, edit);
                        Backup(w.file, original);
                    }
                    if (writefile(path, text)) {
                        if (auto playlist = PlaylistCore::GetPlaylist(path))
                            PlaylistCore::MarkPlaylistForReload(playlist);
                        auto title = GetString(edit, "playlistTitle").empty() ? w.file : GetString(edit, "playlistTitle");
                        (w.create ? run->created : run->edited).push_back(title);
                        run->touched.push_back(path);
                        run->changed = true;
                        ok = true;
                        Ack(run, w.id, w.savedAs);
                    }
                }
                if (!ok) {
                    logger.error("Could not apply Studio edit for {} (http {})", w.file, r.httpCode);
                    run->failed++;
                }
                ApplyWrite(run, i + 1);
            });
        }

        // Playlists deleted in Studio with "also delete its songs": their songs that no playlist left on the
        // headset uses go too, except favorites and songs BeatSaver no longer has. The playlist is kept as a
        // backup, so restoring it downloads the songs again.
        void DropSongs(RunPtr run) {
            if (run->dropSongs.empty()) {
                SendInventory(run);
                return;
            }
            auto favorites = GetFavorites();
            if (!favorites || !SongCore::API::Loading::AreSongsLoaded() || SongCore::API::Loading::AreSongsRefreshing()) {
                logger.warn("Favorites or songs not ready; keeping the songs of deleted playlists");
                SendInventory(run);
                return;
            }
            auto used = SongsInPlaylists();
            std::vector<std::string> candidates;
            std::map<std::string, std::string> paths;
            for (auto const& h : run->dropSongs) {
                if (used.contains(h))
                    continue;
                if (auto level = Deletable(h, favorites)) {
                    candidates.push_back(h);
                    paths[h] = std::string(level->customLevelPath);
                }
            }
            if (candidates.empty()) {
                SendInventory(run);
                return;
            }
            SetStatus("Checking " + std::to_string(candidates.size()) + " songs on BeatSaver...");
            CheckBeatSaver(candidates, [](int) {}, [run, candidates, paths](bool ok, BeatSaverMap found) {
                if (!ok) {
                    logger.warn("BeatSaver unreachable; keeping the songs of deleted playlists");
                    SendInventory(run);
                    return;
                }
                auto list = std::make_shared<std::vector<std::string>>();
                for (auto const& h : candidates)
                    if (found.contains(h))
                        list->push_back(paths.at(h));
                if (list->empty()) {
                    SendInventory(run);
                    return;
                }
                auto total = list->size();
                DeleteSongFolders(
                    list, 0, [total](int n) { SetStatus("Deleting songs: " + std::to_string(n) + " of " + std::to_string(total)); },
                    [run](int gone) {
                        run->songsDeleted = gone;
                        statsDirty = true;
                        SendInventory(run);
                    });
            });
        }

        void Download(RunPtr run, std::shared_ptr<std::vector<Item>> queue, size_t i) {
            if (i >= queue->size()) {
                DropSongs(run);
                return;
            }
            auto item = (*queue)[i];
            Get(item.url, "", [run, queue, i, item](Response r) {
                rapidjson::Document doc;
                bool ok = false;
                if (r.IsSuccessful() && r.responseData->size() <= MAX_PLAYLIST_BYTES && ParseJson(r, doc) && IsPlaylist(doc)) {
                    auto path = PathFor(item.id);
                    if (writefile(path, *r.responseData)) {
                        if (auto playlist = PlaylistCore::GetPlaylist(path))
                            PlaylistCore::MarkPlaylistForReload(playlist);
                        auto synced = LoadSynced();
                        synced[item.id] = item.updated;
                        SaveSynced(synced);
                        ok = true;
                    }
                }
                if (ok) {
                    run->written++;
                    run->changed = true;
                } else {
                    logger.error("Could not download playlist {} (http {})", item.id, r.httpCode);
                    run->failed++;
                }
                Download(run, queue, i + 1);
            });
        }

        void ApplyItems(RunPtr run) {
            auto files = OurFiles();
            auto synced = LoadSynced();
            std::set<std::string> keep;
            for (auto const& it : run->items)
                keep.insert(it.id);
            // Remove only files this mod wrote that Studio no longer lists.
            for (auto const& [id, path] : files) {
                if (!keep.contains(id)) {
                    RemoveFile(path);
                    synced.erase(id);
                    run->removed++;
                    run->changed = true;
                }
            }
            SaveSynced(synced);
            auto queue = std::make_shared<std::vector<Item>>();
            for (auto const& it : run->items) {
                auto have = synced.find(it.id);
                if (!files.contains(it.id) || have == synced.end() || have->second != it.updated)
                    queue->push_back(it);
            }
            Download(run, queue, 0);
        }

        void UploadNeeded(RunPtr run, size_t i) {
            if (i >= run->need.size()) {
                Finish(run);
                return;
            }
            auto file = run->need[i];
            if (!ValidFile(file)) {
                UploadNeeded(run, i + 1);
                return;
            }
            Post(Server() + "/link/inventory/body?file=" + UrlEncode(file), run->token, readfile(PathIn(file)), [run, i, file](Response r) {
                if (!r.IsSuccessful())
                    logger.error("Could not share {} with Studio (http {})", file, r.httpCode);
                UploadNeeded(run, i + 1);
            });
        }

        // Studio's "My order", put in game: the playlists it lists take the spots they already hold in PlaylistCore's
        // order, sorted the way Studio has them. Everything else (Studio-hosted playlists, ones not shared, other
        // mods') keeps its place. PlaylistCore only moves one playlist at a time, so the listed ones go to the end of
        // the known order first, then each into its spot from the top; that gives exactly the order wanted.
        bool ApplyOrder(RunPtr run) {
            std::map<std::string, PlaylistCore::Playlist*> byFile;
            int last = -1;
            for (auto p : PlaylistCore::GetLoadedPlaylists()) {
                if (!p)
                    continue;
                int idx = PlaylistCore::GetPlaylistIndex(p->path, false);
                if (idx < 0)
                    continue;
                byFile[std::filesystem::path(p->path).filename().string()] = p;
                last = std::max(last, idx);
            }
            std::vector<PlaylistCore::Playlist*> want;
            std::set<PlaylistCore::Playlist*> seen;
            for (auto const& f : run->orderFiles)
                if (auto it = byFile.find(f); it != byFile.end() && seen.insert(it->second).second)
                    want.push_back(it->second);
            std::vector<int> slots;
            for (auto p : want)
                slots.push_back(PlaylistCore::GetPlaylistIndex(p->path, false));
            std::sort(slots.begin(), slots.end());
            bool inPlace = true;
            for (size_t k = 0; k < want.size(); k++)
                inPlace = inPlace && PlaylistCore::GetPlaylistIndex(want[k]->path, false) == slots[k];
            if (inPlace || last < 0)
                return false;
            for (auto p : want)
                PlaylistCore::MovePlaylist(p, last);
            for (size_t k = 0; k < want.size(); k++)
                if (PlaylistCore::GetPlaylistIndex(want[k]->path, false) != slots[k])
                    PlaylistCore::MovePlaylist(want[k], slots[k]);
            return true;
        }

        void PostInventory(RunPtr run);
        void SendInventory(RunPtr run) {
            if (run->orderT > 0 && run->orderT > getConfig().OrderApplied.GetValue()) {
                // Playlists this sync wrote load first, so the order can place them too.
                if (run->changed && !run->reloaded) {
                    ReloadPlaylists();
                    run->reloaded = true;
                }
                if (ApplyOrder(run)) {
                    ReloadPlaylists();
                    run->reloaded = true;
                    run->reordered = true;
                }
                getConfig().OrderApplied.SetValue((int) run->orderT);
            }
            PostInventory(run);
        }

        // Tell Studio which of the headset's own playlists exist; it asks for the ones it lacks.
        void PostInventory(RunPtr run) {
            rapidjson::Document body(rapidjson::kObjectType);
            auto& a = body.GetAllocator();
            rapidjson::Value list(rapidjson::kArrayType);
            std::error_code ec;
            run->shared = 0;
            run->total = 0;
            auto backups = AllBackups();
            std::set<std::string> present;
            // How many playlist files use each song (Studio's synced and unshared ones count), and the
            // songs of each shared playlist, for its "only this playlist uses" count.
            std::map<std::string, int> uses;
            std::vector<std::pair<rapidjson::SizeType, std::set<std::string>>> songsOf;
            for (auto const& entry : std::filesystem::directory_iterator(PlaylistsDir(), ec)) {
                auto name = entry.path().filename().string();
                if (!entry.is_regular_file() || !IsPlaylistFile(name))
                    continue;
                bool own = !name.starts_with(FILE_PREFIX) && ValidFile(name);
                std::error_code sec;
                bool shared = own && !Excluded(name) && entry.file_size(sec) <= MAX_PLAYLIST_BYTES;
                if (own) {
                    present.insert(name);
                    run->total++;
                }
                auto text = readfile(entry.path().string());
                auto hashes = ScanHashes(text);
                for (auto const& h : hashes)
                    uses[h]++;
                if (!shared)
                    continue;
                rapidjson::Document doc;
                if (!ParseText(text, doc) || !IsPlaylist(doc))
                    continue;
                rapidjson::Value it(rapidjson::kObjectType);
                it.AddMember("file", rapidjson::Value(name.c_str(), a), a);
                it.AddMember("title", rapidjson::Value(GetString(doc, "playlistTitle").c_str(), a), a);
                it.AddMember("count", (int) doc["songs"].Size(), a);
                it.AddMember("maps", MapCount(doc), a);
                it.AddMember("sha", rapidjson::Value(Fingerprint(text).c_str(), a), a);
                it.AddMember("auto", PlaylistAutoDownload(name), a);
                // Backups Studio can offer to restore.
                rapidjson::Value baks(rapidjson::kArrayType);
                if (auto found = backups.find(name); found != backups.end())
                    for (auto const& b : found->second) {
                        rapidjson::Value bv(rapidjson::kObjectType);
                        bv.AddMember("t", b.time, a);
                        bv.AddMember("count", b.count, a);
                        if (b.maps >= 0)
                            bv.AddMember("maps", b.maps, a);
                        baks.PushBack(bv, a);
                    }
                it.AddMember("backups", baks, a);
                songsOf.emplace_back(list.Size(), std::move(hashes));
                list.PushBack(it, a);
                run->shared++;
            }
            // Songs only this playlist uses that are downloaded and not favorites: the most that deleting it
            // with "also delete its songs" can remove (songs BeatSaver lacks stay too).
            if (SongCore::API::Loading::AreSongsLoaded() && !SongCore::API::Loading::AreSongsRefreshing()) {
                auto favorites = GetFavorites();
                for (auto& [index, hashes] : songsOf) {
                    int solo = 0;
                    for (auto const& h : hashes)
                        if (uses[h] == 1 && Deletable(h, favorites))
                            solo++;
                    list[index].AddMember("solo", solo, a);
                }
            }
            body.AddMember("items", list, a);

            // Playlists that are gone but still have backups: Studio lists them as recently deleted.
            // After 30 days their backups are cleared out.
            std::vector<std::pair<std::string, std::vector<BackupInfo> const*>> gone;
            auto cutoff = (int64_t) std::time(nullptr) - KEEP_DELETED_SECONDS;
            for (auto const& [file, list] : backups) {
                if (present.contains(file) || list.empty() || !ValidFile(file))
                    continue;
                if (list.front().time < cutoff) {
                    for (auto const& b : list)
                        std::filesystem::remove(b.path, ec);
                    continue;
                }
                if (!Excluded(file))
                    gone.emplace_back(file, &list);
            }
            std::sort(gone.begin(), gone.end(), [](auto const& x, auto const& y) { return x.second->front().time > y.second->front().time; });
            rapidjson::Value deleted(rapidjson::kArrayType);
            for (size_t g = 0; g < gone.size() && g < MAX_DELETED_LISTED; g++) {
                auto const& [file, list] = gone[g];
                rapidjson::Value dv(rapidjson::kObjectType);
                dv.AddMember("file", rapidjson::Value(file.c_str(), a), a);
                dv.AddMember("title", rapidjson::Value(list->front().title.c_str(), a), a);
                rapidjson::Value baks(rapidjson::kArrayType);
                for (auto const& b : *list) {
                    rapidjson::Value bv(rapidjson::kObjectType);
                    bv.AddMember("t", b.time, a);
                    bv.AddMember("count", b.count, a);
                    if (b.maps >= 0)
                        bv.AddMember("maps", b.maps, a);
                    baks.PushBack(bv, a);
                }
                dv.AddMember("backups", baks, a);
                deleted.PushBack(dv, a);
            }
            body.AddMember("deleted", deleted, a);
            // What this version of the mod can do, so Studio only offers what it can carry out.
            rapidjson::Value features(rapidjson::kArrayType);
            features.PushBack("restore", a);
            features.PushBack("delete", a);
            features.PushBack("delete-songs", a);
            features.PushBack("storage", a);
            features.PushBack("cleanup", a);
            features.PushBack("order", a);
            features.PushBack("missing", a);
            features.PushBack("autodl", a);
            body.AddMember("features", features, a);
            // The auto-download change from Studio it last took, so Studio stops showing it as waiting.
            if (int applied = getConfig().AutoApplied.GetValue(); applied > 0)
                body.AddMember("autodl", applied, a);
            // The "My order" it last put in game, so Studio stops showing it as waiting.
            if (int applied = getConfig().OrderApplied.GetValue(); applied > 0)
                body.AddMember("order", applied, a);
            Post(Server() + "/link/inventory", run->token, ToJson(body), [run](Response r) {
                rapidjson::Document doc;
                if (r.IsSuccessful() && ParseJson(r, doc) && doc.HasMember("need") && doc["need"].IsArray())
                    for (auto const& v : doc["need"].GetArray())
                        if (v.IsString())
                            run->need.push_back(v.GetString());
                UploadNeeded(run, 0);
            });
        }

        // ---- headset storage, for the card in Studio ----

        struct StatsWork {
            std::vector<std::pair<std::string, int>> folders;  // song folder, and: 0 in a playlist, 1 in none, 2 in none but a favorite, 3 WIP
            std::string game, disk;
            std::vector<std::pair<std::string, int>> missingBy;  // playlist file, songs it lists that aren't downloaded
            int missing = 0;                                     // those songs, counted once
        };

        int64_t FolderBytes(std::string const& path) {
            int64_t total = 0;
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(path, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code fec;
                if (it->is_regular_file(fec))
                    total += (int64_t) it->file_size(fec);
            }
            return total;
        }

        // Songs, how much space they take, which ones no playlist uses, and the headset's free space. The folder
        // list comes from SongCore on the main thread; adding up sizes happens on a worker thread. Skipped while
        // a sync, download or clean up runs (it's asked for again when they finish).
        void RefreshStats(bool force) {
            if (force)
                statsDirty = true;
            if (!IsLinked() || statsRunning || syncing || downloading || cleaning)
                return;
            if (Playing()) {
                statsDirty = true;  // the next sync, back in the menu, picks it up
                return;
            }
            auto now = std::chrono::steady_clock::now();
            if (!statsDirty && statsAt.time_since_epoch().count() != 0 && now - statsAt < std::chrono::seconds(STATS_EVERY_SECONDS))
                return;
            if (!SongCore::API::Loading::AreSongsLoaded() || SongCore::API::Loading::AreSongsRefreshing())
                return;
            auto favorites = GetFavorites();
            auto work = std::make_shared<StatsWork>();
            // Every playlist's songs in one pass: which songs any playlist uses, and what each one is missing.
            std::set<std::string> used, missingAll;
            {
                std::error_code ec;
                for (auto const& entry : std::filesystem::directory_iterator(PlaylistsDir(), ec)) {
                    auto name = entry.path().filename().string();
                    if (!entry.is_regular_file() || !IsPlaylistFile(name))
                        continue;
                    int n = 0;
                    for (auto const& h : ScanHashes(readfile(entry.path().string()))) {
                        if (!SongCore::API::Loading::GetLevelByHash(h)) {
                            n++;
                            missingAll.insert(h);
                        }
                        used.insert(h);
                    }
                    if (n > 0)
                        work->missingBy.emplace_back(name, n);
                }
            }
            work->missing = (int) missingAll.size();
            std::sort(work->missingBy.begin(), work->missingBy.end(), [](auto const& x, auto const& y) { return x.second > y.second; });
            std::set<std::string> seenPaths;
            for (auto level : SongCore::API::Loading::GetAllLevels()) {
                if (!level)
                    continue;
                std::string path(level->customLevelPath);
                if (path.empty() || !seenPaths.insert(path).second)
                    continue;
                int kind = 0;
                if (IsWip(level))
                    kind = 3;
                else if (auto hash = HashOfLevelId(std::string(level->levelID)); !hash.empty() && !used.contains(hash))
                    kind = favorites && favorites->Contains(level->levelID) ? 2 : 1;
                work->folders.emplace_back(std::move(path), kind);
            }
            work->game = std::string(UnityEngine::Application::get_version());
            if (auto cut = work->game.find('_'); cut != std::string::npos)
                work->game.erase(cut);
            work->disk = getDataDir(std::string_view("StudioSync"));
            statsRunning = true;
            statsDirty = false;
            statsAt = now;
            std::thread([work]() {
                int64_t bytes[4] = {0, 0, 0, 0};
                int counts[4] = {0, 0, 0, 0};
                for (auto const& [path, kind] : work->folders) {
                    bytes[kind] += FolderBytes(path);
                    counts[kind]++;
                }
                int64_t total = -1, freeBytes = -1;
                struct statvfs fs{};
                if (statvfs(work->disk.c_str(), &fs) == 0) {
                    total = (int64_t) fs.f_blocks * (int64_t) fs.f_frsize;
                    freeBytes = (int64_t) fs.f_bavail * (int64_t) fs.f_frsize;
                }
                rapidjson::Document body(rapidjson::kObjectType);
                auto& a = body.GetAllocator();
                body.AddMember("songs", counts[0] + counts[1] + counts[2], a);
                body.AddMember("songBytes", bytes[0] + bytes[1] + bytes[2], a);
                body.AddMember("loose", counts[1] + counts[2], a);
                body.AddMember("looseBytes", bytes[1] + bytes[2], a);
                body.AddMember("looseFav", counts[2], a);
                body.AddMember("looseFavBytes", bytes[2], a);
                body.AddMember("wip", counts[3], a);
                body.AddMember("wipBytes", bytes[3], a);
                body.AddMember("total", total, a);
                body.AddMember("free", freeBytes, a);
                body.AddMember("mod", rapidjson::Value(VERSION, a), a);
                body.AddMember("game", rapidjson::Value(work->game.c_str(), a), a);
                body.AddMember("missing", work->missing, a);
                // Most missing first, kept under the Worker's size limit for this report (older Workers allow 4000 bytes).
                rapidjson::Value by(rapidjson::kArrayType);
                size_t room = 3400 > ToJson(body).size() ? 3400 - ToJson(body).size() : 0;
                for (auto const& [file, n] : work->missingBy) {
                    size_t need = file.size() + 24;
                    if (need > room)
                        break;
                    room -= need;
                    rapidjson::Value e(rapidjson::kArrayType);
                    e.PushBack(rapidjson::Value(file.c_str(), a), a);
                    e.PushBack(n, a);
                    by.PushBack(e, a);
                }
                body.AddMember("missingBy", by, a);
                auto text = ToJson(body);
                BSML::MainThreadScheduler::Schedule([text]() {
                    statsRunning = false;
                    auto token = getConfig().Token.GetValue();
                    if (token.empty())
                        return;
                    Post(Server() + "/link/stats", token, text, [](Response r) {
                        if (!r.IsSuccessful())
                            logger.warn("Could not send headset storage to Studio (http {})", r.httpCode);
                    });
                });
            }).detach();
        }

        // ---- song clean up asked for in Studio ----
        // The same Clean up as the button in the tab: songs in no playlist go, favorites, WIP maps and songs BeatSaver
        // no longer has stay, and what's deleted is saved as a "Removed songs" backup Studio can restore. It runs
        // once nothing else is going on. If it can't run yet (songs still loading, favorites not read, BeatSaver down)
        // it's left for the next sync; Studio keeps showing it as waiting.

        void AckCleanup(std::string const& id, std::string const& token, std::string const& body) {
            Post(Server() + "/link/cleanup/" + id + "/ack", token, body, [id](Response r) {
                if (!r.IsSuccessful())
                    logger.warn("Could not tell Studio clean up {} is done (http {})", id, r.httpCode);
            });
        }

        void CleanupFinished(std::string const& id, std::string const& token, int deleted, int64_t bytes, int kept) {
            rapidjson::Document d(rapidjson::kObjectType);
            auto& a = d.GetAllocator();
            d.AddMember("deleted", deleted, a);
            d.AddMember("bytes", std::max<int64_t>(0, bytes), a);
            d.AddMember("kept", kept, a);
            auto body = ToJson(d);
            getConfig().CleanupDone.SetValue(id);
            getConfig().CleanupResult.SetValue(body);
            cleanupRunning.clear();
            AckCleanup(id, token, body);
            RefreshStats(true);
        }

        void StudioCleanup(std::string id, std::string token) {
            if (id == getConfig().CleanupDone.GetValue()) {
                // Done already; Studio didn't hear about it.
                auto result = getConfig().CleanupResult.GetValue();
                AckCleanup(id, token, result.empty() ? "{}" : result);
                return;
            }
            if (!cleanupRunning.empty())
                return;
            cleanupRunning = id;
            BSML::MainThreadScheduler::ScheduleUntil([]() { return !syncing && !downloading && !cleaning && !Playing(); }, [id, token]() {
                if (token != getConfig().Token.GetValue() || loose.state == LooseSongs::State::Deleting) {
                    cleanupRunning.clear();
                    return;
                }
                SetStatus("Playlist Studio asked for a song clean up. Looking for songs in no playlist...");
                ScanLooseSongs();
                int scan = looseScan;
                using State = LooseSongs::State;
                BSML::MainThreadScheduler::ScheduleUntil(
                    // Done scanning, the size of what goes is added up, or the scan was dropped (the tab's Clean up closed).
                    [scan]() { return scan != looseScan || loose.state == State::Failed || loose.state == State::Idle || (loose.state == State::Ready && (loose.toDelete == 0 || loose.bytes >= 0)); },
                    [id, token, scan]() {
                        if (scan != looseScan || loose.state != State::Ready) {
                            if (scan == looseScan && loose.state == State::Failed)
                                SetStatus("Song clean up from Playlist Studio waits for the next sync. " + loose.message);
                            cleanupRunning.clear();
                            return;
                        }
                        int kept = loose.favorites + loose.notOnBeatSaver;
                        if (loose.toDelete == 0) {
                            SetStatus(loose.found == 0 ? "Every song is in a playlist. Nothing to clean up." : "None of the songs in no playlist can be deleted.");
                            CleanupFinished(id, token, 0, 0, kept);
                            return;
                        }
                        int64_t bytes = loose.bytes;
                        // Checking BeatSaver takes a moment; if a song started meanwhile, delete once it's over.
                        BSML::MainThreadScheduler::ScheduleUntil([]() { return !Playing() && !syncing && !downloading && !cleaning; }, [id, token, scan, bytes, kept]() {
                            if (scan != looseScan || loose.state != State::Ready || token != getConfig().Token.GetValue()) {
                                cleanupRunning.clear();
                                return;
                            }
                            DeleteLooseSongs();
                            if (loose.state != State::Deleting) {
                                cleanupRunning.clear();
                                return;
                            }
                            BSML::MainThreadScheduler::ScheduleUntil([]() { return loose.state != State::Deleting; }, [id, token, bytes, kept]() {
                                if (loose.state == State::Done)
                                    CleanupFinished(id, token, loose.progress, bytes, kept);
                                else
                                    cleanupRunning.clear();
                            });
                        });
                    });
            });
        }

        // ---- Get missing songs asked for in Studio ----
        // The same download as Get missing in the tab, for the playlists Studio picked (or all of them). It runs once
        // nothing else is going on and no song is playing; Studio keeps showing it as waiting until it's done.

        void AckMissing(std::string const& id, std::string const& token, std::string const& body) {
            Post(Server() + "/link/getmissing/" + id + "/ack", token, body, [id](Response r) {
                if (!r.IsSuccessful())
                    logger.warn("Could not tell Studio get missing {} is done (http {})", id, r.httpCode);
            });
        }

        void MissingFinished(std::string const& id, std::string const& token, int downloaded, int notFound) {
            rapidjson::Document d(rapidjson::kObjectType);
            auto& a = d.GetAllocator();
            d.AddMember("downloaded", downloaded, a);
            d.AddMember("notFound", notFound, a);
            auto body = ToJson(d);
            getConfig().MissingDone.SetValue(id);
            getConfig().MissingResult.SetValue(body);
            missingRunning.clear();
            AckMissing(id, token, body);
            RefreshStats(true);
        }

        void StudioGetMissing(std::string id, std::vector<std::string> files, bool all, std::string token) {
            if (id == getConfig().MissingDone.GetValue()) {
                // Done already; Studio didn't hear about it.
                auto result = getConfig().MissingResult.GetValue();
                AckMissing(id, token, result.empty() ? "{}" : result);
                return;
            }
            if (!missingRunning.empty())
                return;
            missingRunning = id;
            BSML::MainThreadScheduler::ScheduleUntil([]() { return !syncing && !downloading && !cleaning && !Playing() && PlaylistCore::hasLoaded; }, [id, files, all, token]() {
                if (token != getConfig().Token.GetValue()) {
                    missingRunning.clear();
                    return;
                }
                std::set<std::string> only(files.begin(), files.end());
                std::vector<std::string> paths;
                for (auto playlist : PlaylistCore::GetLoadedPlaylists())
                    if (playlist && playlist->playlistCS && (all || only.contains(std::filesystem::path(playlist->path).filename().string())) &&
                        PlaylistCore::PlaylistHasMissingSongs(playlist) > 0)
                        paths.push_back(playlist->path);
                summary.downloadedLast = summary.notFoundLast = 0;
                if (paths.empty()) {
                    MissingFinished(id, token, 0, 0);
                    return;
                }
                SetStatus("Playlist Studio asked for missing songs...");
                StartDownloads(paths, "");
                BSML::MainThreadScheduler::ScheduleUntil([]() { return !downloading; }, [id, token]() {
                    MissingFinished(id, token, summary.downloadedLast, summary.notFoundLast);
                });
            });
        }

        // ---- the minute check in the menu ----

        void MenuCheck() {
            BSML::MainThreadScheduler::ScheduleAfterTime(60.0f, []() {
                MenuCheck();
                auto token = getConfig().Token.GetValue();
                if (token.empty() || pingOff || !pendingToken.empty() || syncing || downloading || cleaning || Playing())
                    return;
                // A sync just ran (or one is due from the main menu anyway).
                if (lastSync.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() - lastSync < std::chrono::seconds(50))
                    return;
                Get(Server() + "/link/ping", token, [token](Response r) {
                    if (token != getConfig().Token.GetValue())
                        return;
                    if (r.httpCode == 404) {
                        // An older Worker without the check: syncs happen the old way (going back to the main menu).
                        pingOff = true;
                        logger.info("Playlist Studio doesn't answer the menu check; syncing on the main menu only");
                        return;
                    }
                    rapidjson::Document doc;
                    if (!r.IsSuccessful() || !ParseJson(r, doc))
                        return;
                    auto chg = doc.FindMember("chg");
                    if (chg != doc.MemberEnd() && chg->value.IsInt64() && chg->value.GetInt64() > knownChg && !Playing())
                        SyncNow(20);
                });
            });
        }

        // ---- linking ----

        void PollLink(std::string token);
        void ScheduleLinkPoll(std::string token) {
            BSML::MainThreadScheduler::ScheduleAfterTime(LINK_POLL_SECONDS, [token]() { PollLink(token); });
        }
        void PollLink(std::string token) {
            if (token != pendingToken)
                return;  // cancelled or replaced
            if (std::chrono::steady_clock::now() - linkStarted > std::chrono::seconds(LINK_GIVE_UP_SECONDS)) {
                pendingToken.clear();
                code.clear();
                SetStatus("That code expired. Press Link to get a new one.");
                return;
            }
            Get(Server() + "/link/status?code=" + code, token, [token](Response r) {
                if (token != pendingToken)
                    return;
                rapidjson::Document doc;
                if (!r.IsSuccessful() || !ParseJson(r, doc)) {
                    ScheduleLinkPoll(token);  // network blip, keep trying until the code runs out
                    return;
                }
                auto state = GetString(doc, "status");
                if (state == "linked") {
                    getConfig().Token.SetValue(token);
                    SaveSynced({});
                    getConfig().OrderApplied.SetValue(0);
                    pendingToken.clear();
                    code.clear();
                    SetStatus("Linked. Syncing...");
                    SyncNow();
                } else if (state == "waiting") {
                    ScheduleLinkPoll(token);
                } else {
                    pendingToken.clear();
                    code.clear();
                    SetStatus("That code expired. Press Link to get a new one.");
                }
            });
        }
    }

    bool IsLinked() {
        return !getConfig().Token.GetValue().empty();
    }
    bool IsLinking() {
        return !pendingToken.empty();
    }
    bool IsSyncing() {
        return syncing || downloading || cleaning;
    }
    bool SyncQueued() {
        return syncAgain;
    }

    int MissingSongs() {
        return std::max(0, summary.missing);
    }
    // Songs any playlist on the headset lists that it doesn't have, counted once each.
    void CountMissingSongs() {
        if (!PlaylistCore::hasLoaded || !SongCore::API::Loading::AreSongsLoaded() || SongCore::API::Loading::AreSongsRefreshing())
            return;
        std::set<std::string> missing;
        for (auto playlist : PlaylistCore::GetLoadedPlaylists()) {
            if (!playlist)
                continue;
            for (auto const& song : playlist->playlistJSON.Songs) {
                auto hash = song.Hash ? Lower(*song.Hash) : HashOfLevelId(song.LevelID);
                if (IsHash(hash) && !SongCore::API::Loading::GetLevelByHash(hash))
                    missing.insert(hash);
            }
        }
        if (summary.missing != (int) missing.size()) {
            summary.missing = (int) missing.size();
            Notify();
        }
    }
    void GetMissingSongs() {
        if (syncing || downloading || cleaning || !PlaylistCore::hasLoaded)
            return;
        std::vector<std::string> paths;
        for (auto playlist : PlaylistCore::GetLoadedPlaylists())
            if (playlist && playlist->playlistCS && PlaylistCore::PlaylistHasMissingSongs(playlist) > 0)
                paths.push_back(playlist->path);
        if (paths.empty()) {
            summary.missing = 0;
            SetStatus("Every playlist already has all its songs.");
            return;
        }
        SetStatus("Getting missing songs...");
        StartDownloads(paths, "");
    }

    LooseSongs const& GetLooseSongs() {
        return loose;
    }
    void ResetLooseSongs() {
        if (loose.state == LooseSongs::State::Deleting)
            return;
        looseScan++;
        loose = {};
        looseHashes.clear();
        loosePaths.clear();
        looseInfo.clear();
    }
    void ScanLooseSongs() {
        if (loose.state == LooseSongs::State::Deleting)
            return;
        ResetLooseSongs();
        int scan = looseScan;
        if (!SongCore::API::Loading::AreSongsLoaded() || SongCore::API::Loading::AreSongsRefreshing()) {
            loose.state = LooseSongs::State::Failed;
            loose.message = "Songs are still loading. Try again in a moment.";
            Notify();
            return;
        }
        auto favorites = GetFavorites();
        if (!favorites) {
            loose.state = LooseSongs::State::Failed;
            loose.message = "Couldn't read your favorites yet, so nothing will be deleted. Try again from the song list.";
            Notify();
            return;
        }
        loose.state = LooseSongs::State::Scanning;
        Notify();
        auto used = SongsInPlaylists();
        std::vector<std::string> candidates;
        std::map<std::string, std::string> paths;
        for (auto level : SongCore::API::Loading::GetAllLevels()) {
            if (!level || IsWip(level))
                continue;
            auto hash = HashOfLevelId(std::string(level->levelID));
            if (hash.empty() || used.contains(hash) || paths.contains(hash))
                continue;
            loose.found++;
            if (favorites->Contains(level->levelID)) {
                loose.favorites++;
                continue;
            }
            candidates.push_back(hash);
            paths[hash] = std::string(level->customLevelPath);
        }
        if (candidates.empty()) {
            loose.state = LooseSongs::State::Ready;
            loose.bytes = 0;
            Notify();
            return;
        }
        loose.state = LooseSongs::State::Checking;
        Notify();
        CheckBeatSaver(
            candidates,
            [scan](int n) {
                if (scan != looseScan)
                    return;
                loose.progress = n;
                Notify();
            },
            [scan, candidates, paths](bool ok, BeatSaverMap found) {
                if (scan != looseScan)
                    return;
                if (!ok) {
                    loose.state = LooseSongs::State::Failed;
                    loose.message = "Couldn't reach BeatSaver to check the songs, so nothing will be deleted. Try again in a bit.";
                    Notify();
                    return;
                }
                for (auto const& h : candidates) {
                    if (!found.contains(h)) {
                        loose.notOnBeatSaver++;
                        continue;
                    }
                    looseHashes.push_back(h);
                    loosePaths.push_back(paths.at(h));
                }
                looseInfo = std::move(found);
                loose.toDelete = (int) looseHashes.size();
                loose.progress = 0;
                loose.bytes = looseHashes.empty() ? 0 : -1;
                loose.state = LooseSongs::State::Ready;
                Notify();
                if (!looseHashes.empty())
                    SizeOf(loosePaths, [scan](int64_t bytes) {
                        if (scan != looseScan)
                            return;
                        loose.bytes = bytes;
                        Notify();
                    });
            });
    }
    void DeleteLooseSongs() {
        if (loose.state != LooseSongs::State::Ready || looseHashes.empty() || syncing || downloading || cleaning)
            return;
        // A sync since the scan may have put some of them in a playlist: those stay.
        auto used = SongsInPlaylists();
        std::vector<std::string> hashes;
        auto paths = std::make_shared<std::vector<std::string>>();
        for (size_t i = 0; i < looseHashes.size(); i++)
            if (!used.contains(looseHashes[i])) {
                hashes.push_back(looseHashes[i]);
                paths->push_back(loosePaths[i]);
            }
        if (hashes.empty()) {
            ScanLooseSongs();
            return;
        }
        SaveRemovedList(hashes, looseInfo);
        cleaning = true;
        activity.kind = Activity::Kind::Cleaning;
        activity.playlist.clear();
        activity.done = 0;
        activity.total = (int) hashes.size();
        loose.state = LooseSongs::State::Deleting;
        loose.toDelete = (int) hashes.size();
        loose.progress = 0;
        Notify();
        SetStatus("Cleaning up songs: 0 of " + std::to_string(hashes.size()));
        DeleteSongFolders(
            paths, 0,
            [total = hashes.size()](int n) {
                loose.progress = n;
                activity.done = n;
                SetStatus("Cleaning up songs: " + std::to_string(n) + " of " + std::to_string(total));
            },
            [](int gone) {
                cleaning = false;
                activity = {};
                statsDirty = true;
                loose.state = LooseSongs::State::Done;
                loose.progress = gone;
                SetStatus("Deleted " + std::to_string(gone) + (gone == 1 ? " song that wasn't" : " songs that weren't") + " in any playlist");
                CountMissingSongs();
                // Tell Studio, so "Removed songs" shows under Recently deleted.
                missedSync = true;
                RunQueued();
            });
    }
    bool IsDownloading() {
        return downloading;
    }
    Activity const& GetActivity() {
        return activity;
    }
    bool AutoDownload() {
        return getConfig().AutoDownload.GetValue();
    }
    bool PlaylistAutoDownload(std::string const& file) {
        if (InList(getConfig().AutoOff.GetValue(), file))
            return false;
        if (InList(getConfig().AutoOn.GetValue(), file))
            return true;
        return getConfig().AutoDownload.GetValue();
    }
    void SetPlaylistAutoDownload(std::string const& file, bool on) {
        SetAuto(file, on);
        Notify();
    }
    void SetAllAutoDownload(bool on) {
        getConfig().AutoDownload.SetValue(on);
        getConfig().AutoOff.SetValue({});
        getConfig().AutoOn.SetValue({});
        Notify();
    }
    void StartMenuChecks() {
        if (menuChecks)
            return;
        menuChecks = true;
        MenuCheck();
    }
    std::string const& Code() {
        return code;
    }
    std::string const& Status() {
        static std::string const linked = "Linked to Playlist Studio.", unlinked = "Not linked yet. Press Link to start.";
        if (status.empty())
            return IsLinked() ? linked : unlinked;
        return status;
    }
    Summary const& GetSummary() {
        return summary;
    }
    void SetListener(std::function<void()> l) {
        listener = std::move(l);
    }

    std::vector<HeadsetPlaylist> HeadsetPlaylists() {
        std::vector<HeadsetPlaylist> out;
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(PlaylistsDir(), ec)) {
            auto name = entry.path().filename().string();
            if (!entry.is_regular_file() || name.starts_with(FILE_PREFIX) || !ValidFile(name))
                continue;
            std::error_code sec;
            if (entry.file_size(sec) > MAX_PLAYLIST_BYTES)
                continue;
            rapidjson::Document doc;
            if (!ParseText(readfile(entry.path().string()), doc) || !IsPlaylist(doc))
                continue;
            auto title = GetString(doc, "playlistTitle");
            out.push_back({name, title.empty() ? name : title, (int) doc["songs"].Size(), !Excluded(name), PlaylistAutoDownload(name)});
        }
        std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.title < b.title; });
        return out;
    }

    void SetShared(std::string const& file, bool shared) {
        auto list = getConfig().Excluded.GetValue();
        list.erase(std::remove(list.begin(), list.end(), file), list.end());
        if (!shared)
            list.push_back(file);
        getConfig().Excluded.SetValue(list);
        Notify();
    }

    void SetAllShared(bool shared) {
        std::vector<std::string> list;
        if (!shared)
            for (auto const& p : HeadsetPlaylists())
                list.push_back(p.file);
        getConfig().Excluded.SetValue(list);
        Notify();
    }

    void StartLink() {
        auto token = NewToken();
        pendingToken = token;
        code.clear();
        SetStatus("Getting a code...");
        Post(Server() + "/link/start", token, R"({"name":"Quest"})", [token](Response r) {
            if (token != pendingToken)
                return;
            rapidjson::Document doc;
            if (!r.IsSuccessful() || !ParseJson(r, doc) || GetString(doc, "code").empty()) {
                pendingToken.clear();
                SetStatus(r.httpCode == 429 ? "Too many tries. Wait a few minutes." : "Couldn't reach Playlist Studio (" + r.Describe() + ").");
                return;
            }
            code = GetString(doc, "code");
            linkStarted = std::chrono::steady_clock::now();
            SetStatus("In Playlist Studio, open Linked Quest and type this code.");
            ScheduleLinkPoll(token);
        });
    }

    void CancelLink() {
        if (pendingToken.empty())
            return;
        pendingToken.clear();
        code.clear();
        SetStatus("");
    }

    void Unlink() {
        auto token = getConfig().Token.GetValue();
        if (token.empty())
            return;
        // Playlists already on the headset stay; they just stop updating.
        Post(Server() + "/link/unlink", token, "{}", [](Response) {});
        ForgetLink("Unlinked. Your synced playlists stay but won't update.");
    }

    void SyncNow(int minGapSeconds) {
        auto token = getConfig().Token.GetValue();
        if (token.empty())
            return;
        // A sync waits for one already running, songs still downloading and a song clean up. One you asked
        // for (Sync now, a Sharing change) runs as soon as they finish, and so does an automatic one skipped
        // while songs were downloading or being cleaned up, so changes waiting in Studio still come through.
        if (syncing || downloading || cleaning) {
            if (minGapSeconds == 0) {
                syncAgain = true;
                Notify();
            } else if (downloading || cleaning)
                missedSync = true;
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (minGapSeconds > 0 && lastSync.time_since_epoch().count() != 0 && now - lastSync < std::chrono::seconds(minGapSeconds))
            return;
        if (!PlaylistCore::hasLoaded) {
            // PlaylistCore ignores reloads until its first load, so wait for it.
            BSML::MainThreadScheduler::ScheduleUntil([]() { return PlaylistCore::hasLoaded; }, [minGapSeconds]() { SyncNow(minGapSeconds); });
            return;
        }
        syncing = true;
        lastSync = now;
        SetStatus("Syncing...");
        Get(Server() + "/link/manifest", token, [token](Response r) {
            if (token != getConfig().Token.GetValue()) {
                syncing = false;
                return;
            }
            if (r.httpCode == 401) {
                syncing = false;
                ForgetLink("This headset was unlinked in Playlist Studio. Link again to keep syncing.");
                return;
            }
            rapidjson::Document doc;
            if (!r.IsSuccessful() || !ParseJson(r, doc) || !doc.HasMember("playlists") || !doc["playlists"].IsArray()) {
                syncing = false;
                SetStatus("Couldn't reach Playlist Studio (" + r.Describe() + "). Will try again later.");
                return;
            }
            auto run = std::make_shared<Run>();
            run->token = token;
            // Only accept files served by the same Worker, never an arbitrary address.
            auto prefix = Server() + "/p/";
            for (auto const& v : doc["playlists"].GetArray()) {
                if (!v.IsObject())
                    continue;
                Item it{GetString(v, "id"), GetString(v, "title"), GetString(v, "url")};
                auto upd = v.FindMember("updated");
                if (upd != v.MemberEnd() && upd->value.IsNumber())
                    it.updated = upd->value.IsInt64() ? upd->value.GetInt64() : (int64_t) upd->value.GetDouble();
                if (ValidId(it.id) && it.url == prefix + it.id + ".bplist")
                    run->items.push_back(std::move(it));
            }
            if (doc.HasMember("writes") && doc["writes"].IsArray())
                for (auto const& v : doc["writes"].GetArray())
                    if (v.IsObject()) {
                        Write w{GetString(v, "id"), GetString(v, "file"), GetString(v, "url"), v.HasMember("create") && v["create"].IsBool() && v["create"].GetBool()};
                        if (v.HasMember("restore") && v["restore"].IsInt64())
                            w.restore = v["restore"].GetInt64();
                        w.remove = v.HasMember("remove") && v["remove"].IsBool() && v["remove"].GetBool();
                        w.songs = w.remove && v.HasMember("songs") && v["songs"].IsBool() && v["songs"].GetBool();
                        run->writes.push_back(std::move(w));
                    }
            if (auto order = doc.FindMember("order"); order != doc.MemberEnd() && order->value.IsObject()) {
                auto t = order->value.FindMember("t");
                auto files = order->value.FindMember("files");
                if (t != order->value.MemberEnd() && t->value.IsInt64() && files != order->value.MemberEnd() && files->value.IsArray()) {
                    run->orderT = t->value.GetInt64();
                    for (auto const& f : files->value.GetArray())
                        if (f.IsString() && ValidFile(f.GetString()))
                            run->orderFiles.push_back(f.GetString());
                }
            }
            if (auto clean = doc.FindMember("cleanup"); clean != doc.MemberEnd() && clean->value.IsObject()) {
                auto id = GetString(clean->value, "id");
                if (ValidId(id))
                    run->cleanupId = id;
            }
            if (auto gm = doc.FindMember("getMissing"); gm != doc.MemberEnd() && gm->value.IsObject()) {
                auto id = GetString(gm->value, "id");
                if (ValidId(id)) {
                    run->missingId = id;
                    auto all = gm->value.FindMember("all");
                    run->missingAll = all != gm->value.MemberEnd() && all->value.IsBool() && all->value.GetBool();
                    auto files = gm->value.FindMember("files");
                    if (files != gm->value.MemberEnd() && files->value.IsArray())
                        for (auto const& f : files->value.GetArray())
                            if (f.IsString() && ValidFile(f.GetString()))
                                run->missingFiles.push_back(f.GetString());
                }
            }
            // Auto-download switches changed in Studio take effect now, before this sync decides what to download.
            if (auto adl = doc.FindMember("autodl"); adl != doc.MemberEnd() && adl->value.IsObject()) {
                auto t = adl->value.FindMember("t");
                auto set = adl->value.FindMember("set");
                if (t != adl->value.MemberEnd() && t->value.IsInt64() && t->value.GetInt64() > getConfig().AutoApplied.GetValue() && set != adl->value.MemberEnd() && set->value.IsArray()) {
                    for (auto const& e : set->value.GetArray()) {
                        if (!e.IsObject())
                            continue;
                        auto file = GetString(e, "file");
                        auto on = e.FindMember("on");
                        if (ValidFile(file) && on != e.MemberEnd() && on->value.IsBool())
                            SetAuto(file, on->value.GetBool());
                    }
                    getConfig().AutoApplied.SetValue((int) t->value.GetInt64());
                }
            }
            if (auto chg = doc.FindMember("chg"); chg != doc.MemberEnd() && chg->value.IsInt64())
                knownChg = chg->value.GetInt64();
            ApplyWrite(run, 0);
        });
    }
}
