// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * The implementation of AuthzSessionExternalState functionality common to mongod and mongos.
 */
class AuthzSessionExternalStateServerCommon : public AuthzSessionExternalState {
    AuthzSessionExternalStateServerCommon(const AuthzSessionExternalStateServerCommon&) = delete;
    AuthzSessionExternalStateServerCommon& operator=(const AuthzSessionExternalStateServerCommon&) =
        delete;

public:
    ~AuthzSessionExternalStateServerCommon() override;

    bool shouldAllowLocalhost() const override;
    bool shouldIgnoreAuthChecks() const override;
    bool serverIsArbiter() const override;

protected:
    AuthzSessionExternalStateServerCommon(Client* client);

    // Checks whether or not localhost connections should be given full access and stores the
    // result in _allowLocalhost.  Currently localhost connections are only given full access
    // if there are no users in the admin database.
    void _checkShouldAllowLocalhost(OperationContext* opCtx);

private:
    bool _allowLocalhost;
};

}  // namespace mongo
