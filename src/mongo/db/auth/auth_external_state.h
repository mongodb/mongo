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

    /**
     * Public interface for a class that encapsulates all the information related to system state
     * not stored in AuthorizationManager.  This is primarily to make AuthorizationManager easier
     * to test.  There are two classes that implement this interface, AuthExternalStateImpl, which
     * is what's used for the actual system, and AuthExternalStateMock, which is used in the tests.
     */
    class AuthExternalState {
        MONGO_DISALLOW_COPYING(AuthExternalState);

    public:

        virtual ~AuthExternalState();

        // Returns true if this connection should be treated as if it has full access to do
        // anything, regardless of the current auth state.  Currently the reasons why this could be
        // are that auth isn't enabled, the connection is from localhost and there are admin users,
        // or the connection is a "god" connection.
        virtual bool shouldIgnoreAuthChecks() const = 0;

        // Gets the privilege information document for "principalName" on "dbname".
        //
        // On success, returns Status::OK() and stores a shared-ownership copy of the document into
        // "result".
        virtual Status getPrivilegeDocument(const std::string& dbname,
                                            const PrincipalName& principalName,
                                            BSONObj* result);

    protected:
        AuthExternalState(); // This class should never be instantiated directly.

        // Queries the userNamespace with the given query and returns the privilegeDocument found
        // in *result.  Returns true if it finds a document matching the query, or false if not.
        virtual bool _findUser(const std::string& usersNamespace,
                               const BSONObj& query,
                               BSONObj* result) const = 0;

        // Returns true if there exists at least one privilege document in the given database.
        bool _hasPrivilegeDocument(const std::string& dbname) const;
    };

} // namespace mongo
