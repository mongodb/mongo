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

#include <memory>
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
#include "mongo/db/namespace_string.h"

namespace mongo {

namespace auth {

struct CreateOrUpdateRoleArgs;
}
class Client;

/**
 * Contains all the authorization logic for a single client connection.  It contains a set of
 * the users which have been authenticated, as well as a set of privileges that have been
 * granted to those users to perform various actions.
 *
 * An AuthorizationSession object is present within every mongo::Client object.
 *
 * Users in the _authenticatedUsers cache may get marked as invalid by the AuthorizationManager,
 * for instance if their privileges are changed by a user or role modification command.  At the
 * beginning of every user-initiated operation startRequest() gets called which updates
 * the cached information about any users who have been marked as invalid.  This guarantees that
 * every operation looks at one consistent view of each user for every auth check required over
 * the lifetime of the operation.
 */
class AuthorizationSession {
    MONGO_DISALLOW_COPYING(AuthorizationSession);

public:
    /**
     * Gets the AuthorizationSession associated with the given "client", or nullptr.
     *
     * The "client" object continues to own the returned AuthorizationSession.
     */
    static AuthorizationSession* get(Client* client);

    /**
     * Gets the AuthorizationSession associated with the given "client", or nullptr.
     *
     * The "client" object continues to own the returned AuthorizationSession.
     */
    static AuthorizationSession* get(Client& client);

    /**
     * Returns false if AuthorizationSession::get(client) would return nullptr.
     */
    static bool exists(Client* client);

    /**
     * Sets the AuthorizationSession associated with "client" to "session".
     *
     * "session" must not be NULL, and it is only legal to call this function once
     * on each instance of "client".
     */
    static void set(Client* client, std::unique_ptr<AuthorizationSession> session);

    // Takes ownership of the externalState.
    explicit AuthorizationSession(std::unique_ptr<AuthzSessionExternalState> externalState);
    ~AuthorizationSession();

    AuthorizationManager& getAuthorizationManager();

    // Should be called at the beginning of every new request.  This performs the checks
    // necessary to determine if localhost connections should be given full access.
    // TODO: try to eliminate the need for this call.
    void startRequest(OperationContext* opCtx);

    /**
     * Adds the User identified by "UserName" to the authorization session, acquiring privileges
     * for it in the process.
     */
    Status addAndAuthorizeUser(OperationContext* opCtx, const UserName& userName);

    // Returns the authenticated user with the given name.  Returns NULL
    // if no such user is found.
    // The user remains in the _authenticatedUsers set for this AuthorizationSession,
    // and ownership of the user stays with the AuthorizationManager
    User* lookupUser(const UserName& name);

    // Gets an iterator over the names of all authenticated users stored in this manager.
    UserNameIterator getAuthenticatedUserNames();

    // Gets an iterator over the roles of all authenticated users stored in this manager.
    RoleNameIterator getAuthenticatedRoleNames();

    // Returns a std::string representing all logged-in users on the current session.
    // WARNING: this std::string will contain NUL bytes so don't call c_str()!
    std::string getAuthenticatedUserNamesToken();

    // Removes any authenticated principals whose authorization credentials came from the given
    // database, and revokes any privileges that were granted via that principal.
    void logoutDatabase(const std::string& dbname);

    // Adds the internalSecurity user to the set of authenticated users.
    // Used to grant internal threads full access.
    void grantInternalAuthorization();

    // Generates a vector of default privileges that are granted to any user,
    // regardless of which roles that user does or does not possess.
    // If localhost exception is active, the permissions include the ability to create
    // the first user and the ability to run the commands needed to bootstrap the system
    // into a state where the first user can be created.
    PrivilegeVector getDefaultPrivileges();

    // Checks if this connection has the privileges necessary to perform a find operation
    // on the supplied namespace identifier.
    Status checkAuthForFind(const NamespaceString& ns, bool hasTerm);

    // Checks if this connection has the privileges necessary to perform a getMore operation on
    // the identified cursor, supposing that cursor is associated with the supplied namespace
    // identifier.
    Status checkAuthForGetMore(const NamespaceString& ns, long long cursorID, bool hasTerm);

    // Checks if this connection has the privileges necessary to perform the given update on the
    // given namespace.
    Status checkAuthForUpdate(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& query,
                              const BSONObj& update,
                              bool upsert);

    // Checks if this connection has the privileges necessary to insert the given document
    // to the given namespace.  Correctly interprets inserts to system.indexes and performs
    // the proper auth checks for index building.
    Status checkAuthForInsert(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& document);

    // Checks if this connection has the privileges necessary to perform a delete on the given
    // namespace.
    Status checkAuthForDelete(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& query);

    // Checks if this connection has the privileges necessary to perform a killCursor on
    // the identified cursor, supposing that cursor is associated with the supplied namespace
    // identifier.
    Status checkAuthForKillCursors(const NamespaceString& ns, long long cursorID);

    // Checks if this connection has the privileges necessary to run the aggregation pipeline
    // specified in 'cmdObj' on the namespace 'ns' either directly on mongoD or via mongoS.
    Status checkAuthForAggregate(const NamespaceString& ns, const BSONObj& cmdObj, bool isMongos);

    // Checks if this connection has the privileges necessary to create 'ns' with the options
    // supplied in 'cmdObj' either directly on mongoD or via mongoS.
    Status checkAuthForCreate(const NamespaceString& ns, const BSONObj& cmdObj, bool isMongos);

    // Checks if this connection has the privileges necessary to modify 'ns' with the options
    // supplied in 'cmdObj' either directly on mongoD or via mongoS.
    Status checkAuthForCollMod(const NamespaceString& ns, const BSONObj& cmdObj, bool isMongos);

    // Checks if this connection has the privileges necessary to grant the given privilege
    // to a role.
    Status checkAuthorizedToGrantPrivilege(const Privilege& privilege);

    // Checks if this connection has the privileges necessary to revoke the given privilege
    // from a role.
    Status checkAuthorizedToRevokePrivilege(const Privilege& privilege);

    // Checks if this connection has the privileges necessary to create a new role
    bool isAuthorizedToCreateRole(const auth::CreateOrUpdateRoleArgs& args);

    // Utility function for isAuthorizedForActionsOnResource(
    //         ResourcePattern::forDatabaseName(role.getDB()), ActionType::grantAnyRole)
    bool isAuthorizedToGrantRole(const RoleName& role);

    // Utility function for isAuthorizedForActionsOnResource(
    //         ResourcePattern::forDatabaseName(role.getDB()), ActionType::grantAnyRole)
    bool isAuthorizedToRevokeRole(const RoleName& role);

    // Utility function for isAuthorizedToChangeOwnPasswordAsUser and
    // isAuthorizedToChangeOwnCustomDataAsUser
    bool isAuthorizedToChangeAsUser(const UserName& userName, ActionType actionType);

    // Returns true if the current session is authenticated as the given user and that user
    // is allowed to change his/her own password
    bool isAuthorizedToChangeOwnPasswordAsUser(const UserName& userName);

    // Returns true if the current session is authorized to list the collections in the given
    // database.
    bool isAuthorizedToListCollections(StringData dbname);

    // Returns true if the current session is authenticated as the given user and that user
    // is allowed to change his/her own customData.
    bool isAuthorizedToChangeOwnCustomDataAsUser(const UserName& userName);

    // Returns true if any of the authenticated users on this session have the given role.
    // NOTE: this does not refresh any of the users even if they are marked as invalid.
    bool isAuthenticatedAsUserWithRole(const RoleName& roleName);

    // Returns true if this session is authorized for the given Privilege.
    //
    // Contains all the authorization logic including handling things like the localhost
    // exception.
    bool isAuthorizedForPrivilege(const Privilege& privilege);

    // Like isAuthorizedForPrivilege, above, except returns true if the session is authorized
    // for all of the listed privileges.
    bool isAuthorizedForPrivileges(const std::vector<Privilege>& privileges);

    // Utility function for isAuthorizedForPrivilege(Privilege(resource, action)).
    bool isAuthorizedForActionsOnResource(const ResourcePattern& resource, ActionType action);

    // Utility function for isAuthorizedForPrivilege(Privilege(resource, actions)).
    bool isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                          const ActionSet& actions);

    // Utility function for
    // isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns), action).
    bool isAuthorizedForActionsOnNamespace(const NamespaceString& ns, ActionType action);

    // Utility function for
    // isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns), actions).
    bool isAuthorizedForActionsOnNamespace(const NamespaceString& ns, const ActionSet& actions);

    // Replaces the data for users that a system user is impersonating with new data.
    // The auditing system adds these users and their roles to each audit record in the log.
    void setImpersonatedUserData(std::vector<UserName> usernames, std::vector<RoleName> roles);

    // Gets an iterator over the names of all users that the system user is impersonating.
    UserNameIterator getImpersonatedUserNames();

    // Gets an iterator over the roles of all users that the system user is impersonating.
    RoleNameIterator getImpersonatedRoleNames();

    // Clears the data for impersonated users.
    void clearImpersonatedUserData();

    // Returns true if the session and 'opClient's AuthorizationSession share an
    // authenticated user. If either object has impersonated users,
    // those users will be considered as 'authenticated' for the purpose of this check.
    //
    // The existence of 'opClient' must be guaranteed through locks taken by the caller.
    bool isCoauthorizedWithClient(Client* opClient);

    // Returns true if the session and 'userNameIter' share an authenticated user, or if both have
    // no authenticated users. Impersonated users are not considered as 'authenticated' for the
    // purpose of this check. This always returns true if auth is not enabled.
    bool isCoauthorizedWith(UserNameIterator userNameIter);

    // Tells whether impersonation is active or not.  This state is set when
    // setImpersonatedUserData is called and cleared when clearImpersonatedUserData is
    // called.
    bool isImpersonating() const;

protected:
    // Builds a vector of all roles held by users who are authenticated on this connection. The
    // vector is stored in _authenticatedRoleNames. This function is called when users are
    // logged in or logged out, as well as when the user cache is determined to be out of date.
    void _buildAuthenticatedRolesVector();

    // All Users who have been authenticated on this connection.
    UserSet _authenticatedUsers;

    // The roles of the authenticated users. This vector is generated when the authenticated
    // users set is changed.
    std::vector<RoleName> _authenticatedRoleNames;

private:
    // If any users authenticated on this session are marked as invalid this updates them with
    // up-to-date information. May require a read lock on the "admin" db to read the user data.
    //
    // When refreshing a user document, we will use the current user's id to confirm that our
    // user is of the same generation as the refreshed user document. If the generations don't
    // match we will remove the outdated user document from the cache.
    void _refreshUserInfoAsNeeded(OperationContext* opCtx);

    // Checks if this connection is authorized for the given Privilege, ignoring whether or not
    // we should even be doing authorization checks in general.  Note: this may acquire a read
    // lock on the admin database (to update out-of-date user privilege information).
    bool _isAuthorizedForPrivilege(const Privilege& privilege);

    std::unique_ptr<AuthzSessionExternalState> _externalState;

    // A vector of impersonated UserNames and a vector of those users' RoleNames.
    // These are used in the auditing system. They are not used for authz checks.
    std::vector<UserName> _impersonatedUserNames;
    std::vector<RoleName> _impersonatedRoleNames;
    bool _impersonationFlag;
};

}  // namespace mongo
