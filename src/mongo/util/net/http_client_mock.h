// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/http_client.h"

#include <cstdint>
#include <string_view>

namespace mongo {

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
            return H::combine(std::move(h), request.method, request.url);
        }
        friend bool operator==(const Request& my, const Request& other) {
            return my.method == other.method && my.url == other.url;
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
