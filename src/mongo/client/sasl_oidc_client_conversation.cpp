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

#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sasl_oidc_client_conversation.h"
#include "mongo/client/sasl_oidc_client_types_gen.h"
#include "mongo/db/auth/oauth_authorization_server_metadata_gen.h"
#include "mongo/db/auth/oauth_discovery_factory.h"
#include "mongo/db/auth/oidc_protocol_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
constexpr auto kClientIdParameterName = "client_id"_sd;
constexpr auto kRequestScopesParameterName = "scope"_sd;
constexpr auto kGrantTypeParameterName = "grant_type"_sd;
constexpr auto kGrantTypeParameterDeviceCodeValue =
    "urn:ietf:params:oauth:grant-type:device_code"_sd;
constexpr auto kGrantTypeParameterRefreshTokenValue = "refresh_token"_sd;
constexpr auto kDeviceCodeParameterName = "device_code"_sd;
constexpr auto kCodeParameterName = "code"_sd;
constexpr auto kRefreshTokenParameterName = kGrantTypeParameterRefreshTokenValue;

inline void appendPostBodyRequiredParams(StringBuilder* sb, StringData clientId) {
    *sb << kClientIdParameterName << "=" << uriEncode(clientId);
}

inline void appendPostBodyDeviceCodeRequestParams(
    StringBuilder* sb, const boost::optional<std::vector<StringData>>& requestScopes) {
    if (requestScopes) {
        *sb << "&" << kRequestScopesParameterName << "=";
        for (std::size_t i = 0; i < requestScopes->size(); i++) {
            *sb << uriEncode(requestScopes.get()[i]);
            if (i < requestScopes->size() - 1) {
                *sb << "%20";
            }
        }
    }
}

inline void appendPostBodyTokenRequestParams(StringBuilder* sb, StringData deviceCode) {
    // kDeviceCodeParameterName and kCodeParameterName are the same, IDP's use different names.
    *sb << "&" << kGrantTypeParameterName << "=" << kGrantTypeParameterDeviceCodeValue << "&"
        << kDeviceCodeParameterName << "=" << uriEncode(deviceCode) << "&" << kCodeParameterName
        << "=" << uriEncode(deviceCode);
}

inline void appendPostBodyRefreshFlowParams(StringBuilder* sb, StringData refreshToken) {
    *sb << "&" << kGrantTypeParameterName << "=" << kGrantTypeParameterRefreshTokenValue << "&"
        << kRefreshTokenParameterName << "=" << uriEncode(refreshToken);
}

BSONObj doPostRequest(HttpClient* httpClient, StringData endPoint, const std::string& requestBody) {
    auto response = httpClient->post(endPoint, requestBody);
    ConstDataRange responseCdr = response.getCursor();
    StringData responseStr;
    responseCdr.readInto<StringData>(&responseStr);
    return fromjson(responseStr);
}

// @returns {accessToken, refreshToken}
std::pair<std::string, std::string> doDeviceAuthorizationGrantFlow(
    const OAuthAuthorizationServerMetadata& discoveryReply,
    const auth::OIDCMechanismServerStep1& serverReply,
    StringData principalName) {
    auto deviceAuthorizationEndpoint = discoveryReply.getDeviceAuthorizationEndpoint().get();
    uassert(ErrorCodes::BadValue,
            "Device authorization endpoint in server reply must be an https endpoint or localhost",
            deviceAuthorizationEndpoint.startsWith("https://"_sd) ||
                deviceAuthorizationEndpoint.startsWith("http://localhost"_sd));

    auto clientId = serverReply.getClientId();
    uassert(ErrorCodes::BadValue,
            "Encountered empty client ID in server reply",
            clientId && !clientId->empty());

    // Cache clientId for potential refresh flow uses in the future.
    oidcClientGlobalParams.oidcClientId = clientId->toString();

    // Construct body of POST request to device authorization endpoint based on provided
    // parameters.
    StringBuilder deviceCodeRequestSb;
    appendPostBodyRequiredParams(&deviceCodeRequestSb, clientId.value());
    appendPostBodyDeviceCodeRequestParams(&deviceCodeRequestSb, serverReply.getRequestScopes());
    auto deviceCodeRequest = deviceCodeRequestSb.str();

    // Retrieve device code and user verification URI from IdP.
    auto httpClient = HttpClient::createWithoutConnectionPool();
    httpClient->setHeaders(
        {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
    BSONObj deviceAuthorizationResponseObj =
        doPostRequest(httpClient.get(), deviceAuthorizationEndpoint, deviceCodeRequest);

    // Simulate end user login via user verification URI.
    auto deviceAuthorizationResponse = OIDCDeviceAuthorizationResponse::parse(
        IDLParserContext{"oidcDeviceAuthorizationResponse"}, deviceAuthorizationResponseObj);

    // IDP's use different names to refer to the verification url.
    const auto& optURI = deviceAuthorizationResponse.getVerificationUri();
    const auto& optURL = deviceAuthorizationResponse.getVerificationUrl();
    uassert(ErrorCodes::BadValue, "Encountered empty device authorization url", optURI || optURL);
    uassert(ErrorCodes::BadValue,
            "Encounterd both verification_uri and verification_url",
            !(optURI && optURL));
    auto deviceAuthURL = optURI ? optURI.get() : optURL.get();

    oidcClientGlobalParams.oidcIdPAuthCallback(
        principalName, deviceAuthURL, deviceAuthorizationResponse.getUserCode());

    // Poll token endpoint for access and refresh tokens. It should return immediately since
    // the shell blocks on the authenticationSimulator until it completes, but poll anyway.
    StringBuilder tokenRequestSb;
    appendPostBodyRequiredParams(&tokenRequestSb, clientId.value());
    appendPostBodyTokenRequestParams(&tokenRequestSb, deviceAuthorizationResponse.getDeviceCode());
    auto tokenRequest = tokenRequestSb.str();

    while (true) {
        // SASLOIDCClientConversation::_step2() already checked that tokenEndpoint exists in
        // discoveryReply and points to http://localhost or a https:// URL.
        BSONObj tokenResponseObj =
            doPostRequest(httpClient.get(), discoveryReply.getTokenEndpoint().get(), tokenRequest);
        auto tokenResponse =
            OIDCTokenResponse::parse(IDLParserContext{"oidcTokenResponse"}, tokenResponseObj);

        // The token endpoint will either respond with the tokens or {"error":
        // "authorization pending"}.
        bool hasAccessToken = tokenResponse.getAccessToken().has_value();
        bool hasError = tokenResponse.getError().has_value();
        uassert(ErrorCodes::UnknownError,
                fmt::format("Received unrecognized reply from token endpoint: {}",
                            tokenResponseObj.toString()),
                hasAccessToken || hasError);

        if (hasAccessToken) {
            auto accessToken = tokenResponse.getAccessToken()->toString();

            // If a refresh token was also provided, cache that as well.
            if (tokenResponse.getRefreshToken()) {
                return {accessToken, tokenResponse.getRefreshToken()->toString()};
            }

            return {accessToken, ""};
        }

        // Assert that the error returned with "authorization pending", which indicates that
        // the token endpoint has not perceived end-user authentication yet and we should
        // poll again.
        auto error = tokenResponse.getError()->toString();
        uassert(ErrorCodes::UnknownError,
                fmt::format("Received unexpected error from token endpoint: {}", error),
                error == "authorization pending");
    }

    MONGO_UNREACHABLE
}

std::pair<std::string, std::string> doAuthorizationCodeFlow(
    const auth::OIDCMechanismServerStep1& serverReply) {
    // TODO SERVER-73969 Add authorization code flow support.
    uasserted(ErrorCodes::NotImplemented, "Authorization code flow is not yet supported");
}

}  // namespace
OIDCClientGlobalParams oidcClientGlobalParams;

StatusWith<bool> SaslOIDCClientConversation::step(StringData inputData, std::string* outputData) {
    switch (++_step) {
        case 1:
            return _firstStep(outputData);
        case 2:
            return _secondStep(inputData, outputData);
        default:
            return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                                    str::stream()
                                        << "Invalid client OIDC authentication step: " << _step);
    }
}

StatusWith<std::string> SaslOIDCClientConversation::doRefreshFlow() try {
    // The refresh flow can only be performed if a successful auth attempt has already occurred.
    uassert(ErrorCodes::IllegalOperation,
            "Cannot perform refresh flow without previously-successful auth attempt",
            !oidcClientGlobalParams.oidcRefreshToken.empty() &&
                !oidcClientGlobalParams.oidcClientId.empty() &&
                !oidcClientGlobalParams.oidcTokenEndpoint.empty());

    StringBuilder refreshFlowRequestBuilder;
    appendPostBodyRequiredParams(&refreshFlowRequestBuilder, oidcClientGlobalParams.oidcClientId);
    appendPostBodyRefreshFlowParams(&refreshFlowRequestBuilder,
                                    oidcClientGlobalParams.oidcRefreshToken);

    auto refreshFlowRequestBody = refreshFlowRequestBuilder.str();

    auto httpClient = HttpClient::createWithoutConnectionPool();
    httpClient->setHeaders(
        {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
    BSONObj refreshFlowResponseObj = doPostRequest(
        httpClient.get(), oidcClientGlobalParams.oidcTokenEndpoint, refreshFlowRequestBody);
    auto refreshResponse =
        OIDCTokenResponse::parse(IDLParserContext{"oidcRefreshResponse"}, refreshFlowResponseObj);

    // New tokens should be supplied immediately.
    uassert(ErrorCodes::UnknownError,
            "Failed to retrieve refreshed access token",
            refreshResponse.getAccessToken());
    if (refreshResponse.getRefreshToken()) {
        oidcClientGlobalParams.oidcRefreshToken = refreshResponse.getRefreshToken()->toString();
    }

    return refreshResponse.getAccessToken()->toString();
} catch (const DBException& ex) {
    return ex.toStatus();
}

StatusWith<bool> SaslOIDCClientConversation::_firstStep(std::string* outputData) {
    // If an access token was provided without a username, proceed to the second step and send it
    // directly to the server.
    if (_principalName.empty()) {
        if (_accessToken.empty()) {
            return Status(ErrorCodes::AuthenticationFailed,
                          "Either a username or an access token must be provided for the "
                          "MONGODB-OIDC mechanism");
        }
        try {
            auto ret = _secondStep("", outputData);
            ++_step;
            return ret;
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    // If the username is provided, then request information needed to contact the identity provider
    // from the server.
    auth::OIDCMechanismClientStep1 firstClientRequest;
    firstClientRequest.setPrincipalName(StringData(_principalName));
    auto firstClientRequestBSON = firstClientRequest.toBSON();
    *outputData = std::string(firstClientRequestBSON.objdata(), firstClientRequestBSON.objsize());
    return false;
}

StatusWith<bool> SaslOIDCClientConversation::_secondStep(StringData input,
                                                         std::string* outputData) try {
    // If the client already has a non-empty access token, then token acquisition can be skipped.
    if (_accessToken.empty()) {
        // Currently, only device authorization flow is supported for token acquisition.
        // Parse device authorization endpoint from input.
        ConstDataRange inputCdr(input.rawData(), input.size());
        auto payload = inputCdr.read<Validated<BSONObj>>().val;
        auto serverReply = auth::OIDCMechanismServerStep1::parse(
            IDLParserContext{"oidcServerStep1Reply"}, payload);

        auto issuer = serverReply.getIssuer();

        OAuthDiscoveryFactory discoveryFactory(HttpClient::create());
        OAuthAuthorizationServerMetadata discoveryReply = discoveryFactory.acquire(issuer);

        // The token endpoint must be provided for both device auth and authz code flows.
        auto tokenEndpoint = discoveryReply.getTokenEndpoint();
        uassert(ErrorCodes::BadValue,
                "Missing or invalid token endpoint in server reply",
                tokenEndpoint && !tokenEndpoint->empty() &&
                    (tokenEndpoint->startsWith("https://"_sd) ||
                     tokenEndpoint->startsWith("http://localhost"_sd)));

        // Cache the token endpoint for potential reuse during the refresh flow.
        oidcClientGlobalParams.oidcTokenEndpoint = tokenEndpoint->toString();

        // Try device authorization grant flow first if provided, falling back to authorization code
        // flow.
        if (discoveryReply.getDeviceAuthorizationEndpoint()) {
            auto tokens =
                doDeviceAuthorizationGrantFlow(discoveryReply, serverReply, _principalName);
            _accessToken = tokens.first;
            oidcClientGlobalParams.oidcAccessToken = tokens.first;
            oidcClientGlobalParams.oidcRefreshToken = tokens.second;
        } else if (discoveryReply.getAuthorizationEndpoint()) {
            auto tokens = doAuthorizationCodeFlow(serverReply);
            _accessToken = tokens.first;
            oidcClientGlobalParams.oidcAccessToken = tokens.first;
            oidcClientGlobalParams.oidcRefreshToken = tokens.second;
        } else {
            uasserted(ErrorCodes::BadValue,
                      "Missing device authorization and authorization endpoint in server reply");
        }
    }

    auth::OIDCMechanismClientStep2 secondClientRequest;
    secondClientRequest.setJWT(_accessToken);
    auto bson = secondClientRequest.toBSON();
    *outputData = std::string(bson.objdata(), bson.objsize());

    return true;
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
