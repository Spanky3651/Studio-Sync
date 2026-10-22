#include "Settings.hpp"

#include <chrono>

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
#include "UnityEngine/UI/HorizontalLayoutGroup.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"

using namespace StudioSync;

namespace {
    constexpr size_t MAX_LISTED = 6;

    UnityW<HMUI::CurvedTextMeshPro> statusText, codeText, detailText;
    UnityW<UnityEngine::UI::Button> linkButton, cancelButton, syncButton, unlinkButton;
    UnityW<UnityEngine::GameObject> linkedRow;
    std::chrono::steady_clock::time_point unlinkArmed{};

    void Show(UnityW<UnityEngine::UI::Button> button, bool on) {
        if (button)
            button->get_gameObject()->SetActive(on);
    }

    std::string Details() {
        auto const& s = GetSummary();
        std::string out;
        if (s.fromStudio.empty()) {
            out += "<color=#9aa4b2>No Studio playlists yet. In Playlist Studio, tick \"Sync to my linked Quest\" on a hosted playlist.</color>\n";
        } else {
            out += "<b>From Studio (" + std::to_string(s.fromStudio.size()) + ")</b>\n";
            for (size_t i = 0; i < s.fromStudio.size() && i < MAX_LISTED; i++)
                out += "  " + s.fromStudio[i] + "\n";
            if (s.fromStudio.size() > MAX_LISTED)
                out += "  <color=#9aa4b2>and " + std::to_string(s.fromStudio.size() - MAX_LISTED) + " more</color>\n";
        }
        if (!s.editedLast.empty()) {
            out += "<b>Edited in Studio</b>\n";
            for (size_t i = 0; i < s.editedLast.size() && i < MAX_LISTED; i++)
                out += "  " + s.editedLast[i] + "\n";
        }
        out += "<color=#9aa4b2>" + std::to_string(s.shared) + " of your playlists can be edited in Studio";
        if (!s.lastSync.empty())
            out += " · last sync " + s.lastSync;
        out += "</color>";
        return out;
    }

    void Refresh() {
        if (!statusText)
            return;
        bool linking = IsLinking(), linked = IsLinked();
        statusText->set_text(Status());
        codeText->set_text(Code());
        codeText->get_gameObject()->SetActive(!Code().empty());
        detailText->get_gameObject()->SetActive(linked && !linking);
        if (linked && !linking)
            detailText->set_text(Details());
        // Link only makes sense before pairing; once paired the tab is about syncing.
        Show(linkButton, !linked && !linking);
        Show(cancelButton, linking);
        if (linkedRow)
            linkedRow->SetActive(linked && !linking);
        if (syncButton)
            syncButton->set_interactable(!IsSyncing());
    }

    void PressUnlink() {
        // Two taps, so a stray click doesn't drop the link.
        auto now = std::chrono::steady_clock::now();
        if (now - unlinkArmed > std::chrono::seconds(5)) {
            unlinkArmed = now;
            if (statusText)
                statusText->set_text("Press Unlink again to stop syncing this headset.");
            return;
        }
        unlinkArmed = {};
        Unlink();
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

        BSML::Lite::CreateText(parent, "Studio Sync", TMPro::FontStyles::Bold, 5);
        statusText = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Normal, 3.6);
        codeText = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Bold, 12);
        detailText = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Normal, 3.2);

        auto link = BSML::Lite::CreateUIButton(parent, "Link", [] { StartLink(); });
        BSML::Lite::AddHoverHint(link, "Shows a code to type into Playlist Studio's Linked Quest panel");
        linkButton = link;
        cancelButton = BSML::Lite::CreateUIButton(parent, "Cancel", [] { CancelLink(); });

        auto row = BSML::Lite::CreateHorizontalLayoutGroup(parent);
        row->set_spacing(2.0f);
        row->set_childForceExpandWidth(false);
        row->set_childAlignment(UnityEngine::TextAnchor::MiddleCenter);
        linkedRow = row->get_gameObject();
        auto sync = BSML::Lite::CreateUIButton(row->get_transform(), "Sync now", [] { SyncNow(); });
        BSML::Lite::AddHoverHint(sync, "Pull playlists and edits from Studio and share your playlists with it");
        syncButton = sync;
        auto unlink = BSML::Lite::CreateUIButton(row->get_transform(), "Unlink", [] { PressUnlink(); });
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
