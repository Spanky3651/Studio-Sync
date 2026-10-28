#pragma once

#include <functional>
#include <string>
#include <vector>

namespace StudioSync {
    // What the Studio Sync tab shows under the status line.
    struct Summary {
        std::vector<std::string> fromStudio;  // titles of playlists Studio sends to this headset
        std::vector<std::string> editedLast;  // headset playlists replaced by Studio edits in the last sync
        std::vector<std::string> addedLast;   // new playlists Studio created in the last sync
        std::vector<std::string> restoredLast;  // playlists put back from a backup in the last sync
        std::vector<std::string> deletedLast;   // playlists deleted from Studio in the last sync (kept as backups)
        int shared = 0;                       // headset playlists visible in Studio
        int total = 0;                        // all of the player's own playlists
        std::string lastSync;                 // local time of the last good sync, e.g. "8:31 AM"
        int downloadedLast = 0;               // songs downloaded after the last sync
        int notFoundLast = 0;                 // songs that couldn't be downloaded (gone from BeatSaver)
        int songsDeletedLast = 0;             // songs deleted along with playlists deleted in Studio
        int missing = -1;                     // songs the headset's playlists list but it doesn't have (-1 = not counted yet)
    };

    // Clean up songs: songs on the headset that aren't in any playlist.
    struct LooseSongs {
        enum class State { Idle, Scanning, Checking, Ready, Deleting, Done, Failed };
        State state = State::Idle;
        int found = 0;           // songs in no playlist (WIP songs never count)
        int favorites = 0;       // of those, in-game favorites, which stay
        int notOnBeatSaver = 0;  // of those, songs BeatSaver doesn't have, which stay (they couldn't come back)
        int toDelete = 0;        // what Delete removes
        int64_t bytes = -1;      // their size on disk, -1 while it's being added up
        int progress = 0;        // songs checked on BeatSaver, or deleted so far
        std::string message;     // why it failed
    };

    // One of the player's own playlists on the headset.
    struct HeadsetPlaylist {
        std::string file, title;
        int count = 0;
        bool shared = true;
        bool autoDownload = true;
    };

    // What's running that takes a while, for the progress line in the menu.
    struct Activity {
        enum class Kind { None, Downloading, Cleaning };
        Kind kind = Kind::None;
        std::string playlist;  // the playlist whose songs are downloading
        int done = 0, total = 0;
        int list = 0, lists = 0;  // which playlist of how many (downloads)
    };

    // All of these run on the main thread.
    bool IsLinked();
    bool IsLinking();
    bool IsSyncing();  // also true while songs are downloading
    bool IsDownloading();
    Activity const& GetActivity();

    // Auto-download, per playlist: when Studio sends, edits or restores a playlist, the songs it's missing download
    // if its switch is on. Playlists without their own choice follow AutoDownload().
    bool AutoDownload();
    bool PlaylistAutoDownload(std::string const& file);
    void SetPlaylistAutoDownload(std::string const& file, bool on);
    void SetAllAutoDownload(bool on);
    std::string const& Code();
    std::string const& Status();
    Summary const& GetSummary();

    // Called whenever anything above changes, so the tab can redraw.
    void SetListener(std::function<void()> listener);

    // The player's own playlists (not the ones Studio sent), sorted by title.
    std::vector<HeadsetPlaylist> HeadsetPlaylists();
    // Choose whether a playlist is shared with Studio. Takes effect on the next sync.
    void SetShared(std::string const& file, bool shared);
    void SetAllShared(bool shared);
    // Whether the in-game favorites are shared with Studio (it can see and change them). Takes effect on the next sync.
    bool FavoritesShared();
    void SetFavoritesShared(bool shared);
    // Hides the temporary playlist Get missing uses for favorites' songs from song select. Call once at startup.
    void RegisterPlaylistFilter();

    void StartLink();
    void CancelLink();
    void Unlink();

    // Pull Studio's playlists and edits, push the headset's own playlists.
    // Skipped when a sync ran less than `minGapSeconds` ago. Sync now (0) during a sync or song
    // download is queued and runs right after it; an automatic one skipped during downloads runs then too.
    void SyncNow(int minGapSeconds = 0);
    bool SyncQueued();
    // Once a minute in the menu: ask Playlist Studio (one cheap request) whether anything new is waiting, and sync if so.
    void StartMenuChecks();

    // Songs every playlist on the headset lists but it doesn't have; Get missing downloads them.
    int MissingSongs();
    void CountMissingSongs();
    void GetMissingSongs();

    // Clean up songs: Scan finds songs in no playlist and checks BeatSaver still has them; Delete removes
    // those (favorites and songs BeatSaver lacks stay) after saving them as a "Removed songs" playlist
    // backup, so Restore in Playlist Studio brings them back for 30 days.
    LooseSongs const& GetLooseSongs();
    void ScanLooseSongs();
    void DeleteLooseSongs();
    void ResetLooseSongs();
}
