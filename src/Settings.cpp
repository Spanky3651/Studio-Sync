#include "Settings.hpp"

#include "Sync.hpp"
#include "main.hpp"

#include "bsml/shared/BSML-Lite.hpp"

#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/Touchable.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/UI/Button.hpp"

DEFINE_TYPE(StudioSync, SettingsViewController);

using namespace StudioSync;

namespace {
    UnityW<HMUI::CurvedTextMeshPro> statusText, codeText;
    UnityW<UnityEngine::UI::Button> linkButton, cancelButton, syncButton, unlinkButton;

    void Show(UnityW<UnityEngine::UI::Button> button, bool on) {
        if (button)
            button->get_gameObject()->SetActive(on);
    }

    void Refresh() {
        if (!statusText)
            return;
        bool linking = IsLinking(), linked = IsLinked();
        statusText->set_text(Status());
        codeText->set_text(Code());
        codeText->get_gameObject()->SetActive(!Code().empty());
        Show(linkButton, !linking);
        Show(cancelButton, linking);
        Show(syncButton, linked && !linking);
        Show(unlinkButton, linked && !linking);
    }
}

void SettingsViewController::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    if (firstActivation) {
        get_gameObject()->AddComponent<HMUI::Touchable*>();
        auto container = BSML::Lite::CreateScrollableSettingsContainer(this);
        auto parent = container->get_transform();

        BSML::Lite::CreateText(parent, "Playlists you tick in Playlist Studio show up here on their own.", TMPro::FontStyles::Normal, 3.5);
        statusText = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Normal, 4);
        codeText = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Bold, 12);

        auto link = BSML::Lite::CreateUIButton(parent, "Link", [] { StartLink(); });
        BSML::Lite::AddHoverHint(link, "Shows a code to type into Playlist Studio's Linked Quest panel");
        linkButton = link;
        cancelButton = BSML::Lite::CreateUIButton(parent, "Cancel", [] { CancelLink(); });
        syncButton = BSML::Lite::CreateUIButton(parent, "Sync now", [] { SyncNow(); });
        auto unlink = BSML::Lite::CreateUIButton(parent, "Unlink", [] { Unlink(); });
        BSML::Lite::AddHoverHint(unlink, "Stops syncing. Playlists already here stay.");
        unlinkButton = unlink;

        SetListener(Refresh);
    }
    if (IsLinked() && !IsLinking())
        SyncNow(30);
    Refresh();
}
