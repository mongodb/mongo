// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/net/http_client.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(HttpClient, HTTPSIsSecure) {
    ASSERT_OK(HttpClient::endpointIsSecure("https://example.com"));
}

TEST(HttpClient, HTTPIsNotSecure) {
    ASSERT_NOT_OK(HttpClient::endpointIsSecure("http://example.com"));
}

TEST(HttpClient, EvilHTTPIsNotSecure) {
    ASSERT_NOT_OK(HttpClient::endpointIsSecure("http://localhost.example.com"));
    ASSERT_NOT_OK(HttpClient::endpointIsSecure("http://localhost:password@example.com"));
}

TEST(HttpClient, LocalhostHTTPIsSecure) {
    ASSERT_OK(HttpClient::endpointIsSecure("http://localhost"));
    ASSERT_OK(HttpClient::endpointIsSecure("http://localhost/"));
    ASSERT_OK(HttpClient::endpointIsSecure("http://localhost/resource"));
    ASSERT_OK(HttpClient::endpointIsSecure("http://localhost:9001"));
    ASSERT_OK(HttpClient::endpointIsSecure("http://localhost:9001/"));
    ASSERT_OK(HttpClient::endpointIsSecure("http://localhost:9001/resource"));
}

}  // namespace mongo
