#include "Http.hpp"

#include <filesystem>
#include <thread>

#include "main.hpp"

#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "libcurl/shared/curl.h"
#include "libcurl/shared/easy.h"

namespace StudioSync::Http {
    namespace {
        // Quest's native TLS can't use the system CA store on newer Horizon OS builds,
        // so the qmod ships Mozilla's bundle here (see fileCopies in mod.template.json).
        constexpr char const* CA_BUNDLE = "/sdcard/ModData/com.beatgames.beatsaber/Mods/StudioSync/cacert.pem";
        constexpr long MAX_BYTES = 4000000;

        size_t Write(void* data, size_t size, size_t count, std::string* out) {
            size_t n = size * count;
            if (out->size() + n > (size_t) MAX_BYTES)
                return 0;  // abort oversized responses
            out->append((char const*) data, n);
            return n;
        }

        Response Perform(std::string const& url, std::string const& token, std::optional<std::string> const& body) {
            Response r;
            CURL* curl = curl_easy_init();
            if (!curl) {
                r.curlCode = -1;
                return r;
            }
            curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Accept: application/json");
            headers = curl_slist_append(headers, "Content-Type: application/json");
            std::string auth;
            if (!token.empty()) {
                auth = "Authorization: Bearer " + token;
                headers = curl_slist_append(headers, auth.c_str());
            }
            std::string out, agent = std::string("StudioSync/") + VERSION + " (Quest Beat Saber)";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
            // The device token travels in a header, so certificates are always checked.
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            std::error_code ec;
            if (std::filesystem::exists(CA_BUNDLE, ec))
                curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE);
            else
                curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
            if (body) {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) body->size());
            }
            auto res = curl_easy_perform(curl);
            r.curlCode = (int) res;
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.httpCode);
                r.responseData = std::move(out);
            } else {
                logger.error("Request to {} failed: curl {} {}", url, (int) res, curl_easy_strerror(res));
            }
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return r;
        }
    }

    std::string Response::Describe() const {
        if (curlCode == CURLE_PEER_FAILED_VERIFICATION || curlCode == CURLE_SSL_CACERT_BADFILE)
            return "certificate check failed";
        if (curlCode == CURLE_COULDNT_RESOLVE_HOST || curlCode == CURLE_COULDNT_CONNECT || curlCode == CURLE_OPERATION_TIMEDOUT)
            return "no connection";
        if (curlCode != 0)
            return "network error " + std::to_string(curlCode);
        return "server answered " + std::to_string(httpCode);
    }

    void Request(std::string url, std::string token, std::optional<std::string> body, std::function<void(Response)> done) {
        std::thread([url = std::move(url), token = std::move(token), body = std::move(body), done = std::move(done)]() {
            auto r = Perform(url, token, body);
            BSML::MainThreadScheduler::Schedule([r = std::move(r), done]() mutable { done(std::move(r)); });
        }).detach();
    }
}
