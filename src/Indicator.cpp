#include "Indicator.hpp"

#include <algorithm>
#include <chrono>

#include "Config.hpp"
#include "Sync.hpp"
#include "main.hpp"

#include "bsml/shared/BSML-Lite.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "bsml/shared/Helpers/utilities.hpp"

#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "GlobalNamespace/PauseController.hpp"
#include "HMUI/CurvedCanvasSettings.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "TMPro/TextAlignmentOptions.hpp"
#include "TMPro/TextOverflowModes.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/RenderMode.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"

// Built the way SongCore's "Loading songs" text and PlaylistManager's download bar are: a world-space canvas
// that isn't attached to any menu screen, kept across scene changes, with BSML text on it.
namespace StudioSync::Indicator {
    namespace {
        struct Spot {
            char const* name;
            float x, y, z, rx;
        };
        // Top right is where PlaylistManager shows its own download bar. Floor is nearer than SongCore's
        // "Loading songs" text so the two don't overlap.
        constexpr Spot SPOTS[] = {
            {"Top left", -1.4f, 3.1f, 4.0f, 0.0f},
            {"Top right", 1.4f, 3.1f, 4.0f, 0.0f},
            {"Top center", 0.0f, 3.35f, 4.0f, 0.0f},
            {"Floor", 0.0f, 0.02f, 1.9f, 90.0f},
        };
        constexpr int SPOT_COUNT = sizeof(SPOTS) / sizeof(SPOTS[0]);
        constexpr float BAR_WIDTH = 110.0f;
        constexpr int DONE_SECONDS = 6;

        using Clock = std::chrono::steady_clock;

        UnityW<UnityEngine::GameObject> root;
        UnityW<HMUI::CurvedTextMeshPro> head, line, sub;
        UnityW<UnityEngine::UI::Image> barBack, bar;
        int placedAt = -1;
        bool preview = false, polling = false, shown = false;
        // What finished last, shown for a few seconds after the work is done.
        Activity::Kind lastKind = Activity::Kind::None;
        Clock::time_point doneUntil{};
        std::string doneLine, doneSub;

        // At most n characters, cut on a whole UTF-8 character, ending in "..." when cut.
        std::string Shorten(std::string const& t, size_t n) {
            if (t.size() <= n)
                return t;
            size_t cut = n - 3;
            while (cut > 0 && ((unsigned char) t[cut] & 0xC0) == 0x80)
                cut--;
            return t.substr(0, cut) + "...";
        }
        std::string Plural(int n, std::string const& one, std::string const& many) {
            return std::to_string(n) + " " + (n == 1 ? one : many);
        }

        // In the menu, not in a song: the menu's own objects are switched off while a level plays, and a song
        // (solo or party) has an active pause controller.
        bool InMenu() {
            for (auto p : UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::PauseController*>())
                if (p && p->get_isActiveAndEnabled())
                    return false;
            bool any = false;
            for (auto m : UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::MainFlowCoordinator*>()) {
                if (!m)
                    continue;
                if (m->get_isActiveAndEnabled())
                    return true;
                any = true;
            }
            return !any;
        }

        HMUI::CurvedTextMeshPro* MakeText(UnityEngine::RectTransform* parent, float size, float y, UnityEngine::Color color) {
            auto t = BSML::Lite::CreateText(parent, "", TMPro::FontStyles::Normal, size, {0.0f, y}, {160.0f, size + 2.0f});
            t->set_alignment(TMPro::TextAlignmentOptions::Center);
            t->set_richText(true);
            t->set_enableWordWrapping(false);
            t->set_overflowMode(TMPro::TextOverflowModes::Ellipsis);
            t->set_color(color);
            return t;
        }

        UnityEngine::UI::Image* MakeBar(UnityEngine::RectTransform* parent, float width, UnityEngine::Color color, bool filled) {
            auto img = UnityEngine::GameObject::New_ctor(filled ? "Bar" : "BarBack")->AddComponent<UnityEngine::UI::Image*>();
            auto rect = img->get_rectTransform();
            rect->SetParent(parent, false);
            rect->set_sizeDelta({width, 2.0f});
            rect->set_anchoredPosition({0.0f, -15.0f});
            img->set_sprite(BSML::Utilities::ImageResources::GetWhitePixel());
            img->set_color(color);
            if (filled) {
                img->set_type(UnityEngine::UI::Image::Type::Filled);
                img->set_fillMethod(UnityEngine::UI::Image::FillMethod::Horizontal);
                img->set_fillAmount(0.0f);
            }
            return img;
        }

        void Build() {
            auto go = UnityEngine::GameObject::New_ctor("StudioSyncIndicator");
            UnityEngine::Object::DontDestroyOnLoad(go);
            auto canvas = go->AddComponent<UnityEngine::Canvas*>();
            canvas->set_renderMode(UnityEngine::RenderMode::WorldSpace);
            go->AddComponent<HMUI::CurvedCanvasSettings*>()->SetRadius(0.0f);
            auto rect = go->GetComponent<UnityEngine::RectTransform*>();
            rect->set_sizeDelta({170.0f, 40.0f});
            go->get_transform()->set_localScale({0.01f, 0.01f, 0.01f});

            head = MakeText(rect, 5.5f, 12.0f, {0.8f, 0.85f, 0.9f, 1.0f});
            line = MakeText(rect, 8.5f, 2.5f, {1.0f, 1.0f, 1.0f, 1.0f});
            sub = MakeText(rect, 5.5f, -7.0f, {0.6f, 0.65f, 0.7f, 1.0f});
            barBack = MakeBar(rect, BAR_WIDTH, {1.0f, 1.0f, 1.0f, 0.15f}, false);
            bar = MakeBar(rect, BAR_WIDTH, {0.24f, 0.86f, 0.52f, 0.9f}, true);
            root = go;
            placedAt = -1;
            go->SetActive(false);
            shown = false;
        }

        void Place() {
            int spot = std::clamp(getConfig().IndicatorSpot.GetValue(), 0, SPOT_COUNT - 1);
            if (!root || spot == placedAt)
                return;
            auto const& s = SPOTS[spot];
            auto t = root->get_transform();
            t->set_position({s.x, s.y, s.z});
            t->set_eulerAngles({s.rx, 0.0f, 0.0f});
            placedAt = spot;
        }

        void SetBar(bool on, float fill) {
            if (barBack)
                barBack->get_gameObject()->SetActive(on);
            if (bar) {
                bar->get_gameObject()->SetActive(on);
                bar->set_fillAmount(std::clamp(fill, 0.0f, 1.0f));
            }
        }

        void Fill(Activity const& a, bool done) {
            std::string l, s;
            bool showBar = false;
            float fill = 0.0f;
            if (preview && a.kind == Activity::Kind::None && !done) {
                l = "Downloading songs   12 of 40";
                s = "for MSploit - Session   ·   preview, spot: " + SpotName(getConfig().IndicatorSpot.GetValue());
                showBar = true;
                fill = 0.3f;
            } else if (done) {
                l = doneLine;
                s = doneSub;
            } else if (a.kind == Activity::Kind::Downloading) {
                l = "Downloading songs   " + std::to_string(a.done) + " of " + std::to_string(a.total);
                s = "for " + Shorten(a.playlist, 34);
                if (a.lists > 1)
                    s += "   ·   playlist " + std::to_string(a.list) + " of " + std::to_string(a.lists);
                showBar = a.total > 0;
                fill = a.total > 0 ? (float) a.done / (float) a.total : 0.0f;
            } else if (a.kind == Activity::Kind::Cleaning) {
                l = "Cleaning up songs   " + std::to_string(a.done) + " of " + std::to_string(a.total);
                s = "songs that aren't in any playlist";
                showBar = a.total > 0;
                fill = a.total > 0 ? (float) a.done / (float) a.total : 0.0f;
            }
            if (head)
                head->set_text("<color=#3ddc84>•</color> Studio Sync");
            if (line)
                line->set_text(l);
            if (sub) {
                sub->set_text(s);
                sub->get_gameObject()->SetActive(!s.empty());
            }
            SetBar(showBar, fill);
        }

        // What to say for a few seconds once the work is done; empty when there's nothing worth saying.
        void NoteDone(Activity::Kind kind) {
            auto const& sum = GetSummary();
            doneLine.clear();
            doneSub.clear();
            if (kind == Activity::Kind::Downloading) {
                if (sum.downloadedLast <= 0 && sum.notFoundLast <= 0)
                    return;
                doneLine = Plural(sum.downloadedLast, "song", "songs") + " downloaded";
                if (sum.notFoundLast > 0)
                    doneSub = Plural(sum.notFoundLast, "song", "songs") + " couldn't be found on BeatSaver";
            } else if (kind == Activity::Kind::Cleaning) {
                auto const& l = GetLooseSongs();
                if (l.state != LooseSongs::State::Done)
                    return;
                doneLine = "Deleted " + Plural(l.progress, "song", "songs");
                doneSub = "restore \"Removed songs\" in Playlist Studio to get them back";
            }
            if (!doneLine.empty())
                doneUntil = Clock::now() + std::chrono::seconds(DONE_SECONDS);
        }

        void Update(bool fromPoll);

        // While it wants to show, check twice a second whether a song started or the menu came back.
        void Poll() {
            if (polling)
                return;
            polling = true;
            BSML::MainThreadScheduler::ScheduleAfterTime(0.5f, []() {
                polling = false;
                Update(true);
            });
        }

        void Update(bool fromPoll) {
            auto const& a = GetActivity();
            if (a.kind != Activity::Kind::None)
                lastKind = a.kind;
            else if (lastKind != Activity::Kind::None) {
                NoteDone(lastKind);
                lastKind = Activity::Kind::None;
            }
            bool done = a.kind == Activity::Kind::None && Clock::now() < doneUntil;
            bool want = preview || (getConfig().ShowIndicator.GetValue() && (a.kind != Activity::Kind::None || done));
            if (!want) {
                if (root && shown) {
                    root->SetActive(false);
                    shown = false;
                }
                return;
            }
            if (!root)
                Build();
            Place();
            // Checking for the menu looks through objects, so only the poll does it; changes in between update text.
            if (fromPoll) {
                bool show = InMenu();
                if (show != shown) {
                    root->SetActive(show);
                    shown = show;
                }
            }
            if (shown)
                Fill(a, done);
            Poll();
        }
    }

    void Refresh() {
        Update(false);
    }

    int SpotCount() {
        return SPOT_COUNT;
    }
    std::string SpotName(int spot) {
        return SPOTS[std::clamp(spot, 0, SPOT_COUNT - 1)].name;
    }
    void SetSpot(int spot) {
        getConfig().IndicatorSpot.SetValue(((spot % SPOT_COUNT) + SPOT_COUNT) % SPOT_COUNT);
        Update(false);
    }

    bool Previewing() {
        return preview;
    }
    void SetPreview(bool on) {
        preview = on;
        Update(true);
    }
}
