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
*/

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/principal_name.h"

namespace mongo {

    class Principal;

    /**
     * Public interface for a class that encapsulates all the information related to system state
     * not stored in AuthorizationManager.  This is primarily to make AuthorizationManager easier
     * to test.  There are two classes that implement this interface, AuthExternalStateImpl, which
     * is what's used for the actual system, and AuthExternalStateMock, which is used in the tests.
     */
    class AuthSessionExternalState {
        MONGO_DISALLOW_COPYING(AuthSessionExternalState);

    public:

        virtual ~AuthSessionExternalState();

        // Returns true if this connection should be treated as if it has full access to do
        // anything, regardless of the current auth state.  Currently the reasons why this could be
        // are that auth isn't enabled, the connection is from localhost and there are no admin
        // users, or the connection is a "god" connection.
        // NOTE: _checkShouldAllowLocalhost MUST be called at least once before any call to
        // shouldIgnoreAuthChecks or we could ignore auth checks incorrectly.
        virtual bool shouldIgnoreAuthChecks() const = 0;

        // Should be called at the beginning of every new request.  This performs the checks
        // necessary to determine if localhost connections should be given full access.
        virtual void startRequest() = 0;

        // Gets the privilege information document for "principalName" on "dbname".
        //
        // On success, returns Status::OK() and stores a shared-ownership copy of the document into
        // "result".
        Status getPrivilegeDocument(const std::string& dbname,
                                    const PrincipalName& principalName,
                                    BSONObj* result);

        // Authorization event hooks

        // Handle any global state which needs to be updated when a new user has been authorized
        virtual void onAddAuthorizedPrincipal(Principal*) = 0;

        // Handle any global state which needs to be updated when a user logs out
        virtual void onLogoutDatabase(const std::string& dbname) = 0;

    protected:
        AuthSessionExternalState(); // This class should never be instantiated directly.

        // Queries the userNamespace with the given query and returns the privilegeDocument found
        // in *result.  Returns true if it finds a document matching the query, or false if not.
        virtual bool _findUser(const std::string& usersNamespace,
                               const BSONObj& query,
                               BSONObj* result) const = 0;

        // Returns true if there exists at least one privilege document in the given database.
        bool _hasPrivilegeDocument(const std::string& dbname) const;
    };

} // namespace mongo
