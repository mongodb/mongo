// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state_server_common.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * The implementation of AuthzSessionExternalState functionality for the router cluster role.
 */
class AuthzSessionExternalStateRouter : public AuthzSessionExternalStateServerCommon {
    AuthzSessionExternalStateRouter(const AuthzSessionExternalStateRouter&) = delete;
    AuthzSessionExternalStateRouter& operator=(const AuthzSessionExternalStateRouter&) = delete;

public:
    AuthzSessionExternalStateRouter(Client* client);
    ~AuthzSessionExternalStateRouter() override;

    void startRequest(OperationContext* opCtx) override;
};

}  // namespace mongo
