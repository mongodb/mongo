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

#include <boost/optional/optional.hpp>

namespace mongo {
using oidcIdPAuthCallbackT = void(StringData, StringData, StringData);

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
