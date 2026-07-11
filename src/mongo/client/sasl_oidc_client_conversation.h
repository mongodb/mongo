// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_oidc_client_params.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] SaslOIDCClientConversation : public SaslClientConversation {
    SaslOIDCClientConversation(const SaslOIDCClientConversation&) = delete;
    SaslOIDCClientConversation& operator=(const SaslOIDCClientConversation&) = delete;

public:
    SaslOIDCClientConversation(SaslClientSession* clientSession,
                               std::string_view principalName,
                               std::string_view accessToken)
        : SaslClientConversation(clientSession),
          _principalName(principalName),
          _accessToken(accessToken) {}

    static void setOIDCIdPAuthCallback(std::function<oidcIdPAuthCallbackT> callback) {
        oidcClientGlobalParams.oidcIdPAuthCallback = std::move(callback);
    }

    StatusWith<bool> step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override {
        return _step;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return _maxStep;
    }

    // Refreshes oidcClientGlobalParams.accessToken using oidcClientGlobalParams.refreshToken,
    // returning the acquired access token if successful.
    static StatusWith<std::string> doRefreshFlow();

private:
    // Step of the conversation - can be 1 or 2.
    std::uint32_t _step{0};
    const std::uint32_t _maxStep = 2;

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
    StatusWith<bool> _secondStep(std::string_view input, std::string* outputData);
};

}  // namespace mongo
