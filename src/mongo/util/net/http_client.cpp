/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/net/http_client.h"

#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/util/ctype.h"

namespace mongo {

namespace {
HttpClientProvider* _factory{nullptr};
}

HttpClientProvider::~HttpClientProvider() {}

void registerHTTPClientProvider(HttpClientProvider* factory) {
    invariant(_factory == nullptr);
    _factory = factory;
}

Status HttpClient::endpointIsSecure(StringData url) {
    return [&] {
        if (url.starts_with("https://"))
            return true;
        if (!getTestCommandsEnabled())
            return false;
        constexpr StringData localhostPrefix = "http://localhost"_sd;
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
