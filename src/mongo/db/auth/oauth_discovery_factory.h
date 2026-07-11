// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/oauth_authorization_server_metadata_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/http_client.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {

/**
 * Uses RFC8414 to acquire Authorization Server metadata for an issuer.
 */
class [[MONGO_MOD_PUBLIC]] OAuthDiscoveryFactory {
public:
    OAuthDiscoveryFactory(std::unique_ptr<HttpClient> client) : _client(std::move(client)) {}

    /**
     * Resolve the issuer provided into its metadata payload.
     */
    OAuthAuthorizationServerMetadata acquire(std::string_view issuer);

private:
    std::unique_ptr<HttpClient> _client;
};

}  // namespace mongo
