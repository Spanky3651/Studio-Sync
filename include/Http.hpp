#pragma once

#include <functional>
#include <optional>
#include <string>

namespace StudioSync::Http {
    struct Response {
        long httpCode = 0;
        int curlCode = 0;  // 0 means the request itself went through
        std::optional<std::string> responseData;
        bool IsSuccessful() const { return curlCode == 0 && httpCode >= 200 && httpCode < 300; }
        // Short reason for the status line when a request fails.
        std::string Describe() const;
    };

    // Runs the request on a worker thread and calls `done` on the main thread.
    // An empty token sends no Authorization header. An empty body means GET.
    void Request(std::string url, std::string token, std::optional<std::string> body, std::function<void(Response)> done);
}
