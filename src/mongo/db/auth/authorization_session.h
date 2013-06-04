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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

    /**
     * Contains all the authorization logic for a single client connection.  It contains a set of
     * the principals which have been authenticated, as well as a set of privileges that have been
     * granted by those principals to perform various actions.
     * An AuthorizationSession object is present within every mongo::Client object, therefore there
     * is one per thread that corresponds to an incoming client connection.
     */
    class AuthorizationSession {
        MONGO_DISALLOW_COPYING(AuthorizationSession);
    public:

        // Checks to see if "doc" is a valid privilege document, assuming it is stored in the
        // "system.users" collection of database "dbname".
        //
        // Returns Status::OK() if the document is good, or Status(ErrorCodes::BadValue), otherwise.
        static Status checkValidPrivilegeDocument(const StringData& dbname, const BSONObj& doc);

        // Takes ownership of the externalState.
        explicit AuthorizationSession(AuthzSessionExternalState* externalState);
        ~AuthorizationSession();

        const AuthorizationManager& getAuthorizationManager() const;

        // Should be called at the beginning of every new request.  This performs the checks
        // necessary to determine if localhost connections should be given full access.
        // TODO: try to eliminate the need for this call.
        void startRequest();

        // Adds "principal" to the authorization manager, and takes ownership of it.
        void addAuthorizedPrincipal(Principal* principal);

        // Returns the authenticated principal with the given name.  Returns NULL
        // if no such user is found.
        // Ownership of the returned Principal remains with _authenticatedPrincipals
        Principal* lookupPrincipal(const UserName& name);

        // Gets an iterator over the names of all authenticated principals stored in this manager.
        PrincipalSet::NameIterator getAuthenticatedPrincipalNames();

        // Removes any authenticated principals whose authorization credentials came from the given
        // database, and revokes any privileges that were granted via that principal.
        void logoutDatabase(const std::string& dbname);

        // Grant this connection the given privilege.
        Status acquirePrivilege(const Privilege& privilege,
                                const UserName& authorizingUser);

        // Adds a new principal with the given principal name and authorizes it with full access.
        // Used to grant internal threads full access.
        void grantInternalAuthorization(const std::string& userName);

        // Checks if this connection has been authenticated as an internal user.
        bool hasInternalAuthorization();

        // Checks if this connection has the privileges required to perform the given action
        // on the given resource.  Contains all the authorization logic including handling things
        // like the localhost exception.  Returns true if the action may proceed on the resource.
        // Note: this may acquire a database read lock (for automatic privilege acquisition).
        bool checkAuthorization(const std::string& resource, ActionType action);

        // Same as above but takes an ActionSet instead of a single ActionType.  Returns true if
        // all of the actions may proceed on the resource.
        bool checkAuthorization(const std::string& resource, ActionSet actions);

        // Parses the privilege documents and acquires all privileges that the privilege document
        // grants
        Status acquirePrivilegesFromPrivilegeDocument(const std::string& dbname,
                                                      const UserName& user,
                                                      const BSONObj& privilegeDocument);

        // Checks if this connection has the privileges necessary to perform a query on the given
        // namespace.
        Status checkAuthForQuery(const std::string& ns);

        // Checks if this connection has the privileges necessary to perform an update on the given
        // namespace.
        Status checkAuthForUpdate(const std::string& ns, bool upsert);

        // Checks if this connection has the privileges necessary to perform an insert to the given
        // namespace.
        Status checkAuthForInsert(const std::string& ns);

        // Checks if this connection has the privileges necessary to perform a delete on the given
        // namespace.
        Status checkAuthForDelete(const std::string& ns);

        // Checks if this connection has the privileges necessary to perform a getMore on the given
        // namespace.
        Status checkAuthForGetMore(const std::string& ns);

        // Checks if this connection is authorized for the given Privilege.
        Status checkAuthForPrivilege(const Privilege& privilege);

        // Checks if this connection is authorized for all the given Privileges.
        Status checkAuthForPrivileges(const vector<Privilege>& privileges);

        // Given a database name and a readOnly flag return an ActionSet describing all the actions
        // that an old-style user with those attributes should be given.
        static ActionSet getActionsForOldStyleUser(const std::string& dbname, bool readOnly);

        // Parses the privilege document and returns a PrivilegeSet of all the Privileges that
        // the privilege document grants.
        static Status buildPrivilegeSet(const std::string& dbname,
                                        const UserName& user,
                                        const BSONObj& privilegeDocument,
                                        PrivilegeSet* result);

    private:
        // Finds the set of privileges attributed to "principal" in database "dbname",
        // and adds them to the set of acquired privileges.
        void _acquirePrivilegesForPrincipalFromDatabase(const std::string& dbname,
                                                        const UserName& user);

        // Checks to see if the given privilege is allowed, performing implicit privilege
        // acquisition if enabled and necessary to resolve the privilege.
        Status _probeForPrivilege(const Privilege& privilege);

        // Parses the old-style (pre 2.4) privilege document and returns a PrivilegeSet of all the
        // Privileges that the privilege document grants.
        static Status _buildPrivilegeSetFromOldStylePrivilegeDocument(
                const std::string& dbname,
                const UserName& user,
                const BSONObj& privilegeDocument,
                PrivilegeSet* result);

        // Parses extended-form (2.4+) privilege documents and returns a PrivilegeSet of all the
        // privileges that the document grants.
        //
        // The document, "privilegeDocument", is assumed to describe privileges for "principal", and
        // to come from database "dbname".
        static Status _buildPrivilegeSetFromExtendedPrivilegeDocument(
                const std::string& dbname,
                const UserName& user,
                const BSONObj& privilegeDocument,
                PrivilegeSet* result);

        // Returns a new privilege that has replaced the actions needed to handle special casing
        // certain namespaces like system.users and system.profile.
        Privilege _modifyPrivilegeForSpecialCases(const Privilege& privilege);


        scoped_ptr<AuthzSessionExternalState> _externalState;

        // All the privileges that have been acquired by the authenticated principals.
        PrivilegeSet _acquiredPrivileges;
        // All principals who have been authenticated on this connection
        PrincipalSet _authenticatedPrincipals;
    };

} // namespace mongo
