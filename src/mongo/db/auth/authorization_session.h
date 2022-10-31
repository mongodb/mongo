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
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

class Client;
class AuthorizationContract;

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
    static std::unique_ptr<AuthorizationSession> create(AuthorizationManager*);

    AuthorizationSession() = default;

    /**
     * Provides a way to swap out impersonate data for the duration of the ScopedImpersonate's
     * lifetime.
     */
    class ScopedImpersonate {
    public:
        ScopedImpersonate(AuthorizationSession* authSession,
                          boost::optional<UserName>* user,
                          std::vector<RoleName>* roles)
            : _authSession(*authSession), _user(*user), _roles(*roles) {
            swap();
        }

        ~ScopedImpersonate() {
            this->swap();
        }

    private:
        void swap();

        AuthorizationSession& _authSession;
        boost::optional<UserName>& _user;
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
     * Start tracking permissions and privileges in the authorization contract.
     */
    virtual void startContractTracking() = 0;

    /**
     * Adds the User identified by "UserName" to the authorization session, acquiring privileges
     * for it in the process.
     */
    virtual Status addAndAuthorizeUser(OperationContext* opCtx,
                                       const UserRequest& userRequest,
                                       boost::optional<Date_t> expirationTime) = 0;

    // Returns the authenticated user with the given name.  Returns NULL
    // if no such user is found.
    // The user remains in the _authenticatedUsers set for this AuthorizationSession,
    // and ownership of the user stays with the AuthorizationManager
    virtual User* lookupUser(const UserName& name) = 0;

    // Get the authenticated user's object handle, if any.
    virtual boost::optional<UserHandle> getAuthenticatedUser() = 0;

    // Is auth disabled? Returns true if auth is disabled.
    virtual bool shouldIgnoreAuthChecks() = 0;

    // Is authenticated as at least one user.
    virtual bool isAuthenticated() = 0;

    // Gets the name of the currently authenticated user (if any).
    virtual boost::optional<UserName> getAuthenticatedUserName() = 0;

    // Gets an iterator over the roles of all authenticated users stored in this manager.
    virtual RoleNameIterator getAuthenticatedRoleNames() = 0;

    // Removes all authenticated principals while in kSecurityToken authentication mode.
    virtual void logoutSecurityTokenUser(Client* client) = 0;

    // Removes any authenticated principals and revokes any privileges that were granted via those
    // principals. This function modifies state. Synchronizes with the Client lock.
    virtual void logoutAllDatabases(Client* client, StringData reason) = 0;

    // Removes any authenticated principals whose authorization credentials came from the given
    // database, and revokes any privileges that were granted via that principal. This function
    // modifies state. Synchronizes with the Client lock.
    virtual void logoutDatabase(Client* client, StringData dbname, StringData reason) = 0;

    // How the active session is authenticated.
    enum class AuthenticationMode {
        kNone,           // Not authenticated.
        kConnection,     // For the duration of the connection, or until logged out or
                         // expiration.
        kSecurityToken,  // By operation scoped security token.
    };
    virtual AuthenticationMode getAuthenticationMode() const = 0;

    // Adds the internalSecurity user to the set of authenticated users.
    // Used to grant internal threads full access. Takes in the Client
    // as a parameter so it can take out a lock on the client.
    virtual void grantInternalAuthorization(Client* client) = 0;
    virtual void grantInternalAuthorization(OperationContext* opCtx) = 0;

    // Checks if the current session is authorized to list the collections in the given
    // database. If it is, return a privilegeVector containing the privileges used to authorize
    // this command.
    virtual StatusWith<PrivilegeVector> checkAuthorizedToListCollections(StringData dbname,
                                                                         const BSONObj& cmdObj) = 0;

    // Checks if this connection is using the localhost bypass
    virtual bool isUsingLocalhostBypass() = 0;

    // Checks if this connection has the privileges necessary to parse a namespace from a
    // given BSONElement.
    virtual bool isAuthorizedToParseNamespaceElement(const BSONElement& elem) = 0;

    // Checks if this connection has the privileges necessary to parse a namespace from a
    // given NamespaceOrUUID object.
    virtual bool isAuthorizedToParseNamespaceElement(const NamespaceStringOrUUID& nss) = 0;

    // Checks if this connection has the privileges necessary to create a new role
    virtual bool isAuthorizedToCreateRole(const RoleName& roleName) = 0;

    // Utility function for isAuthorizedToChangeOwnPasswordAsUser and
    // isAuthorizedToChangeOwnCustomDataAsUser
    virtual bool isAuthorizedToChangeAsUser(const UserName& userName, ActionType actionType) = 0;

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

    // Returns true if the current session possesses a privilege which could apply to the
    // database resource, or a specific or arbitrary resource within the database.
    virtual bool isAuthorizedForAnyActionOnAnyResourceInDB(StringData dbname) = 0;

    // Returns true if the current session possesses a privilege which applies to the resource.
    virtual bool isAuthorizedForAnyActionOnResource(const ResourcePattern& resource) = 0;

    // Replaces the data for the user that a system user is impersonating with new data.
    // The auditing system adds this user and their roles to each audit record in the log.
    virtual void setImpersonatedUserData(const UserName& username,
                                         const std::vector<RoleName>& roles) = 0;

    // Gets the name of the user, if any, that the system user is impersonating.
    virtual boost::optional<UserName> getImpersonatedUserName() = 0;

    // Gets an iterator over the roles of all users that the system user is impersonating.
    virtual RoleNameIterator getImpersonatedRoleNames() = 0;

    // Clears the data for impersonated users.
    virtual void clearImpersonatedUserData() = 0;

    // Returns true if the session and 'opClient's AuthorizationSession share an
    // authenticated user. If either object has impersonated users,
    // those users will be considered as 'authenticated' for the purpose of this check.
    //
    // The existence of 'opClient' must be guaranteed through locks taken by the caller,
    // as demonstrated by opClientLock which must be a lock taken on opClient.
    //
    // Returns true if the current auth session and the opClient's auth session have users
    // in common.
    virtual bool isCoauthorizedWithClient(Client* opClient, WithLock opClientLock) = 0;

    // Returns true if the specified userName is the currently authenticated user,
    // or if the session is unauthenticated and `boost::none` is specified.
    // Impersonated users are not considered as 'authenticated' for the purpose of this check.
    // This always returns true if auth is not enabled.
    virtual bool isCoauthorizedWith(const boost::optional<UserName>& userName) = 0;

    // Tells whether impersonation is active or not.  This state is set when
    // setImpersonatedUserData is called and cleared when clearImpersonatedUserData is
    // called.
    virtual bool isImpersonating() const = 0;

    // Returns a status encoding whether the current session in the specified `opCtx` has privilege
    // to access a cursor in the specified `cursorSessionId` parameter.  Returns `Status::OK()`,
    // when the session is accessible.  Returns a `mongo::Status` with information regarding the
    // nature of session inaccessibility when the session is not accessible.
    virtual Status checkCursorSessionPrivilege(
        OperationContext* opCtx, boost::optional<LogicalSessionId> cursorSessionId) = 0;

    // Verify the authorization contract. If contract == nullptr, no check is performed.
    virtual void verifyContract(const AuthorizationContract* contract) const = 0;

    // Returns true if any user has the privilege to bypass write blocking mode for the cluster
    // resource.
    virtual bool mayBypassWriteBlockingMode() const = 0;

    // Returns true if the authorization session is expired. When this returns true,
    // isAuthenticated() is also expected to return false.
    virtual bool isExpired() const = 0;

protected:
    virtual std::tuple<boost::optional<UserName>*, std::vector<RoleName>*> _getImpersonations() = 0;
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
