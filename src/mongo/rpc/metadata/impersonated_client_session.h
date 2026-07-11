// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/util/modules.h"

namespace mongo::rpc {

/**
 * RAII class to optionally set impersonated client attributes .
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ImpersonatedClientSessionGuard {
public:
    ImpersonatedClientSessionGuard(Client* client,
                                   const ImpersonatedClientMetadata& parsedClientMetadata);
    ~ImpersonatedClientSessionGuard();

    ImpersonatedClientSessionGuard(const ImpersonatedClientSessionGuard&) = delete;
    ImpersonatedClientSessionGuard& operator=(const ImpersonatedClientSessionGuard&) = delete;

private:
    boost::optional<AuditClientAttrs> _oldClientAttrs;
    Client* _client;
};

}  // namespace mongo::rpc
