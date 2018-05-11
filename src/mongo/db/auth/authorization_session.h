/**
*    Copyright (C) 2018 10gen Inc.
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
}  // namespace auth
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
    AuthorizationSession(const AuthorizationSession&) = delete;
    AuthorizationSession& operator=(const AuthorizationSession&) = delete;

public:
    static MONGO_DECLARE_SHIM(
        (AuthorizationManager * authzManager)->std::unique_ptr<AuthorizationSession>) create;

    AuthorizationSession() = default;

    /**
     * Provides a way to swap out impersonate data for the duration of the ScopedImpersonate's
     * lifetime.
     */
    class ScopedImpersonate {
    public:
        ScopedImpersonate(AuthorizationSession* authSession,
                          std::vector<UserName>* users,
                          std::vector<RoleName>* roles)
            : _authSession(*authSession), _users(*users), _roles(*roles) {
            swap();
        }

        ~ScopedImpersonate() {
            this->swap();
        }

    private:
        void swap();

        AuthorizationSession& _authSession;
        std::vector<UserName>& _users;
        std::vector<RoleName>& _roles;
    };

    friend class ScopedImpersonate;

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
    virtual ~AuthorizationSession() = 0;

    virtual AuthorizationManager& getAuthorizationManager() = 0;

    // Should be called at the beginning of every new request.  This performs the checks
    // necessary to determine if localhost connections should be given full access.
    // TODO: try to eliminate the need for this call.
    virtual void startRequest(OperationContext* opCtx) = 0;

    /**
     * Adds the User identified by "UserName" to the authorization session, acquiring privileges
     * for it in the process.
     */
    virtual Status addAndAuthorizeUser(OperationContext* opCtx, const UserName& userName) = 0;

    // Returns the authenticated user with the given name.  Returns NULL
    // if no such user is found.
    // The user remains in the _authenticatedUsers set for this AuthorizationSession,
    // and ownership of the user stays with the AuthorizationManager
    virtual User* lookupUser(const UserName& name) = 0;

    // Returns the single user on this auth session. If no user is authenticated, or if
    // multiple users are authenticated, this method will throw an exception.
    virtual User* getSingleUser() = 0;

    // Is authenticated as at least one user.
    virtual bool isAuthenticated() = 0;

    // Gets an iterator over the names of all authenticated users stored in this manager.
    virtual UserNameIterator getAuthenticatedUserNames() = 0;

    // Gets an iterator over the roles of all authenticated users stored in this manager.
    virtual RoleNameIterator getAuthenticatedRoleNames() = 0;

    // Returns a std::string representing all logged-in users on the current session.
    // WARNING: this std::string will contain NUL bytes so don't call c_str()!
    virtual std::string getAuthenticatedUserNamesToken() = 0;

    // Removes any authenticated principals whose authorization credentials came from the given
    // database, and revokes any privileges that were granted via that principal.
    virtual void logoutDatabase(const std::string& dbname) = 0;

    // Adds the internalSecurity user to the set of authenticated users.
    // Used to grant internal threads full access.
    virtual void grantInternalAuthorization() = 0;

    // Generates a vector of default privileges that are granted to any user,
    // regardless of which roles that user does or does not possess.
    // If localhost exception is active, the permissions include the ability to create
    // the first user and the ability to run the commands needed to bootstrap the system
    // into a state where the first user can be created.
    virtual PrivilegeVector getDefaultPrivileges() = 0;

    // Checks if this connection has the privileges necessary to perform a find operation
    // on the supplied namespace identifier.
    virtual Status checkAuthForFind(const NamespaceString& ns, bool hasTerm) = 0;

    // Checks if this connection has the privileges necessary to perform a getMore operation on
    // the identified cursor, supposing that cursor is associated with the supplied namespace
    // identifier.
    virtual Status checkAuthForGetMore(const NamespaceString& ns,
                                       long long cursorID,
                                       bool hasTerm) = 0;

    // Checks if this connection has the privileges necessary to perform the given update on the
    // given namespace.
    virtual Status checkAuthForUpdate(OperationContext* opCtx,
                                      const NamespaceString& ns,
                                      const BSONObj& query,
                                      const BSONObj& update,
                                      bool upsert) = 0;

    // Checks if this connection has the privileges necessary to insert the given document
    // to the given namespace.  Correctly interprets inserts to system.indexes and performs
    // the proper auth checks for index building.
    virtual Status checkAuthForInsert(OperationContext* opCtx,
                                      const NamespaceString& ns,
                                      const BSONObj& document) = 0;

    // Checks if this connection has the privileges necessary to perform a delete on the given
    // namespace.
    virtual Status checkAuthForDelete(OperationContext* opCtx,
                                      const NamespaceString& ns,
                                      const BSONObj& query) = 0;

    // Checks if this connection has the privileges necessary to perform a killCursor on
    // the identified cursor, supposing that cursor is associated with the supplied namespace
    // identifier.
    virtual Status checkAuthForKillCursors(const NamespaceString& cursorNss,
                                           UserNameIterator cursorOwner) = 0;

    // Checks if this connection has the privileges necessary to run the aggregation pipeline
    // specified in 'cmdObj' on the namespace 'ns' either directly on mongoD or via mongoS.
    virtual Status checkAuthForAggregate(const NamespaceString& ns,
                                         const BSONObj& cmdObj,
                                         bool isMongos) = 0;

    // Checks if this connection has the privileges necessary to create 'ns' with the options
    // supplied in 'cmdObj' either directly on mongoD or via mongoS.
    virtual Status checkAuthForCreate(const NamespaceString& ns,
                                      const BSONObj& cmdObj,
                                      bool isMongos) = 0;

    // Checks if this connection has the privileges necessary to modify 'ns' with the options
    // supplied in 'cmdObj' either directly on mongoD or via mongoS.
    virtual Status checkAuthForCollMod(const NamespaceString& ns,
                                       const BSONObj& cmdObj,
                                       bool isMongos) = 0;

    // Checks if this connection has the privileges necessary to grant the given privilege
    // to a role.
    virtual Status checkAuthorizedToGrantPrivilege(const Privilege& privilege) = 0;

    // Checks if this connection has the privileges necessary to revoke the given privilege
    // from a role.
    virtual Status checkAuthorizedToRevokePrivilege(const Privilege& privilege) = 0;

    // Checks if this connection is using the localhost bypass
    virtual bool isUsingLocalhostBypass() = 0;

    // Checks if this connection has the privileges necessary to parse a namespace from a
    // given BSONElement.
    virtual bool isAuthorizedToParseNamespaceElement(const BSONElement& elem) = 0;

    // Checks if this connection has the privileges necessary to create a new role
    virtual bool isAuthorizedToCreateRole(const auth::CreateOrUpdateRoleArgs& args) = 0;

    // Utility function for isAuthorizedForActionsOnResource(
    //         ResourcePattern::forDatabaseName(role.getDB()), ActionType::grantAnyRole)
    virtual bool isAuthorizedToGrantRole(const RoleName& role) = 0;

    // Utility function for isAuthorizedForActionsOnResource(
    //         ResourcePattern::forDatabaseName(role.getDB()), ActionType::grantAnyRole)
    virtual bool isAuthorizedToRevokeRole(const RoleName& role) = 0;

    // Utility function for isAuthorizedToChangeOwnPasswordAsUser and
    // isAuthorizedToChangeOwnCustomDataAsUser
    virtual bool isAuthorizedToChangeAsUser(const UserName& userName, ActionType actionType) = 0;

    // Returns true if the current session is authenticated as the given user and that user
    // is allowed to change his/her own password
    virtual bool isAuthorizedToChangeOwnPasswordAsUser(const UserName& userName) = 0;

    // Returns true if the current session is authorized to list the collections in the given
    // database.
    virtual bool isAuthorizedToListCollections(StringData dbname) = 0;

    // Returns true if the current session is authenticated as the given user and that user
    // is allowed to change his/her own customData.
    virtual bool isAuthorizedToChangeOwnCustomDataAsUser(const UserName& userName) = 0;

    // Returns true if any of the authenticated users on this session have the given role.
    // NOTE: this does not refresh any of the users even if they are marked as invalid.
    virtual bool isAuthenticatedAsUserWithRole(const RoleName& roleName) = 0;

    // Returns true if this session is authorized for the given Privilege.
    //
    // Contains all the authorization logic including handling things like the localhost
    // exception.
    virtual bool isAuthorizedForPrivilege(const Privilege& privilege) = 0;

    // Like isAuthorizedForPrivilege, above, except returns true if the session is authorized
    // for all of the listed privileges.
    virtual bool isAuthorizedForPrivileges(const std::vector<Privilege>& privileges) = 0;

    // Utility function for isAuthorizedForPrivilege(Privilege(resource, action)).
    virtual bool isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                  ActionType action) = 0;

    // Utility function for isAuthorizedForPrivilege(Privilege(resource, actions)).
    virtual bool isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                  const ActionSet& actions) = 0;

    // Utility function for
    // isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns), action).
    virtual bool isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                   ActionType action) = 0;

    // Utility function for
    // isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns), actions).
    virtual bool isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                   const ActionSet& actions) = 0;

    // Replaces the data for users that a system user is impersonating with new data.
    // The auditing system adds these users and their roles to each audit record in the log.
    virtual void setImpersonatedUserData(std::vector<UserName> usernames,
                                         std::vector<RoleName> roles) = 0;

    // Gets an iterator over the names of all users that the system user is impersonating.
    virtual UserNameIterator getImpersonatedUserNames() = 0;

    // Gets an iterator over the roles of all users that the system user is impersonating.
    virtual RoleNameIterator getImpersonatedRoleNames() = 0;

    // Clears the data for impersonated users.
    virtual void clearImpersonatedUserData() = 0;

    // Returns true if the session and 'opClient's AuthorizationSession share an
    // authenticated user. If either object has impersonated users,
    // those users will be considered as 'authenticated' for the purpose of this check.
    //
    // The existence of 'opClient' must be guaranteed through locks taken by the caller.
    virtual bool isCoauthorizedWithClient(Client* opClient) = 0;

    // Returns true if the session and 'userNameIter' share an authenticated user, or if both have
    // no authenticated users. Impersonated users are not considered as 'authenticated' for the
    // purpose of this check. This always returns true if auth is not enabled.
    virtual bool isCoauthorizedWith(UserNameIterator userNameIter) = 0;

    // Tells whether impersonation is active or not.  This state is set when
    // setImpersonatedUserData is called and cleared when clearImpersonatedUserData is
    // called.
    virtual bool isImpersonating() const = 0;

    // Returns a status encoding whether the current session in the specified `opCtx` has privilege
    // to access a cursor in the specified `cursorSessionId` parameter.  Returns `Status::OK()`,
    // when the session is accessible.  Returns a `mongo::Status` with information regarding the
    // nature of session inaccessibility when the session is not accessible.
    virtual Status checkCursorSessionPrivilege(
        OperationContext* const opCtx, boost::optional<LogicalSessionId> cursorSessionId) = 0;

protected:
    virtual std::tuple<std::vector<UserName>*, std::vector<RoleName>*> _getImpersonations() = 0;
};

// Returns a status encoding whether the current session in the specified `opCtx` has privilege to
// access a cursor in the specified `cursorSessionId` parameter.  Returns `Status::OK()`, when the
// session is accessible.  Returns a `mongo::Status` with information regarding the nature of
// session inaccessibility when the session is not accessible.
inline Status checkCursorSessionPrivilege(OperationContext* const opCtx,
                                          const boost::optional<LogicalSessionId> cursorSessionId) {
    if (!AuthorizationSession::exists(opCtx->getClient())) {
        return Status::OK();
    }
    auto* const authSession = AuthorizationSession::get(opCtx->getClient());
    return authSession->checkCursorSessionPrivilege(opCtx, cursorSessionId);
}

}  // namespace mongo
