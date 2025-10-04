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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_oidc_client_params.h"

#include <functional>
#include <string>
#include <utility>

namespace mongo {

class SaslOIDCClientConversation : public SaslClientConversation {
    SaslOIDCClientConversation(const SaslOIDCClientConversation&) = delete;
    SaslOIDCClientConversation& operator=(const SaslOIDCClientConversation&) = delete;

public:
    SaslOIDCClientConversation(SaslClientSession* clientSession,
                               StringData principalName,
                               StringData accessToken)
        : SaslClientConversation(clientSession),
          _principalName(principalName.data()),
          _accessToken(accessToken.data()) {}

    static void setOIDCIdPAuthCallback(std::function<oidcIdPAuthCallbackT> callback) {
        oidcClientGlobalParams.oidcIdPAuthCallback = std::move(callback);
    }

    StatusWith<bool> step(StringData inputData, std::string* outputData) override;

    // Refreshes oidcClientGlobalParams.accessToken using oidcClientGlobalParams.refreshToken,
    // returning the acquired access token if successful.
    static StatusWith<std::string> doRefreshFlow();

private:
    // Step of the conversation - can be 1, 2, or 3.
    int _step{0};

    // Name of the user that is trying to authenticate. It will only be non-empty
    // if the client is trying to use MONGODB-OIDC via the device authorization grant flow.
    std::string _principalName;

    // Serialized access token to be sent to the server. It will only be non-empty if the client is
    // trying to use MONGODB-OIDC with a token obtained out-of-band.
    std::string _accessToken;

    // Generate the opening client-side message. This simply includes the principal name.
    StatusWith<bool> _firstStep(std::string* output);

    // Parse the server's response to the client-side message, which should contain the identity
    // provider's issuer endpoint, and the clientID. Then, perform the
    // device authorization grant flow to retrieve a device code, present a user code and
    // verification uri to the user, and poll the token endpoint with the device code until the user
    // authenticates and a token is provided.
    StatusWith<bool> _secondStep(StringData input, std::string* outputData);
};

}  // namespace mongo
