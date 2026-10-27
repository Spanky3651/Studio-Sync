#pragma once

#include <string>
#include <vector>

#include "config-utils/shared/config-utils.hpp"

DECLARE_CONFIG(Config) {
    CONFIG_VALUE(Server, std::string, "server", "https://playlist-replay.msploit.workers.dev");
    CONFIG_VALUE(Token, std::string, "token", "");
    // "id=updated" for every playlist written to the headset
    CONFIG_VALUE(Synced, std::vector<std::string>, "synced", std::vector<std::string>{});
    // headset playlist files the player chose not to share with Studio
    CONFIG_VALUE(Excluded, std::vector<std::string>, "excluded", std::vector<std::string>{});
    // download songs the player doesn't have for playlists Studio sends, edits or restores
    CONFIG_VALUE(AutoDownload, bool, "autoDownload", true);
    // the playlist order from Studio's "My order" last put in game (the time Studio saved it)
    CONFIG_VALUE(OrderApplied, int, "orderApplied", 0);
    // the last song clean up Studio asked for that ran here, and what it did (sent again if the answer got lost)
    CONFIG_VALUE(CleanupDone, std::string, "cleanupDone", "");
    CONFIG_VALUE(CleanupResult, std::string, "cleanupResult", "");
};
