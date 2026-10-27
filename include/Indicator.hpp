#pragma once

#include <string>

namespace StudioSync::Indicator {
    // A small progress line floating in the menu while songs download or get cleaned up, so it shows on every
    // menu screen without opening the Studio Sync tab. Hidden while a song is playing.
    void Refresh();

    // Where it sits: a few spots to try, picked in the tab.
    int SpotCount();
    std::string SpotName(int spot);
    void SetSpot(int spot);

    // Shows sample text until turned off, to check where it sits on each menu screen.
    bool Previewing();
    void SetPreview(bool on);
}
