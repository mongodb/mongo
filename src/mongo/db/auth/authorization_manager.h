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
#include "mongo/db/auth/acquired_privilege.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_external_state.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/auth/privilege_set.h"

namespace mongo {

    /**
     * Internal secret key info.
     */
    struct AuthInfo {
        AuthInfo();
        string user;
        string pwd;
    };
    extern AuthInfo internalSecurity; // set at startup and not changed after initialization.

    /**
     * Contains all the authorization logic for a single client connection.  It contains a set of
     * the principals which have been authenticated, as well as a set of privileges that have been
     * granted by those principals to perform various actions.
     * An AuthorizationManager object is present within every mongo::Client object, therefore there
     * is one per thread that corresponds to an incoming client connection.
     */
    class AuthorizationManager {
        MONGO_DISALLOW_COPYING(AuthorizationManager);
    public:

        static const std::string SERVER_RESOURCE_NAME;
        static const std::string CLUSTER_RESOURCE_NAME;

        // Takes ownership of the externalState.
        explicit AuthorizationManager(AuthExternalState* externalState);
        ~AuthorizationManager();

        // Takes ownership of the principal (by putting into _authenticatedPrincipals).
        void addAuthorizedPrincipal(Principal* principal);

        // Removes and deletes the given principal from the set of authenticated principals.
        // Return an error Status if the given principal isn't a member of the
        // _authenticatedPrincipals set.
        Status removeAuthorizedPrincipal(const Principal* principal);

        // Returns NULL if not found
        // Ownership of the returned Principal remains with _authenticatedPrincipals
        Principal* lookupPrincipal(const std::string& name) const;

        // Grant this connection the given privilege.
        Status acquirePrivilege(const AcquiredPrivilege& privilege);

        // Adds a new principal with the given principal name and authorizes it with full access.
        // Used to grant internal threads full access.
        void grantInternalAuthorization(const std::string& principalName);

        // Checks if this connection has the privileges required to perform the given action
        // on the given resource.  Contains all the authorization logic including handling things
        // like the localhost exception.  If it is authorized, returns the principal that granted
        // the needed privilege.  Returns NULL if not authorized.  If the action is authorized but
        // not because of a standard user Principal but for a special reason such as the localhost
        // exception, it returns a pointer to specialAdminPrincipal.
        const Principal* checkAuthorization(const std::string& resource, ActionType action) const;
        // Same as above but takes an ActionSet instead of a single ActionType.  The one principal
        // returned must be able to perform all the actions in the ActionSet on the given resource.
        const Principal* checkAuthorization(const std::string& resource, ActionSet actions) const;

        // Parses the privilege documents and acquires all privileges that the privilege document
        // grants
        Status acquirePrivilegesFromPrivilegeDocument(const std::string& dbname,
                                                      Principal* principal,
                                                      const BSONObj& privilegeDocument);

        // Returns the privilege document with the given user name in the given database. Currently
        // this information comes from the system.users collection in that database.
        Status getPrivilegeDocument(const std::string& dbname,
                                    const std::string& userName,
                                    BSONObj* result) {
            return _externalState->getPrivilegeDocument(dbname, userName, result);
        }

        // Given a database name and a readOnly flag return an ActionSet describing all the actions
        // that an old-style user with those attributes should be given.
        static ActionSet getActionsForOldStyleUser(const std::string& dbname, bool readOnly);

        // Parses the privilege document and returns a PrivilegeSet of all the Capabilities that
        // the privilege document grants.
        static Status buildPrivilegeSet(const std::string& dbname,
                                        Principal* principal,
                                        const BSONObj& privilegeDocument,
                                        PrivilegeSet* result);

    private:

        // Parses the old-style (pre 2.4) privilege document and returns a PrivilegeSet of all the
        // Privileges that the privilege document grants.
        static Status _buildPrivilegeSetFromOldStylePrivilegeDocument(
                const std::string& dbname,
                Principal* principal,
                const BSONObj& privilegeDocument,
                PrivilegeSet* result);

        scoped_ptr<AuthExternalState> _externalState;

        // All the privileges that have been acquired by the authenticated principals.
        PrivilegeSet _acquiredPrivileges;
        // All principals who have been authenticated on this connection
        PrincipalSet _authenticatedPrincipals;
    };

} // namespace mongo
