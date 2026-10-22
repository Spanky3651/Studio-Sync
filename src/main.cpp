#include "main.hpp"

#include "Config.hpp"
#include "Settings.hpp"
#include "Sync.hpp"

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "bsml/shared/BSML.hpp"
#include "custom-types/shared/register.hpp"

#include "GlobalNamespace/MainMenuViewController.hpp"

modloader::ModInfo modInfo = {MOD_ID, VERSION, 0};

// The main menu comes back after every song, so this is where new and changed
// playlists get pulled. Throttled so a quick run of songs doesn't spam the server.
MAKE_HOOK_MATCH(
    MainMenuViewController_DidActivate,
    &GlobalNamespace::MainMenuViewController::DidActivate,
    void,
    GlobalNamespace::MainMenuViewController* self,
    bool firstActivation,
    bool addedToHierarchy,
    bool screenSystemEnabling
) {
    MainMenuViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
    StudioSync::SyncNow(firstActivation ? 0 : 120);
}

extern "C" __attribute__((visibility("default"))) void setup(CModInfo* info) {
    *info = modInfo.to_c();
    Paper::Logger::RegisterFileContextId(MOD_ID);
    getConfig().Init(modInfo);
    logger.info("Studio Sync {} set up, linked: {}", VERSION, StudioSync::IsLinked());
}

extern "C" __attribute__((visibility("default"))) void late_load() {
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();
    BSML::Init();
    BSML::Register::RegisterSettingsMenu<StudioSync::SettingsViewController*>("Studio Sync");
    INSTALL_HOOK(logger, MainMenuViewController_DidActivate);
}
