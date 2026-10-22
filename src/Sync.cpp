#include "Sync.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <vector>

#include "Config.hpp"
#include "main.hpp"

#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "playlistcore/shared/PlaylistCore.hpp"
#include "web-utils/shared/WebUtils.hpp"

namespace StudioSync {
    namespace {
        constexpr std::string_view FILE_PREFIX = "StudioSync_";
        constexpr size_t MAX_PLAYLIST_BYTES = 2500000;
        constexpr int LINK_POLL_SECONDS = 5;
        constexpr int LINK_GIVE_UP_SECONDS = 11 * 60;

        using SyncedMap = std::map<std::string, int64_t>;

        SyncedMap LoadSynced() {
            SyncedMap out;
            for (auto const& entry : getConfig().Synced.GetValue()) {
                auto eq = entry.find('=');
                if (eq == std::string::npos)
                    continue;
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

        struct Item {
            std::string id, title, url;
            int64_t updated = 0;
        };

        std::string pendingToken, code, status;
        std::chrono::steady_clock::time_point linkStarted, lastSync{};
        bool syncing = false;
        std::function<void()> listener;

        void Notify() {
            if (listener)
                listener();
        }
        void SetStatus(std::string text) {
            status = std::move(text);
            Notify();
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

        WebUtils::URLOptions Options(std::string const& url, std::string const& token, WebUtils::URLOptions::QueryMap query = {}) {
            WebUtils::URLOptions::HeaderMap headers = {{"Content-Type", "application/json"}};
            if (!token.empty())
                headers["Authorization"] = "Bearer " + token;
            // useSSL = true turns on certificate checks
            return WebUtils::URLOptions(url, std::move(query), std::move(headers), true);
        }

        void Post(std::string const& url, std::string const& token, std::string body, std::function<void(WebUtils::StringResponse)> done) {
            // the body must outlive the request, so the callback keeps it alive
            auto data = std::make_shared<std::string>(std::move(body));
            std::span<uint8_t const> span((uint8_t const*) data->data(), data->size());
            WebUtils::PostAsync<WebUtils::StringResponse>(Options(url, token), span, [data, done](WebUtils::StringResponse r) {
                BSML::MainThreadScheduler::Schedule([r = std::move(r), done]() mutable { done(std::move(r)); });
            });
        }

        void Get(WebUtils::URLOptions options, std::function<void(WebUtils::StringResponse)> done) {
            WebUtils::GetAsync<WebUtils::StringResponse>(std::move(options), [done](WebUtils::StringResponse r) {
                BSML::MainThreadScheduler::Schedule([r = std::move(r), done]() mutable { done(std::move(r)); });
            });
        }

        bool ParseJson(WebUtils::StringResponse const& r, rapidjson::Document& doc) {
            if (!r.responseData)
                return false;
            doc.Parse(r.responseData->c_str());
            return !doc.HasParseError() && doc.IsObject();
        }

        std::string GetString(rapidjson::Value const& v, char const* key) {
            auto it = v.FindMember(key);
            return it != v.MemberEnd() && it->value.IsString() ? it->value.GetString() : "";
        }

        bool ValidId(std::string const& id) {
            if (id.size() < 6 || id.size() > 16)
                return false;
            for (char c : id)
                if (!std::isalnum((unsigned char) c))
                    return false;
            return true;
        }

        std::string PlaylistsDir() {
            return getDataDir(std::string_view("PlaylistManager")) + "Playlists";
        }
        std::string PathFor(std::string const& id) {
            return (std::filesystem::path(PlaylistsDir()) / (std::string(FILE_PREFIX) + id + ".bplist")).string();
        }

        // Every playlist file this mod wrote, keyed by playlist id.
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
            SetStatus(why);
        }

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
            Get(Options(Server() + "/link/status", token, {{"code", code}}), [token](WebUtils::StringResponse r) {
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
                    SetStatus("Linked. Pulling your playlists...");
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

        void Finish(bool changed, int written, int removed, int failed, size_t total) {
            syncing = false;
            if (changed)
                PlaylistCore::ReloadPlaylists(false);
            std::string text = "Synced " + std::to_string(total) + (total == 1 ? " playlist" : " playlists");
            if (written || removed)
                text += " (" + std::to_string(written) + " updated, " + std::to_string(removed) + " removed)";
            if (failed)
                text += ". " + std::to_string(failed) + " failed, will retry.";
            SetStatus(text);
        }

        void Download(std::shared_ptr<std::vector<Item>> queue, size_t index, bool changed, int written, int removed, int failed, size_t total) {
            if (index >= queue->size()) {
                Finish(changed, written, removed, failed, total);
                return;
            }
            auto item = (*queue)[index];
            Get(Options(item.url, ""), [=](WebUtils::StringResponse r) {
                bool ok = false;
                if (r.IsSuccessful() && r.responseData && r.responseData->size() <= MAX_PLAYLIST_BYTES) {
                    rapidjson::Document doc;
                    if (ParseJson(r, doc) && doc.HasMember("songs") && doc["songs"].IsArray()) {
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
                }
                if (!ok)
                    logger.error("Could not download playlist {} (http {})", item.id, r.httpCode);
                Download(queue, index + 1, changed || ok, written + (ok ? 1 : 0), removed, failed + (ok ? 0 : 1), total);
            });
        }

        void Apply(std::vector<Item> items) {
            auto files = OurFiles();
            auto synced = LoadSynced();
            std::set<std::string> keep;
            for (auto& it : items)
                keep.insert(it.id);

            // Remove only files this mod wrote that Studio no longer lists.
            int removed = 0;
            for (auto& [id, path] : files) {
                if (!keep.contains(id)) {
                    RemoveFile(path);
                    synced.erase(id);
                    removed++;
                }
            }
            SaveSynced(synced);

            auto queue = std::make_shared<std::vector<Item>>();
            for (auto& it : items) {
                auto have = synced.find(it.id);
                if (!files.contains(it.id) || have == synced.end() || have->second != it.updated)
                    queue->push_back(it);
            }
            Download(queue, 0, removed > 0, 0, removed, 0, items.size());
        }
    }

    bool IsLinked() {
        return !getConfig().Token.GetValue().empty();
    }
    bool IsLinking() {
        return !pendingToken.empty();
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
    void SetListener(std::function<void()> l) {
        listener = std::move(l);
    }

    void StartLink() {
        auto token = NewToken();
        pendingToken = token;
        code.clear();
        SetStatus("Getting a code...");
        Post(Server() + "/link/start", token, R"({"name":"Quest"})", [token](WebUtils::StringResponse r) {
            if (token != pendingToken)
                return;
            rapidjson::Document doc;
            if (!r.IsSuccessful() || !ParseJson(r, doc) || GetString(doc, "code").empty()) {
                pendingToken.clear();
                SetStatus(r.httpCode == 429 ? "Too many tries. Wait a few minutes." : "Couldn't reach Playlist Studio. Check your connection.");
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
        Post(Server() + "/link/unlink", token, "{}", [](WebUtils::StringResponse) {});
        ForgetLink("Unlinked. Your synced playlists stay but won't update.");
    }

    void SyncNow(int minGapSeconds) {
        auto token = getConfig().Token.GetValue();
        if (token.empty() || syncing)
            return;
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
        Get(Options(Server() + "/link/manifest", token), [token](WebUtils::StringResponse r) {
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
                SetStatus("Couldn't reach Playlist Studio. Will try again later.");
                return;
            }
            // Only accept files served by the same Worker, never an arbitrary address.
            auto prefix = Server() + "/p/";
            std::vector<Item> items;
            for (auto const& v : doc["playlists"].GetArray()) {
                if (!v.IsObject())
                    continue;
                Item it{GetString(v, "id"), GetString(v, "title"), GetString(v, "url")};
                auto upd = v.FindMember("updated");
                if (upd != v.MemberEnd() && upd->value.IsNumber())
                    it.updated = upd->value.IsInt64() ? upd->value.GetInt64() : (int64_t) upd->value.GetDouble();
                if (ValidId(it.id) && it.url == prefix + it.id + ".bplist")
                    items.push_back(std::move(it));
            }
            Apply(std::move(items));
        });
    }
}
