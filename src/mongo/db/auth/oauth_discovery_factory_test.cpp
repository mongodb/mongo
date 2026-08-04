// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

TEST_F(OAuthDiscoveryFactoryFixture, IssuerAndJWKSUriMustBeSecure) {
    auto defaultMetadata = makeDefaultMetadata();

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

        // Only the issuer and jwks_uri are dereferenced by the server, so only they must be secure
        // at parse time; the remaining endpoints are validated by the client at point of use
        // (SERVER-89723).
        if (field.fieldName() == "jwks_uri"sv || field.fieldName() == "issuer"sv) {
            ASSERT_THROWS(factory.acquire("https://idp.example"), DBException);
        } else {
            OAuthAuthorizationServerMetadata precomputedMetadata =
                OAuthAuthorizationServerMetadata::parse(splicedMetadata,
                                                        IDLParserContext("metadata"));
            ASSERT_EQ(precomputedMetadata, factory.acquire("https://idp.example"));
        }
    }
}

TEST_F(OAuthDiscoveryFactoryFixture, EndpointsMayBeURNs) {
    // Kubernetes-style issuers advertise URNs for endpoints they do not support; the discovery
    // document must still parse so the server can reach the jwks_uri (SERVER-89723, HELP-97856).
    auto defaultMetadata = makeDefaultMetadata();
    BSONObj splicedMetadata = [&] {
        BSONObjBuilder builder;
        builder.append("authorization_endpoint", "urn:kubernetes:programmatic_authorization");
        builder.appendElementsUnique(defaultMetadata.toBSON());
        return builder.obj();
    }();

    std::unique_ptr<MockHttpClient> client = std::make_unique<MockHttpClient>();
    client->expect(
        {HttpClient::HttpMethod::kGET, "https://idp.example/.well-known/openid-configuration"},
        {200, {}, splicedMetadata.jsonString()});
    OAuthDiscoveryFactory factory(std::move(client));

    OAuthAuthorizationServerMetadata precomputedMetadata =
        OAuthAuthorizationServerMetadata::parse(splicedMetadata, IDLParserContext("metadata"));
    ASSERT_EQ(precomputedMetadata, factory.acquire("https://idp.example"));
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
