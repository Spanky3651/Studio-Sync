#include "Settings.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>

#include "Sync.hpp"
#include "main.hpp"

#include "bsml/shared/BSML-Lite.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/Components/Tab.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"

#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/TextSegmentedControlCell.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "TMPro/TextAlignmentOptions.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
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
    // The Mods tab label gets a green dot while a sync or song download is running,
    // so it shows from the tab row without opening Studio Sync.
    constexpr std::string_view TAB_NAME = "Studio Sync";
    constexpr std::string_view TAB_NAME_BUSY = "Studio Sync <color=#3ddc84>\u2022</color>";
    bool tabDotShown = false, tabDotWatching = false;

    // Returns false until BSML has built the tab (the first time the panel opens).
    bool RenameTab(std::string_view from, std::string_view to) {
        bool found = false;
        // BSML rebuilds the labels from each Tab's name, so rename the Tab too, not only the label on screen.
        for (auto tab : UnityEngine::Resources::FindObjectsOfTypeAll<BSML::Tab*>())
            if (tab && tab->tabName && std::string(tab->tabName) == from) {
                tab->tabName = StringW(to);
                found = true;
            }
        for (auto cell : UnityEngine::Resources::FindObjectsOfTypeAll<HMUI::TextSegmentedControlCell*>())
            if (cell && cell->_text && std::string(cell->get_text()) == from) {
                // Tab labels have rich text off, which would print the color tag instead of a green dot.
                cell->_text->set_richText(true);
                cell->set_text(StringW(to));
            }
        return found;
    }

    void UpdateTabDot();
    // A sync that ends without saying so (rare) can't leave the dot stuck: recheck while it shows.
    void WatchTabDot() {
        if (tabDotWatching)
            return;
        tabDotWatching = true;
        BSML::MainThreadScheduler::ScheduleAfterTime(2.0f, []() {
            tabDotWatching = false;
            UpdateTabDot();
        });
    }

    // BSML can rebuild the labels (paging the tab row), which turns rich text back off.
    void KeepDotRendered() {
        for (auto cell : UnityEngine::Resources::FindObjectsOfTypeAll<HMUI::TextSegmentedControlCell*>())
            if (cell && cell->_text && !cell->_text->get_richText() && std::string(cell->get_text()) == TAB_NAME_BUSY)
                cell->_text->set_richText(true);
    }

    void UpdateTabDot() {
        bool busy = IsSyncing();
        if (tabDotShown)
            KeepDotRendered();
        if (busy != tabDotShown && RenameTab(busy ? TAB_NAME : TAB_NAME_BUSY, busy ? TAB_NAME_BUSY : TAB_NAME))
            tabDotShown = busy;
        // Keep checking while busy (the tab may not exist yet) and while the dot shows.
        if (busy || tabDotShown)
            WatchTabDot();
    }

    UnityW<HMUI::CurvedTextMeshPro> statusText, codeText, detailText;
    UnityW<UnityEngine::UI::Button> linkButton, cancelButton, syncButton, unlinkButton;
    UnityW<UnityEngine::GameObject> linkedRow, downloadToggle;
    UnityW<BSML::ModalView> shareModal;
    UnityW<UnityEngine::Transform> shareList;
    std::vector<UnityW<UnityEngine::GameObject>> shareRows;
    UnityW<HMUI::CurvedTextMeshPro> shareCount;
    bool shareChanged = false;
    std::chrono::steady_clock::time_point unlinkArmed{};
    // Second row of the tab (Get missing, Clean up...) and the Clean up songs window.
    UnityW<UnityEngine::GameObject> songsRow;
    UnityW<UnityEngine::UI::Button> missingButton, cleanButton, cleanDelete;
    UnityW<BSML::ModalView> cleanModal;
    UnityW<HMUI::CurvedTextMeshPro> cleanText;
    bool cleanArmed = false;

    std::string Plural(int n, std::string const& one, std::string const& many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    }
    std::string Size(int64_t bytes) {
        char buf[32];
        if (bytes >= 1000000000LL)
            std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1e9);
        else
            std::snprintf(buf, sizeof(buf), "%.0f MB", std::max<double>(1.0, bytes / 1e6));
        return buf;
    }

    void Show(UnityW<UnityEngine::UI::Button> button, bool on) {
        if (button)
            button->get_gameObject()->SetActive(on);
    }

    // At most n characters, ending in "..." when cut.
    std::string Shorten(std::string const& t, size_t n) {
        return t.size() <= n ? t : t.substr(0, n - 3) + "...";
    }

    // What the last sync changed, one short line per kind of change, so a big sync can't grow the tab.
    constexpr size_t LINE_CHARS = 56;  // the "Sharing N of your M playlists..." line fits on one line
    constexpr size_t NAME_CHARS = 24;
    constexpr size_t MAX_CHANGE_LINES = 3;

    struct Change {
        std::string label;
        std::vector<std::string> const* names;
        std::string note;  // gray text after the names
    };

    // "Edited Favorites, MSploit - Session and 3 more": as many names as fit on the line.
    std::string ChangeLine(Change const& c) {
        auto const& names = *c.names;
        size_t fixed = c.label.size() + 1 + (c.note.empty() ? 0 : c.note.size() + 1);
        auto more = [&](size_t k) { return k < names.size() ? " and " + std::to_string(names.size() - k) + " more" : std::string(); };
        size_t k = names.size();
        for (; k > 1; k--) {
            size_t len = fixed + more(k).size();
            for (size_t i = 0; i < k; i++)
                len += (i ? 2 : 0) + std::min(names[i].size(), NAME_CHARS);
            if (len <= LINE_CHARS)
                break;
        }
        // Down to one name: give it whatever room is left.
        size_t room = LINE_CHARS > fixed + more(k).size() ? LINE_CHARS - fixed - more(k).size() : 0;
        std::string out = "<b>" + c.label + "</b> ";
        for (size_t i = 0; i < k; i++)
            out += (i ? ", " : "") + Shorten(names[i], k == 1 ? std::max<size_t>(10, std::min(room, NAME_CHARS + 8)) : NAME_CHARS);
        if (k < names.size())
            out += " <color=#9aa4b2>" + more(k).substr(1) + "</color>";
        if (!c.note.empty())
            out += " <color=#9aa4b2>" + c.note + "</color>";
        return out;
    }

    std::string Details() {
        auto const& s = GetSummary();
        std::string out = "<color=#9aa4b2>Sharing " + std::to_string(s.shared) + " of your " + std::to_string(s.total) + " playlists with Studio";
        if (!s.lastSync.empty())
            out += " · last sync " + s.lastSync;
        if (s.missing > 0)
            out += "\n" + Plural(s.missing, "song", "songs") + " in your playlists " + (s.missing == 1 ? "isn't" : "aren't") + " downloaded";
        out += "</color>";

        std::vector<Change> changes;
        if (!s.addedLast.empty())
            changes.push_back({"Added", &s.addedLast, ""});
        if (!s.editedLast.empty())
            changes.push_back({"Edited", &s.editedLast, ""});
        if (!s.deletedLast.empty())
            changes.push_back({"Deleted", &s.deletedLast, s.songsDeletedLast > 0 ? "(with " + Plural(s.songsDeletedLast, "song", "songs") + ", restorable)" : "(restorable)"});
        if (!s.restoredLast.empty())
            changes.push_back({"Restored", &s.restoredLast, ""});
        // Older links to playlists hosted by Studio (before it wrote straight to the headset).
        if (!s.fromStudio.empty())
            changes.push_back({"Linked from Studio", &s.fromStudio, ""});
        for (size_t i = 0; i < changes.size(); i++) {
            if (i + 1 == MAX_CHANGE_LINES && changes.size() > MAX_CHANGE_LINES) {
                // No room for a line each: count the rest on one line.
                std::string rest;
                for (size_t j = i; j < changes.size(); j++) {
                    std::string label = changes[j].label;
                    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    rest += (rest.empty() ? "" : ", ") + std::to_string(changes[j].names->size()) + " " + label;
                }
                out += "\n<b>Also</b> " + rest;
                break;
            }
            out += "\n" + ChangeLine(changes[i]);
        }
        return out;
    }

    // ---- Clean up songs ----
    void RefreshClean() {
        if (!cleanText)
            return;
        using State = LooseSongs::State;
        auto const& l = GetLooseSongs();
        std::string text, kept;
        if (l.favorites > 0)
            kept = Plural(l.favorites, "favorite", "favorites");
        if (l.notOnBeatSaver > 0)
            kept += (kept.empty() ? "" : " and ") + Plural(l.notOnBeatSaver, "song", "songs") + " BeatSaver doesn't have";
        if (!kept.empty())
            kept = "\n<color=#9aa4b2>" + kept + " stay.</color>";
        bool canDelete = false;
        switch (l.state) {
            case State::Idle:
            case State::Scanning:
                text = "Looking for songs that aren't in any playlist...";
                break;
            case State::Checking:
                text = "Checking them on BeatSaver: " + std::to_string(l.progress) + " of " + std::to_string(l.found - l.favorites);
                break;
            case State::Ready:
                if (l.found == 0)
                    text = "Every song on this headset is in a playlist. Nothing to clean up.";
                else if (l.toDelete == 0)
                    text = Plural(l.found, "song isn't", "songs aren't") + " in any playlist, and none can be deleted." + kept;
                else {
                    text = Plural(l.toDelete, "song", "songs") + (l.bytes > 0 ? " (" + Size(l.bytes) + ")" : "") + " " + (l.toDelete == 1 ? "isn't" : "aren't") +
                           " in any of your playlists.\nThey're saved as \"Removed songs\" under Recently deleted in Playlist Studio. Restore it there within 30 days to download them again." + kept;
                    canDelete = !IsSyncing();
                    if (!canDelete)
                        text += "\n<color=#9aa4b2>Waiting for the sync to finish.</color>";
                    else if (cleanArmed)
                        text += "\n<b>Press Confirm to delete them.</b>";
                }
                break;
            case State::Deleting:
                text = "Deleting songs: " + std::to_string(l.progress) + " of " + std::to_string(l.toDelete);
                break;
            case State::Done:
                text = "Deleted " + Plural(l.progress, "song", "songs") + (l.bytes > 0 ? ", freed " + Size(l.bytes) : "") + ".\nTo get them back, restore \"Removed songs\" in Playlist Studio.";
                break;
            case State::Failed:
                text = l.message;
                break;
        }
        cleanText->set_text(text);
        if (cleanDelete) {
            bool show = l.state == State::Ready && l.toDelete > 0;
            cleanDelete->get_gameObject()->SetActive(show);
            cleanDelete->set_interactable(canDelete);
            BSML::Lite::SetButtonText(cleanDelete.ptr(), cleanArmed && canDelete ? "Confirm" : "Delete");
        }
    }

    void OpenClean() {
        if (!cleanModal)
            return;
        cleanArmed = false;
        ScanLooseSongs();
        RefreshClean();
        cleanModal->Show();
    }

    // Two presses, like Unlink: the first shows what's about to happen.
    void PressCleanDelete() {
        if (!cleanArmed) {
            cleanArmed = true;
            RefreshClean();
            return;
        }
        cleanArmed = false;
        DeleteLooseSongs();
        RefreshClean();
    }

    void CleanClosed() {
        cleanArmed = false;
        ResetLooseSongs();
    }

    void Refresh() {
        if (!statusText)
            return;
        bool linking = IsLinking(), linked = IsLinked();
        std::string status = Status();
        if (SyncQueued())
            status += "\n<color=#9aa4b2>Sync now is queued. It runs as soon as this finishes.</color>";
        statusText->set_text(status);
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
        if (songsRow)
            songsRow->SetActive(linked && !linking);
        if (downloadToggle)
            downloadToggle->SetActive(linked && !linking);
        // Sync now stays pressable: during a sync or song download the press is queued (see the status line).
        if (syncButton)
            syncButton->set_interactable(true);
        if (missingButton)
            missingButton->set_interactable(!IsSyncing() && MissingSongs() > 0);
        RefreshClean();
    }

    // ---- choose which headset playlists Studio can see ----
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

    // Same build as the Sharing window (fixed-height rows in a scroll container), which lays out right in game.
    void BuildCleanModal(UnityEngine::Transform* parent) {
        auto modal = BSML::Lite::CreateModal(parent, {100, 62}, [] { CleanClosed(); }, true);
        cleanModal = modal;
        auto content = BSML::Lite::CreateScrollableModalContainer(modal);
        auto layout = content->GetComponent<UnityEngine::UI::VerticalLayoutGroup*>();
        layout->set_childControlHeight(true);
        layout->set_childForceExpandHeight(false);
        layout->set_childAlignment(UnityEngine::TextAnchor::UpperCenter);
        layout->set_spacing(1.0f);
        auto parentT = content->get_transform();

        Sized(BSML::Lite::CreateText(parentT, "Clean up songs", TMPro::FontStyles::Bold, 5), 7.0f)->set_alignment(TMPro::TextAlignmentOptions::Center);
        auto text = Sized(BSML::Lite::CreateText(parentT, "", TMPro::FontStyles::Normal, 3.2), 30.0f);
        text->set_alignment(TMPro::TextAlignmentOptions::Center);
        text->set_enableWordWrapping(true);
        cleanText = text;

        auto row = Sized(BSML::Lite::CreateHorizontalLayoutGroup(parentT), 9.0f);
        row->set_spacing(2.0f);
        row->set_childForceExpandWidth(false);
        row->set_childControlWidth(true);
        row->set_childAlignment(UnityEngine::TextAnchor::MiddleCenter);
        cleanDelete = BSML::Lite::CreateUIButton(row->get_transform(), "Delete", [] { PressCleanDelete(); });
        BSML::Lite::CreateUIButton(row->get_transform(), "Close", [] {
            if (cleanModal)
                cleanModal->Hide();
            CleanClosed();
        });
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

        // Second row, built exactly like the first so the buttons match it.
        auto songs = BSML::Lite::CreateHorizontalLayoutGroup(parent);
        songs->set_spacing(2.0f);
        songs->set_childForceExpandWidth(false);
        songs->set_childAlignment(UnityEngine::TextAnchor::MiddleCenter);
        songsRow = songs->get_gameObject();
        auto missing = BSML::Lite::CreateUIButton(songs->get_transform(), "Get missing", [] { GetMissingSongs(); });
        BSML::Lite::AddHoverHint(missing, "Download every song your playlists list that this headset doesn't have");
        missingButton = missing;
        auto clean = BSML::Lite::CreateUIButton(songs->get_transform(), "Clean up...", [] { OpenClean(); });
        BSML::Lite::AddHoverHint(clean, "Delete songs that aren't in any playlist. Shows what it would remove first.");
        cleanButton = clean;

        auto dl = Sized(BSML::Lite::CreateToggle(parent, "Download missing songs", AutoDownload(), [](bool on) { SetAutoDownload(on); }), 7.0f);
        BSML::Lite::AddHoverHint(dl, "Downloads songs you don't have for playlists Studio sends, edits or restores");
        downloadToggle = dl->get_gameObject();

        // Below every button, so a long list of changes can never push them off the panel.
        detailText = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Normal, 3.2);

        BuildShareModal(tab->get_transform());
        BuildCleanModal(tab->get_transform());
    }

    void TabActivated(UnityEngine::GameObject* tab, bool firstActivation) {
        if (firstActivation)
            Build(tab);
        if (IsLinked() && !IsLinking())
            SyncNow(30);
        CountMissingSongs();
        Refresh();
    }
}

namespace StudioSync {
    void RegisterUI() {
        // Same Mods panel beside song select that ReeSaber and other mods use.
        BSML::Register::RegisterGameplaySetupTab(std::string(TAB_NAME), TabActivated, BSML::MenuType::All);
        // Set here, not when the tab is first opened, so the dot works before anyone opens it.
        SetListener([]() {
            UpdateTabDot();
            Refresh();
        });
    }
}
