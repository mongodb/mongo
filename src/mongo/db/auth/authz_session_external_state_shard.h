// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state_server_common.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * The implementation of AuthzSessionExternalState functionality for the shard cluster role.
 */
class AuthzSessionExternalStateShard : public AuthzSessionExternalStateServerCommon {
    AuthzSessionExternalStateShard(const AuthzSessionExternalStateShard&) = delete;
    AuthzSessionExternalStateShard& operator=(const AuthzSessionExternalStateShard&) = delete;

public:
    AuthzSessionExternalStateShard(Client* client);
    ~AuthzSessionExternalStateShard() override;

    bool shouldIgnoreAuthChecks() const override;

    bool serverIsArbiter() const override;

    void startRequest(OperationContext* opCtx) override;
};

}  // namespace mongo
