/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/idl/idl_parser.h"
#include "mongo/platform/basic.h"

#include "mongo/db/auth/oauth_discovery_factory.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/net/http_client_mock.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class OAuthDiscoveryFactoryFixture : public unittest::Test {
public:
    OAuthAuthorizationServerMetadata makeDefaultMetadata() {
        OAuthAuthorizationServerMetadata metadata;
        metadata.setIssuer("https://idp.example"_sd);
        metadata.setAuthorizationEndpoint("https://idp.example/authorization"_sd);
        metadata.setTokenEndpoint("https://idp.example/token"_sd);
        metadata.setDeviceAuthorizationEndpoint("https://idp.example/dae"_sd);
        metadata.setJwksUri("https://idp.example/jwks"_sd);
        return metadata;
    }
};

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryQueriesOIDC) {
    auto defaultMetadata = makeDefaultMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, defaultMetadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    OAuthAuthorizationServerMetadata metadata = factory.acquire("https://idp.example");

    ASSERT_EQ(defaultMetadata, metadata);
}

TEST_F(OAuthDiscoveryFactoryFixture, LookupsMustBeSecure) {
    auto defaultMetadata = makeDefaultMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, defaultMetadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    ASSERT_THROWS(factory.acquire("http://idp.example"), DBException);
}

TEST_F(OAuthDiscoveryFactoryFixture, EndpointsMustBeSecure) {
    auto defaultMetadata = makeDefaultMetadata();

    for (const auto& field : defaultMetadata.toBSON()) {
        BSONObj splicedMetadata = [&] {
            BSONObjBuilder builder;
            builder.append(field.fieldName(),
                           field.str().replace(0, "https://"_sd.size(), "http://"));
            builder.appendElementsUnique(defaultMetadata.toBSON());
            return builder.obj();
        }();

        std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
        client->expect(
            {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
            {200, {}, splicedMetadata.jsonString()});

        OAuthDiscoveryFactory factory(std::move(client));
        ASSERT_THROWS(factory.acquire("https://idp.example"), DBException);
    }
}

TEST_F(OAuthDiscoveryFactoryFixture, EndpointMayBeInsecureLocalhostUnderTest) {
    auto defaultMetadata = makeDefaultMetadata();

    for (const auto& field : defaultMetadata.toBSON()) {
        BSONObj splicedMetadata = [&] {
            BSONObjBuilder builder;
            builder.append(
                field.fieldName(),
                field.str().replace(0, "https://idp.example"_sd.size(), "http://localhost:9000"));
            builder.appendElementsUnique(defaultMetadata.toBSON());
            return builder.obj();
        }();
        OAuthAuthorizationServerMetadata precomputedMetadata =
            OAuthAuthorizationServerMetadata::parse(IDLParserContext("metadata"), splicedMetadata);

        std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
        client->expect(
            {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
            {200, {}, splicedMetadata.jsonString()});

        OAuthDiscoveryFactory factory(std::move(client));
        ASSERT_EQ(precomputedMetadata, factory.acquire("https://idp.example"));
    }
}
}  // namespace
}  // namespace mongo
