// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo {

class Principal;
class OperationContext;

/**
 * Public interface for a class that encapsulates all the session information related to system
 * state not stored in AuthorizationSession.  This is primarily to make AuthorizationSession
 * easier to test as well as to allow different implementations in mongos and mongod.
 */
class AuthzSessionExternalState {
    AuthzSessionExternalState(const AuthzSessionExternalState&) = delete;
    AuthzSessionExternalState& operator=(const AuthzSessionExternalState&) = delete;

public:
    virtual ~AuthzSessionExternalState();

    // Returns true if this connection should be treated as if it has full access to do
    // anything, regardless of the current auth state.  Currently the reasons why this could be
    // are that auth isn't enabled or the connection is a "god" connection.
    virtual bool shouldIgnoreAuthChecks() const = 0;

    // Returns true if this connection should be treated as a localhost connection with no
    // admin authentication users created. This condition is used to allow the creation of
    // the first user on a server with authorization enabled.
    // NOTE: _checkShouldAllowLocalhost MUST be called at least once before any call to
    // shouldAllowLocalhost or we could ignore auth checks incorrectly.
    virtual bool shouldAllowLocalhost() const = 0;

    // Returns true if this connection should allow extra server configuration actions under
    // the localhost exception. This condition is used to allow special privileges on arbiters.
    // See SERVER-5479 for details on when this may be removed.
    virtual bool serverIsArbiter() const = 0;

    // Should be called at the beginning of every new request.  This performs the checks
    // necessary to determine if localhost connections should be given full access.
    virtual void startRequest(OperationContext* opCtx) = 0;

protected:
    // This class should never be instantiated directly.
    AuthzSessionExternalState(Client* client);

    // Pointer to the client that owns the owning AuthorizationSession.
    Client* _client;
};

}  // namespace mongo
