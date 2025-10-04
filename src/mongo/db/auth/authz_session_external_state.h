/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"

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
