#pragma once

#include <functional>
#include <string>

namespace StudioSync {
    // All of these run on the main thread.
    bool IsLinked();
    bool IsLinking();
    std::string const& Code();
    std::string const& Status();

    // Called whenever Status() or Code() changes, so the settings screen can redraw.
    void SetListener(std::function<void()> listener);

    void StartLink();
    void CancelLink();
    void Unlink();

    // Pull the manifest and bring the headset copies in line with it.
    // Skipped when a sync ran less than `minGapSeconds` ago.
    void SyncNow(int minGapSeconds = 0);
}
