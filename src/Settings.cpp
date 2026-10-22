#include "Settings.hpp"

#include <chrono>
#include <vector>

#include "Sync.hpp"
#include "main.hpp"

#include "bsml/shared/BSML-Lite.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"

#include "HMUI/CurvedTextMeshPro.hpp"
#include "TMPro/TextAlignmentOptions.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/TextAnchor.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "UnityEngine/UI/HorizontalLayoutGroup.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"

using namespace StudioSync;

namespace {
    constexpr size_t MAX_LISTED = 6;

    UnityW<HMUI::CurvedTextMeshPro> statusText, codeText, detailText;
    UnityW<UnityEngine::UI::Button> linkButton, cancelButton, syncButton, unlinkButton;
    UnityW<UnityEngine::GameObject> linkedRow;
    UnityW<BSML::ModalView> shareModal;
    UnityW<UnityEngine::Transform> shareList;
    std::vector<UnityW<UnityEngine::GameObject>> shareRows;
    UnityW<HMUI::CurvedTextMeshPro> shareCount;
    bool shareChanged = false;
    std::chrono::steady_clock::time_point unlinkArmed{};

    void Show(UnityW<UnityEngine::UI::Button> button, bool on) {
        if (button)
            button->get_gameObject()->SetActive(on);
    }

    std::string Details() {
        auto const& s = GetSummary();
        std::string out;
        // Older links to playlists hosted by Studio (before it wrote straight to the headset).
        if (!s.fromStudio.empty()) {
            out += "<b>Linked from Studio (" + std::to_string(s.fromStudio.size()) + ")</b>\n";
            for (size_t i = 0; i < s.fromStudio.size() && i < MAX_LISTED; i++)
                out += "  " + s.fromStudio[i] + "\n";
            if (s.fromStudio.size() > MAX_LISTED)
                out += "  <color=#9aa4b2>and " + std::to_string(s.fromStudio.size() - MAX_LISTED) + " more</color>\n";
        }
        if (!s.addedLast.empty()) {
            out += "<b>Added from Studio</b>\n";
            for (size_t i = 0; i < s.addedLast.size() && i < MAX_LISTED; i++)
                out += "  " + s.addedLast[i] + "\n";
        }
        if (!s.editedLast.empty()) {
            out += "<b>Edited in Studio</b>\n";
            for (size_t i = 0; i < s.editedLast.size() && i < MAX_LISTED; i++)
                out += "  " + s.editedLast[i] + "\n";
        }
        if (!s.deletedLast.empty()) {
            out += "<b>Deleted in Studio</b> <color=#9aa4b2>(restorable for 30 days)</color>\n";
            for (size_t i = 0; i < s.deletedLast.size() && i < MAX_LISTED; i++)
                out += "  " + s.deletedLast[i] + "\n";
        }
        if (!s.restoredLast.empty()) {
            out += "<b>Restored from a backup</b>\n";
            for (size_t i = 0; i < s.restoredLast.size() && i < MAX_LISTED; i++)
                out += "  " + s.restoredLast[i] + "\n";
        }
        out += "<color=#9aa4b2>Sharing " + std::to_string(s.shared) + " of your " + std::to_string(s.total) + " playlists with Studio";
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

    // ---- choose which headset playlists Studio can see ----
    std::string Shorten(std::string const& t, size_t n) {
        return t.size() <= n ? t : t.substr(0, n - 1) + "...";
    }

    void UpdateShareCount() {
        if (!shareCount)
            return;
        int on = 0, all = 0;
        for (auto const& p : HeadsetPlaylists()) {
            all++;
            on += p.shared ? 1 : 0;
        }
        shareCount->set_text("Sharing " + std::to_string(on) + " of " + std::to_string(all) + ". Studio can only see and edit the ones switched on.");
    }

    // Give a row a fixed height so the scroll content stacks it instead of overlapping.
    template <class T> T* Sized(T* c, float height) {
        auto le = c->get_gameObject()->template GetComponent<UnityEngine::UI::LayoutElement*>();
        if (!le)
            le = c->get_gameObject()->template AddComponent<UnityEngine::UI::LayoutElement*>();
        le->set_preferredHeight(height);
        le->set_minHeight(height);
        return c;
    }

    void FillShareList() {
        if (!shareList)
            return;
        for (auto& row : shareRows)
            if (row)
                UnityEngine::Object::Destroy(row.ptr());
        shareRows.clear();
        auto list = HeadsetPlaylists();
        if (list.empty())
            shareRows.push_back(Sized(BSML::Lite::CreateText(shareList.ptr(), "No playlists of your own on this headset yet.", TMPro::FontStyles::Italic, 3.5), 6.0f)->get_gameObject());
        for (auto const& p : list) {
            auto file = p.file;
            auto toggle = BSML::Lite::CreateToggle(shareList.ptr(), Shorten(p.title, 30) + " <color=#9aa4b2>(" + std::to_string(p.count) + ")</color>", p.shared, [file](bool on) {
                SetShared(file, on);
                shareChanged = true;
                UpdateShareCount();
            });
            shareRows.push_back(Sized(toggle, 7.0f)->get_gameObject());
        }
        UpdateShareCount();
    }

    // Runs when the modal closes, from Done or from tapping outside it.
    void ShareClosed() {
        // Push the new choice right away so Studio drops or picks up those playlists.
        if (shareChanged && IsLinked()) {
            shareChanged = false;
            SyncNow();
        }
    }

    void BuildShareModal(UnityEngine::Transform* parent) {
        auto modal = BSML::Lite::CreateModal(parent, {100, 78}, [] { ShareClosed(); }, true);
        shareModal = modal;
        auto content = BSML::Lite::CreateScrollableModalContainer(modal);
        auto layout = content->GetComponent<UnityEngine::UI::VerticalLayoutGroup*>();
        layout->set_childControlHeight(true);
        layout->set_childForceExpandHeight(false);
        layout->set_childAlignment(UnityEngine::TextAnchor::UpperCenter);
        layout->set_spacing(1.0f);
        auto parentT = content->get_transform();

        Sized(BSML::Lite::CreateText(parentT, "Share with Playlist Studio", TMPro::FontStyles::Bold, 5), 7.0f)->set_alignment(TMPro::TextAlignmentOptions::Center);
        auto count = Sized(BSML::Lite::CreateText(parentT, "", TMPro::FontStyles::Normal, 3.2), 9.0f);
        count->set_alignment(TMPro::TextAlignmentOptions::Center);
        count->set_enableWordWrapping(true);
        shareCount = count;

        auto row = Sized(BSML::Lite::CreateHorizontalLayoutGroup(parentT), 9.0f);
        row->set_spacing(2.0f);
        row->set_childForceExpandWidth(false);
        row->set_childControlWidth(true);
        row->set_childAlignment(UnityEngine::TextAnchor::MiddleCenter);
        BSML::Lite::CreateUIButton(row->get_transform(), "Share all", [] { SetAllShared(true); shareChanged = true; FillShareList(); });
        BSML::Lite::CreateUIButton(row->get_transform(), "Share none", [] { SetAllShared(false); shareChanged = true; FillShareList(); });
        BSML::Lite::CreateUIButton(row->get_transform(), "Done", [] {
            if (shareModal)
                shareModal->Hide();
            ShareClosed();
        });
        // Toggles are added straight into the scroll content after the header.
        shareList = parentT;
    }

    void OpenShare() {
        if (!shareModal)
            return;
        shareChanged = false;
        FillShareList();
        shareModal->Show();
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
        auto share = BSML::Lite::CreateUIButton(row->get_transform(), "Sharing...", [] { OpenShare(); });
        BSML::Lite::AddHoverHint(share, "Choose which of your playlists Playlist Studio can see and edit");
        auto unlink = BSML::Lite::CreateUIButton(row->get_transform(), "Unlink", [] { PressUnlink(); });
        BSML::Lite::AddHoverHint(unlink, "Stops syncing. Playlists already here stay.");
        unlinkButton = unlink;

        BuildShareModal(tab->get_transform());
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
