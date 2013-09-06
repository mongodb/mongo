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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_set.h"

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

        // Takes ownership of the externalState.
        explicit AuthorizationSession(AuthzSessionExternalState* externalState);
        ~AuthorizationSession();

        AuthorizationManager& getAuthorizationManager();

        // Should be called at the beginning of every new request.  This performs the checks
        // necessary to determine if localhost connections should be given full access.
        // TODO: try to eliminate the need for this call.
        void startRequest();

        /**
         * Adds the User identified by "UserName" to the authorization session, acquiring privileges
         * for it in the process.
         */
        Status addAndAuthorizeUser(const UserName& userName);

        // Returns the authenticated user with the given name.  Returns NULL
        // if no such user is found.
        // The user remains in the _authenticatedUsers set for this AuthorizationSession,
        // and ownership of the user stays with the AuthorizationManager
        User* lookupUser(const UserName& name);

        // Gets an iterator over the names of all authenticated users stored in this manager.
        UserSet::NameIterator getAuthenticatedUserNames();

        // Returns a string representing all logged-in users on the current session.
        // WARNING: this string will contain NUL bytes so don't call c_str()!
        std::string getAuthenticatedUserNamesToken();

        // Removes any authenticated principals whose authorization credentials came from the given
        // database, and revokes any privileges that were granted via that principal.
        void logoutDatabase(const std::string& dbname);

        // Adds the internalSecurity user to the set of authenticated users.
        // Used to grant internal threads full access.
        void grantInternalAuthorization();

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

        // Checks if this connection has the privileges necessary to perform the given query on the
        // given namespace.
        Status checkAuthForQuery(const std::string& ns, const BSONObj& query);

        // Checks if this connection has the privileges necessary to perform a getMore on the given
        // cursor in the given namespace.
        Status checkAuthForGetMore(const std::string& ns, long long cursorID);

        // Checks if this connection has the privileges necessary to perform the given update on the
        // given namespace.
        Status checkAuthForUpdate(const std::string& ns,
                                  const BSONObj& query,
                                  const BSONObj& update,
                                  bool upsert);

        // Checks if this connection has the privileges necessary to insert the given document
        // to the given namespace.  Correctly interprets inserts to system.indexes and performs
        // the proper auth checks for index building.
        Status checkAuthForInsert(const std::string& ns, const BSONObj& document);

        // Checks if this connection has the privileges necessary to perform a delete on the given
        // namespace.
        Status checkAuthForDelete(const std::string& ns, const BSONObj& query);

        // Checks if this connection is authorized for the given Privilege.
        Status checkAuthForPrivilege(const Privilege& privilege);

        // Checks if this connection is authorized for all the given Privileges.
        Status checkAuthForPrivileges(const vector<Privilege>& privileges);

    private:

        // Checks if this connection is authorized for the given Privilege, ignoring whether or not
        // we should even be doing authorization checks in general.
        Status _checkAuthForPrivilegeHelper(const Privilege& privilege);

        // Returns a new privilege that has replaced the actions needed to handle special casing
        // certain namespaces like system.users and system.profile.  Note that the special handling
        // of system.indexes takes place in checkAuthForInsert, not here.
        Privilege _modifyPrivilegeForSpecialCases(const Privilege& privilege);


        scoped_ptr<AuthzSessionExternalState> _externalState;

        // All Users who have been authenticated on this connection
        UserSet _authenticatedUsers;
    };

} // namespace mongo
