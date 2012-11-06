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
#include "mongo/db/auth/acquired_capability.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/capability_set.h"
#include "mongo/db/auth/external_state.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"

namespace mongo {

    /**
     * Contains all the authorization logic for a single client connection.  It contains a set of
     * the principals which have been authenticated, as well as a set of capabilities that have been
     * granted by those principals to perform various actions.
     * An AuthorizationManager object is present within every mongo::Client object, therefore there
     * is one per thread that corresponds to an incoming client connection.
     */
    class AuthorizationManager {
        MONGO_DISALLOW_COPYING(AuthorizationManager);
    public:
        // Takes ownership of the externalState.
        explicit AuthorizationManager(ExternalState* externalState);
        ~AuthorizationManager();

        // Takes ownership of the principal (by putting into _authenticatedPrincipals).
        void addAuthorizedPrincipal(Principal* principal);
        // Removes and deletes the given principal from the set of authenticated principals.
        // Return an error Status if the given principal isn't a member of the
        // _authenticatedPrincipals set.
        Status removeAuthorizedPrincipal(const Principal* principal);

        // Grant this connection the given capability.
        Status acquireCapability(const AcquiredCapability& capability);

        // This should be called when the connection gets authenticated as the internal user.
        // This grants a capability on all the actions for the internal role, with the
        // internalPrincipal as the principal.
        void grantInternalAuthorization();

        // Checks if this connection has the capabilities required to perform the given action
        // on the given resource.  Contains all the authorization logic including handling things
        // like the localhost exception.  If it is authorized, returns the principal that granted
        // the needed capability.  Returns NULL if not authorized.  If the action is authorized but
        // not because of a standard user Principal but for a special reason such as the localhost
        // exception, it returns a pointer to specialAdminPrincipal.
        const Principal* checkAuthorization(const std::string& resource, ActionType action) const;

        // Returns the privilege document with the given user name in the given database. Currently
        // this information comes from the system.users collection in that database.
        static Status getPrivilegeDocument(DBClientBase* conn,
                                           const std::string& dbname,
                                           const std::string& userName,
                                           BSONObj* result);

        // Returns true if there exists at least one privilege document in the given database.
        static bool hasPrivilegeDocument(DBClientBase* conn, const std::string& dbname);

        // Parses the privilege document and returns a CapabilitySet of all the Capabilities that
        // the privilege document grants.
        static Status buildCapabilitySet(const std::string& dbname,
                                         Principal* principal,
                                         const BSONObj& privilegeDocument,
                                         CapabilitySet* result);

    private:

        // Parses the old-style (pre 2.4) privilege document and returns a CapabilitySet of all the
        // Capabilities that the privilege document grants.
        static Status _buildCapabilitySetFromOldStylePrivilegeDocument(const std::string& dbname,
                                                                   Principal* principal,
                                                                   const BSONObj& privilegeDocument,
                                                                   CapabilitySet* result);

        scoped_ptr<ExternalState> _externalState;

        // All the capabilities that have been acquired by the authenticated principals.
        CapabilitySet _aquiredCapabilities;
        // All principals who have been authenticated on this connection
        PrincipalSet _authenticatedPrincipals;
    };

} // namespace mongo
