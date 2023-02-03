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

#include "mongo/platform/basic.h"

#include "mongo/client/sasl_oidc_client_conversation.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/bson/json.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/oidc_protocol_gen.h"
#include "mongo/shell/program_runner.h"
#include "mongo/util/net/http_client.h"

namespace mongo {
namespace {
constexpr auto kClientIdParameterName = "client_id"_sd;
constexpr auto kClientSecretParameterName = "client_secret"_sd;
constexpr auto kRequestScopesParameterName = "scope"_sd;
constexpr auto kGrantTypeParameterName = "grant_type"_sd;
constexpr auto kGrantTypeParameterDeviceCodeValue =
    "urn:ietf:params:oauth:grant-type:device_code"_sd;
constexpr auto kDeviceCodeParameterName = "device_code"_sd;

std::string buildPostBody(StringData clientId,
                          const boost::optional<StringData>& clientSecret,
                          const boost::optional<std::vector<StringData>>& requestScopes,
                          const boost::optional<std::string>& deviceCode) {
    StringBuilder sb;
    sb << kClientIdParameterName << "=" << uriEncode(clientId);

    if (clientSecret && !clientSecret->empty()) {
        sb << "&" << kClientSecretParameterName << "=" << uriEncode(clientSecret.get());
    }

    if (requestScopes && requestScopes->size() > 0) {
        sb << "&" << kRequestScopesParameterName << "=";
        for (std::size_t i = 0; i < requestScopes->size(); i++) {
            sb << uriEncode(requestScopes.get()[i]);
            if (i < requestScopes->size() - 1) {
                sb << uriEncode(" ");
            }
        }
    }

    if (deviceCode && !deviceCode->empty()) {
        // If the device code is provided, the request must explicitly specify the grant type as
        // device code.
        sb << "&" << kGrantTypeParameterName << "=" << kGrantTypeParameterDeviceCodeValue;
        sb << "&" << kDeviceCodeParameterName << "=" << uriEncode(deviceCode.get());
    }

    return sb.str();
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
    const auth::OIDCMechanismServerStep1& serverReply, StringData principalName) {
    auto deviceAuthorizationEndpoint = serverReply.getDeviceAuthorizationEndpoint().get();
    uassert(ErrorCodes::BadValue,
            "Device authorization endpoint in server reply must be an https endpoint or localhost",
            deviceAuthorizationEndpoint.startsWith("https://"_sd) ||
                deviceAuthorizationEndpoint.startsWith("http://localhost"_sd));

    auto clientId = serverReply.getClientId();
    uassert(ErrorCodes::BadValue, "Encountered empty client ID in server reply", !clientId.empty());

    // Construct body of POST request to device authorization endpoint based on provided
    // parameters.
    auto deviceCodeRequest = buildPostBody(clientId,
                                           serverReply.getClientSecret(),
                                           serverReply.getRequestScopes(),
                                           boost::none /* deviceCode */);

    // Retrieve device code and user verification URI from IdP.
    auto httpClient = HttpClient::createWithoutConnectionPool();
    httpClient->setHeaders(
        {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
    BSONObj deviceAuthorizationResponseObj =
        doPostRequest(httpClient.get(), deviceAuthorizationEndpoint, deviceCodeRequest);

    // Simulate end user login via user verification URI.
    auto deviceCode = deviceAuthorizationResponseObj["device_code"_sd].String();
    auto activationEndpoint =
        deviceAuthorizationResponseObj["verification_uri_complete"_sd].String();
    oidcClientGlobalParams.oidcIdPAuthCallback(principalName, activationEndpoint);

    // Poll token endpoint for access and refresh tokens. It should return immediately since
    // the shell blocks on the authenticationSimulator until it completes, but poll anyway.
    auto tokenRequest = buildPostBody(
        clientId, serverReply.getClientSecret(), boost::none /* requestScopes */, deviceCode);

    while (true) {
        BSONObj tokenResponseObj =
            doPostRequest(httpClient.get(), serverReply.getTokenEndpoint(), tokenRequest);

        // The token endpoint will either respond with the tokens or {"error":
        // "authorization pending"}.
        bool hasAccessToken = tokenResponseObj.hasField("access_token"_sd);
        bool hasError = tokenResponseObj.hasField("error"_sd);
        uassert(ErrorCodes::UnknownError,
                fmt::format("Received unrecognized reply from token endpoint: {}",
                            tokenResponseObj.toString()),
                hasAccessToken || hasError);

        if (hasAccessToken) {
            auto accessToken = tokenResponseObj["access_token"_sd].String();

            // If a refresh token was also provided, cache that as well.
            if (tokenResponseObj.hasField("refresh_token"_sd)) {
                return {accessToken, tokenResponseObj["refresh_token"_sd].String()};
            }

            return {accessToken, ""};
        }

        // Assert that the error returned with "authorization pending", which indicates that
        // the token endpoint has not perceived end-user authentication yet and we should
        // poll again.
        uassert(ErrorCodes::UnknownError,
                fmt::format("Received unexpected error from token endpoint: {}",
                            tokenResponseObj["error"_sd].String()),
                tokenResponseObj["error"_sd].String() != "authorization pending");
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

        // The token endpoint must be provided for both device auth and authz code flows.
        auto tokenEndpoint = serverReply.getTokenEndpoint();
        uassert(ErrorCodes::BadValue,
                "Missing or invalid token endpoint in server reply",
                !tokenEndpoint.empty() &&
                    (tokenEndpoint.startsWith("https://"_sd) ||
                     tokenEndpoint.startsWith("http://localhost"_sd)));

        // Try device authorization grant flow first if provided, falling back to authorization code
        // flow.
        if (serverReply.getDeviceAuthorizationEndpoint()) {
            auto tokens = doDeviceAuthorizationGrantFlow(serverReply, _principalName);
            _accessToken = tokens.first;
            oidcClientGlobalParams.oidcAccessToken = tokens.first;
            oidcClientGlobalParams.oidcRefreshToken = tokens.second;
        } else if (serverReply.getAuthorizationEndpoint()) {
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
