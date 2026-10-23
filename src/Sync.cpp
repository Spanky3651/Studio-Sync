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
#include <vector>

#include "Config.hpp"
#include "Http.hpp"
#include "main.hpp"

#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "playlistcore/shared/PlaylistCore.hpp"
#include "songcore/shared/SongCore.hpp"

namespace StudioSync {
    namespace {
        constexpr std::string_view FILE_PREFIX = "StudioSync_";
        constexpr size_t MAX_PLAYLIST_BYTES = 2500000;
        constexpr int LINK_POLL_SECONDS = 5;
        constexpr int LINK_GIVE_UP_SECONDS = 11 * 60;
        constexpr size_t BACKUPS_PER_PLAYLIST = 3;
        constexpr int64_t KEEP_DELETED_SECONDS = 30 * 86400;  // deleted playlists stay restorable this long
        constexpr size_t MAX_DELETED_LISTED = 30;

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
        };
        // One sync from start to finish; each step hands it to the next.
        struct Run {
            std::string token;
            std::vector<Item> items;
            std::vector<Write> writes;
            std::vector<std::string> edited, created, restored, deleted;
            std::vector<std::string> touched;  // playlist paths written this sync, checked for missing songs
            std::vector<std::string> need;
            bool changed = false;
            int written = 0, removed = 0, failed = 0, shared = 0, total = 0;
        };
        using RunPtr = std::shared_ptr<Run>;

        std::string pendingToken, code, status;
        std::chrono::steady_clock::time_point linkStarted, lastSync{};
        bool syncing = false, downloading = false;
        // Sync now (or a Sharing change) asked for while a sync or song download was running.
        bool syncAgain = false;
        Summary summary;
        std::function<void()> listener;

        void Notify() {
            if (listener)
                listener();
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

        // ---- the sync pipeline: edits -> Studio playlists -> inventory -> finish ----

        void ApplyItems(RunPtr run);
        void SendInventory(RunPtr run);

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

        // Run a sync that was asked for mid-sync, once this one (and its song downloads) are done.
        void RunQueued() {
            if (!syncAgain || syncing || downloading)
                return;
            syncAgain = false;
            BSML::MainThreadScheduler::ScheduleAfterTime(0.5f, []() { SyncNow(); });
        }

        void DownloadsDone(JobPtr job) {
            if (job->wanted == 0) {
                downloading = false;
                Notify();
                RunQueued();
                return;
            }
            SetStatus(job->base + ". Loading new songs...");
            auto refresh = SongCore::API::Loading::RefreshSongs(false);
            // Wait for SongCore's rescan, then give PlaylistCore a moment to rebuild the playlists.
            BSML::MainThreadScheduler::ScheduleUntil(
                [refresh]() { return refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready; },
                [job]() {
                    BSML::MainThreadScheduler::ScheduleAfterTime(1.5f, [job]() {
                        int left = std::min(job->wanted, MissingIn(job->paths));
                        summary.downloadedLast = job->wanted - left;
                        summary.notFoundLast = left;
                        std::string text = job->base + ", " + std::to_string(summary.downloadedLast) + " song" + (summary.downloadedLast == 1 ? "" : "s") + " downloaded";
                        if (left)
                            text += ". " + std::to_string(left) + " couldn't be found on BeatSaver";
                        downloading = false;
                        SetStatus(text);
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
            PlaylistCore::DownloadMissingSongsFromPlaylist(
                playlist,
                [job, playlist]() {
                    PlaylistCore::MarkPlaylistForReload(playlist);
                    job->i++;
                    DownloadNext(job);
                },
                [job, name](int total, int done) {
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
            if (run->changed)
                PlaylistCore::ReloadPlaylists(false);
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
            if (run->written || run->removed || !run->edited.empty() || !run->created.empty() || !run->restored.empty() || !run->deleted.empty()) {
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
                    text += ", " + std::to_string(run->deleted.size()) + " deleted";
                if (run->removed)
                    text += ", " + std::to_string(run->removed) + " removed";
            }
            if (run->failed)
                text += ". " + std::to_string(run->failed) + " failed, will retry.";
            summary.downloadedLast = summary.notFoundLast = 0;
            SetStatus(text);
            if (getConfig().AutoDownload.GetValue() && !run->touched.empty())
                StartDownloads(run->touched, text);
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

        void Download(RunPtr run, std::shared_ptr<std::vector<Item>> queue, size_t i) {
            if (i >= queue->size()) {
                SendInventory(run);
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

        // Tell Studio which of the headset's own playlists exist; it asks for the ones it lacks.
        void SendInventory(RunPtr run) {
            rapidjson::Document body(rapidjson::kObjectType);
            auto& a = body.GetAllocator();
            rapidjson::Value list(rapidjson::kArrayType);
            std::error_code ec;
            run->shared = 0;
            run->total = 0;
            auto backups = AllBackups();
            std::set<std::string> present;
            for (auto const& entry : std::filesystem::directory_iterator(PlaylistsDir(), ec)) {
                auto name = entry.path().filename().string();
                if (!entry.is_regular_file() || name.starts_with(FILE_PREFIX) || !ValidFile(name))
                    continue;
                present.insert(name);
                run->total++;
                if (Excluded(name))
                    continue;
                std::error_code sec;
                if (entry.file_size(sec) > MAX_PLAYLIST_BYTES)
                    continue;
                auto text = readfile(entry.path().string());
                rapidjson::Document doc;
                if (!ParseText(text, doc) || !IsPlaylist(doc))
                    continue;
                rapidjson::Value it(rapidjson::kObjectType);
                it.AddMember("file", rapidjson::Value(name.c_str(), a), a);
                it.AddMember("title", rapidjson::Value(GetString(doc, "playlistTitle").c_str(), a), a);
                it.AddMember("count", (int) doc["songs"].Size(), a);
                it.AddMember("maps", MapCount(doc), a);
                it.AddMember("sha", rapidjson::Value(Fingerprint(text).c_str(), a), a);
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
                list.PushBack(it, a);
                run->shared++;
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
            body.AddMember("features", features, a);
            Post(Server() + "/link/inventory", run->token, ToJson(body), [run](Response r) {
                rapidjson::Document doc;
                if (r.IsSuccessful() && ParseJson(r, doc) && doc.HasMember("need") && doc["need"].IsArray())
                    for (auto const& v : doc["need"].GetArray())
                        if (v.IsString())
                            run->need.push_back(v.GetString());
                UploadNeeded(run, 0);
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
        return syncing || downloading;
    }
    bool IsDownloading() {
        return downloading;
    }
    bool AutoDownload() {
        return getConfig().AutoDownload.GetValue();
    }
    void SetAutoDownload(bool on) {
        getConfig().AutoDownload.SetValue(on);
        Notify();
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
            out.push_back({name, title.empty() ? name : title, (int) doc["songs"].Size(), !Excluded(name)});
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
        // A sync waits for songs still downloading from the last one. One you asked for (Sync now, a
        // Sharing change) runs as soon as they finish; the automatic ones just skip.
        if (syncing || downloading) {
            if (minGapSeconds == 0)
                syncAgain = true;
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
                        run->writes.push_back(std::move(w));
                    }
            ApplyWrite(run, 0);
        });
    }
}
