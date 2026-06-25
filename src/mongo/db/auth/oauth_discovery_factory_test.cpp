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

#include "mongo/db/auth/oauth_discovery_factory.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/http_client_mock.h"

#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class OAuthDiscoveryFactoryFixture : public unittest::Test {
public:
    static constexpr auto kIssuer = "https://idp.example"sv;
    static constexpr auto kAuthorizationEndpoint = "https://idp.example/authorization"sv;
    static constexpr auto kTokenEndpoint = "https://idp.example/token"sv;
    static constexpr auto kDeviceAuthorizationEndpoint = "https://idp.example/dae"sv;
    static constexpr auto kJWKSUri = "https://idp.example/jwks"sv;

    // Includes every expected field in OIDC discovery document.
    OAuthAuthorizationServerMetadata makeDefaultMetadata() {
        OAuthAuthorizationServerMetadata metadata;
        metadata.setIssuer(kIssuer);
        metadata.setAuthorizationEndpoint(kAuthorizationEndpoint);
        metadata.setTokenEndpoint(kTokenEndpoint);
        metadata.setDeviceAuthorizationEndpoint(kDeviceAuthorizationEndpoint);
        metadata.setJwksUri(kJWKSUri);
        return metadata;
    }

    // Includes only the required issuer and JWKS URI fields.
    OAuthAuthorizationServerMetadata makeRequiredOnlyMetadata() {
        OAuthAuthorizationServerMetadata metadata;
        metadata.setIssuer(kIssuer);
        metadata.setJwksUri(kJWKSUri);
        return metadata;
    }

    // Omits the required JWKS URI field. Constructs BSON directly since serializing
    // OAuthAuthorizationServerMetadata with missing required fields triggers an invariant.
    BSONObj makeWithoutJWKSUriMetadata() {
        return BSON("issuer"sv << kIssuer << "authorization_endpoint"sv << kAuthorizationEndpoint
                               << "token_endpoint"sv << kTokenEndpoint
                               << "device_authorization_endpoint"sv
                               << kDeviceAuthorizationEndpoint);
    }

    // Omit the required issuer field. Constructs BSON directly since serializing
    // OAuthAuthorizationServerMetadata with missing required fields triggers an invariant.
    BSONObj makeWithoutIssuerMetadata() {
        return BSON("authorization_endpoint"sv
                    << kAuthorizationEndpoint << "token_endpoint"sv << kTokenEndpoint
                    << "device_authorization_endpoint"sv << kDeviceAuthorizationEndpoint
                    << "jwks_uri" << kJWKSUri);
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

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryRequiredFieldsOnly) {
    auto requiredMetadata = makeRequiredOnlyMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, requiredMetadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    OAuthAuthorizationServerMetadata metadata = factory.acquire("https://idp.example");

    ASSERT_EQ(requiredMetadata, metadata);
}

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryMissingJWKSUri) {
    auto missingJWKSUriMetadata = makeWithoutJWKSUriMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, missingJWKSUriMetadata.jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    ASSERT_THROWS(factory.acquire("https://idp.example"), DBException);
}

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryMissingIssuer) {
    auto missingIssuerMetadata = makeWithoutIssuerMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, missingIssuerMetadata.jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    ASSERT_THROWS(factory.acquire("https://idp.example"), DBException);
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

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryIssuerWithFwdSlash) {
    auto defaultMetadata = makeDefaultMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, defaultMetadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    OAuthAuthorizationServerMetadata metadata = factory.acquire("https://idp.example/");

    ASSERT_EQ(defaultMetadata, metadata);
}

TEST_F(OAuthDiscoveryFactoryFixture, AllEndpointsMustBeSecure) {
    auto defaultMetadata = makeDefaultMetadata();

    // Every URL advertised in the discovery document must be secure (https, or localhost under
    // test). A malicious server must not be able to direct the client to make plaintext requests
    // to an arbitrary host, even for endpoints the server itself does not use (e.g. the token and
    // device authorization endpoints, which are followed by the client).
    for (const auto& field : defaultMetadata.toBSON()) {
        BSONObj splicedMetadata = [&] {
            BSONObjBuilder builder;
            builder.append(field.fieldName(),
                           field.str().replace(0, "https://"sv.size(), "http://"));
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
        // The issuer must continue to match the requested issuer (RFC 8414 §3.3), so changing it to
        // localhost while requesting https://idp.example is covered separately by
        // LocalhostIssuerMatchesUnderTest.
        if (field.fieldName() == "issuer"sv) {
            continue;
        }

        BSONObj splicedMetadata = [&] {
            BSONObjBuilder builder;
            builder.append(
                field.fieldName(),
                field.str().replace(0, "https://idp.example"sv.size(), "http://localhost:9000"));
            builder.appendElementsUnique(defaultMetadata.toBSON());
            return builder.obj();
        }();
        OAuthAuthorizationServerMetadata precomputedMetadata =
            OAuthAuthorizationServerMetadata::parse(splicedMetadata, IDLParserContext("metadata"));

        std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
        client->expect(
            {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
            {200, {}, splicedMetadata.jsonString()});

        OAuthDiscoveryFactory factory(std::move(client));
        ASSERT_EQ(precomputedMetadata, factory.acquire("https://idp.example"));
    }
}

TEST_F(OAuthDiscoveryFactoryFixture, LocalhostIssuerMatchesUnderTest) {
    // A localhost issuer is permitted under test as long as the returned metadata issuer matches
    // the requested issuer.
    OAuthAuthorizationServerMetadata metadata;
    metadata.setIssuer("http://localhost:9000"sv);
    metadata.setJwksUri("http://localhost:9000/jwks"sv);

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "http://localhost:9000/.well-known/openid-configuration"},
        {200, {}, metadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    ASSERT_EQ(metadata, factory.acquire("http://localhost:9000"));
}

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryIssuerMustMatchRequestedIssuer) {
    // RFC 8414 §3.3: a returned metadata issuer that differs from the issuer used to construct the
    // discovery URL must be rejected, even when it is itself a secure URL. This prevents a
    // malicious server from redirecting the client to an attacker-controlled discovery document.
    auto defaultMetadata = makeDefaultMetadata();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://other.example/.well-known/openid-configuration"},
        {200, {}, defaultMetadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    ASSERT_THROWS(factory.acquire("https://other.example"), DBException);
}

TEST_F(OAuthDiscoveryFactoryFixture, DiscoveryIssuerMatchesWithTrailingSlashMismatch) {
    // A trailing slash difference between the requested issuer and the returned metadata issuer is
    // tolerated; the comparison normalizes a single trailing slash on each side.
    OAuthAuthorizationServerMetadata metadata;
    metadata.setIssuer("https://idp.example/"sv);
    metadata.setJwksUri(kJWKSUri);

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, metadata.toBSON().jsonString()});

    OAuthDiscoveryFactory factory(std::move(client));
    ASSERT_EQ(metadata, factory.acquire("https://idp.example"));
}
}  // namespace
}  // namespace mongo
