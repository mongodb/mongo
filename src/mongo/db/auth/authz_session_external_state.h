/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

class Principal;
class OperationContext;

/**
 * Public interface for a class that encapsulates all the session information related to system
 * state not stored in AuthorizationSession.  This is primarily to make AuthorizationSession
 * easier to test as well as to allow different implementations in mongos and mongod.
 */
class AuthzSessionExternalState {
    MONGO_DISALLOW_COPYING(AuthzSessionExternalState);

public:
    virtual ~AuthzSessionExternalState();

    AuthorizationManager& getAuthorizationManager();

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
    virtual void startRequest(OperationContext* txn) = 0;

protected:
    // This class should never be instantiated directly.
    AuthzSessionExternalState(AuthorizationManager* authzManager);

    // Pointer to the authorization manager associated with the authorization session
    // that owns this object.
    //
    // TODO(schwerin): Eliminate this back pointer.
    AuthorizationManager* _authzManager;
};

}  // namespace mongo
