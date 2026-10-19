// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/ctype.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/http_client.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mongo {

namespace mock_http_client_detail {

// Different libcurl builds percent-encode with either upper- or lower-case hex
// digits; the two encodings are equivalent, so canonicalize to one case for
// URL comparison.
inline std::string canonicalizeUrl(std::string url) {
    for (size_t i = 0; i + 2 < url.size(); ++i) {
        if (url[i] == '%') {
            url[i + 1] = ctype::toUpper(url[i + 1]);
            url[i + 2] = ctype::toUpper(url[i + 2]);
        }
    }
    return url;
}

}  // namespace mock_http_client_detail

class [[MONGO_MOD_PUBLIC]] MockHttpClient : public HttpClient {
public:
    MockHttpClient() = default;

    void allowInsecureHTTP(bool allow) final {
        _allow = allow;
    }

    void setHeaders(const std::vector<std::string>& headers) final {}

    HttpReply request(HttpMethod method,
                      std::string_view url,
                      ConstDataRange data = {nullptr, 0}) const final;

    struct Request {
        HttpMethod method;
        std::string url;

        template <typename H>
        friend H AbslHashValue(H h, const Request& request) {
            return H::combine(std::move(h),
                              request.method,
                              mock_http_client_detail::canonicalizeUrl(request.url));
        }
        friend bool operator==(const Request& my, const Request& other) {
            return my.method == other.method &&
                mock_http_client_detail::canonicalizeUrl(my.url) ==
                mock_http_client_detail::canonicalizeUrl(other.url);
        }
    };
    struct Response {
        std::uint16_t code;
        std::vector<std::string> header;
        std::string body;
    };
    void expect(Request request, Response response) {
        _expectations.emplace(std::move(request), std::move(response));
    }

private:
    bool _allow = false;
    mutable stdx::unordered_map<Request, Response> _expectations;
};

}  // namespace mongo
