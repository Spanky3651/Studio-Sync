# Studio Sync

A Beat Saber Quest mod that links your headset to [Playlist Studio](https://spanky3651.github.io/Playlist-Studio/). Hosted playlists you tick in Studio show up in game on their own and stay updated. No file to load, no Sync button.

Built for Beat Saber **1.40.8** (Scotland2). Needs **PlaylistCore** and **BSML**, which mod managers install with it.

## Use it
1. Install `StudioSync.qmod` with your mod manager (MBF, QuestPatcher).
2. In game: **Mod Settings → Studio Sync → Link**. A 6-character code appears.
3. In Playlist Studio, open **Linked Quest** and type the code.
4. Host a playlist in **In-Game Sync** and tick **Sync to my linked Quest**.

The headset checks for changes when the game starts and each time you return to the main menu (at most every 2 minutes), or right away with **Sync now**. New playlists appear, updated ones refresh, and ones you remove in Studio disappear.

## What it touches
- Only files it wrote: `StudioSync_<id>.bplist` in `ModData/com.beatgames.beatsaber/Mods/PlaylistManager/Playlists`. Your other playlists are never read or changed.
- Downloads only from the Playlist Studio Worker it is linked to, and only `.bplist` files that parse as playlists (2.5 MB max).
- Its settings live in `ModData/com.beatgames.beatsaber/Configs/studiosync.json`, including the device token. The Worker stores only a SHA-256 of that token.
- **Unlink** (here or in Studio) stops syncing. Playlists already on the headset stay.

## Build
Pushing to GitHub runs `.github/workflows/build.yml`, which restores dependencies with qpm, builds with the NDK and uploads `StudioSync.qmod` as an artifact.

Locally: `qpm restore`, then `qpm s qmod`.

## Not yet
- Downloading missing songs automatically. Use PlaylistManager's download button for now.
