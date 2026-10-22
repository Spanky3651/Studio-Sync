#include "Settings.hpp"

#include "Sync.hpp"
#include "main.hpp"

#include "bsml/shared/BSML-Lite.hpp"
#include "bsml/shared/BSML.hpp"

#include "HMUI/CurvedTextMeshPro.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/TextAnchor.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"

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

    void Build(UnityEngine::GameObject* tab) {
        // Stack pinned to the top of the tab so it grows downward, like other Mods tabs.
        auto root = BSML::Lite::CreateVerticalLayoutGroup(tab->get_transform());
        root->set_childControlWidth(true);
        root->set_childControlHeight(true);
        root->set_childForceExpandWidth(false);
        root->set_childForceExpandHeight(false);
        root->set_childAlignment(UnityEngine::TextAnchor::UpperCenter);
        root->set_spacing(1.0f);
        auto fitter = root->get_gameObject()->AddComponent<UnityEngine::UI::ContentSizeFitter*>();
        fitter->set_verticalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);
        auto rect = root->GetComponent<UnityEngine::RectTransform*>();
        rect->set_anchorMin({0.5f, 1.0f});
        rect->set_anchorMax({0.5f, 1.0f});
        rect->set_pivot({0.5f, 1.0f});
        rect->set_anchoredPosition({0.0f, -2.0f});
        auto parent = root->get_transform();

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

    void TabActivated(UnityEngine::GameObject* tab, bool firstActivation) {
        if (firstActivation)
            Build(tab);
        if (IsLinked() && !IsLinking())
            SyncNow(30);
        Refresh();
    }
}

namespace StudioSync {
    void RegisterUI() {
        // Same Mods panel beside song select that ReeSaber and other mods use.
        BSML::Register::RegisterGameplaySetupTab("Studio Sync", TabActivated, BSML::MenuType::All);
    }
}
