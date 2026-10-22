#pragma once

#include <functional>
#include <string>
#include <vector>

namespace StudioSync {
    // What the Studio Sync tab shows under the status line.
    struct Summary {
        std::vector<std::string> fromStudio;  // titles of playlists Studio sends to this headset
        std::vector<std::string> editedLast;  // headset playlists replaced by Studio edits in the last sync
        int shared = 0;                       // headset playlists visible in Studio
        std::string lastSync;                 // local time of the last good sync, e.g. "8:31 AM"
    };

    // All of these run on the main thread.
    bool IsLinked();
    bool IsLinking();
    bool IsSyncing();
    std::string const& Code();
    std::string const& Status();
    Summary const& GetSummary();

    // Called whenever anything above changes, so the tab can redraw.
    void SetListener(std::function<void()> listener);

    void StartLink();
    void CancelLink();
    void Unlink();

    // Pull Studio's playlists and edits, push the headset's own playlists.
    // Skipped when a sync ran less than `minGapSeconds` ago.
    void SyncNow(int minGapSeconds = 0);
}
