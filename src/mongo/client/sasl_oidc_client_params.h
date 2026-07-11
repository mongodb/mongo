// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using oidcIdPAuthCallbackT = void(std::string_view, std::string_view, std::string_view);

/**
 * OIDC Client parameters
 */
struct OIDCClientGlobalParams {
    /**
     * Access Token. Populated either by configuration or token acquisition flow.
     */
    std::string oidcAccessToken;

    /**
     * Refresh Token. Populated during token acquisition flow.
     */
    std::string oidcRefreshToken;

    /*
     * Callback function that accepts the username, activation code and IdP endpoint and then
     * performs IdP authentication. This should be provided by tests, presumably as a JS function.
     */
    std::function<oidcIdPAuthCallbackT> oidcIdPAuthCallback;
    /**
     * Client ID. Populated via server SASL reply.
     */
    std::string oidcClientId;

    /**
     * Token endpoint. Populated via server SASL reply.
     */
    std::string oidcTokenEndpoint;
};

extern OIDCClientGlobalParams oidcClientGlobalParams;
}  // namespace mongo
