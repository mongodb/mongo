// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/net/http_client.h"

#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/util/ctype.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
HttpClientProvider* _factory{nullptr};
}

HttpClientProvider::~HttpClientProvider() {}

void registerHTTPClientProvider(HttpClientProvider* factory) {
    invariant(_factory == nullptr);
    _factory = factory;
}

Status HttpClient::endpointIsSecure(std::string_view url) {
    return [&] {
        if (url.starts_with("https://"))
            return true;
        if (!getTestCommandsEnabled())
            return false;
        constexpr std::string_view localhostPrefix = "http://localhost"sv;
        if (!url.starts_with(localhostPrefix)) {
            return false;
        }
        url.remove_prefix(localhostPrefix.size());
        if (url.starts_with(':')) {
            url.remove_prefix(1);
            while (!url.empty() && ctype::isDigit(url[0])) {
                url.remove_prefix(1);
            }
        }
        return url.empty() || url.starts_with('/');
    }()
        ? Status::OK()
        : Status(ErrorCodes::IllegalOperation, "Endpoint is not HTTPS");
}

std::unique_ptr<HttpClient> HttpClient::create() {
    invariant(_factory != nullptr);
    return _factory->create();
}

std::unique_ptr<HttpClient> HttpClient::createWithoutConnectionPool() {
    invariant(_factory != nullptr);
    return _factory->createWithoutConnectionPool();
}

std::unique_ptr<HttpClient> HttpClient::createWithFirewall(const std::vector<CIDR>& cidrDenyList) {
    invariant(_factory != nullptr);
    return _factory->createWithFirewall(cidrDenyList);
}

BSONObj HttpClient::getServerStatus() {
    invariant(_factory != nullptr);
    return _factory->getServerStatus();
}

}  // namespace mongo
