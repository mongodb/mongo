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
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_contract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class Client;

/**
 * Contains all the authorization logic for a single client connection.  It contains a set of
 * the users which have been authenticated, as well as a set of privileges that have been
 * granted to those users to perform various actions.
 *
 * An AuthorizationSession object is present within every mongo::Client object.
 *
 * The active _authenticatedUser may get marked as invalid by the AuthorizationManager,
 * for instance if their privileges are changed by a user or role modification command.  At the
 * beginning of every user-initiated operation startRequest() gets called which updates
 * the cached information about any users who have been marked as invalid.  This guarantees that
 * every operation looks at one consistent view of each user for every auth check required over
 * the lifetime of the operation.
 */
class AuthorizationSessionImpl : public AuthorizationSession {
public:
    struct InstallMockForTestingOrAuthImpl {
        explicit InstallMockForTestingOrAuthImpl() = default;
    };
    explicit AuthorizationSessionImpl(std::unique_ptr<AuthzSessionExternalState> externalState,
                                      InstallMockForTestingOrAuthImpl);

    ~AuthorizationSessionImpl() override;

    AuthorizationManager& getAuthorizationManager() override;

    void startRequest(OperationContext* opCtx) override;

    void startContractTracking() override;

    Status addAndAuthorizeUser(OperationContext* opCtx,
                               const UserRequest& userRequest,
                               boost::optional<Date_t> expirationTime) override;

    User* lookupUser(const UserName& name) override;

    bool shouldIgnoreAuthChecks() override;

    bool isAuthenticated() override;

    boost::optional<UserHandle> getAuthenticatedUser() override;

    boost::optional<UserName> getAuthenticatedUserName() override;

    RoleNameIterator getAuthenticatedRoleNames() override;

    void logoutSecurityTokenUser(Client* client) override;
    void logoutAllDatabases(Client* client, StringData reason) override;
    void logoutDatabase(Client* client, StringData dbname, StringData reason) override;

    AuthenticationMode getAuthenticationMode() const override {
        return _authenticationMode;
    }

    void grantInternalAuthorization(Client* client) override;

    void grantInternalAuthorization(OperationContext* opCtx) override;

    StatusWith<PrivilegeVector> checkAuthorizedToListCollections(StringData dbname,
                                                                 const BSONObj& cmdObj) override;

    bool isUsingLocalhostBypass() override;

    bool isAuthorizedToParseNamespaceElement(const BSONElement& elem) override;

    bool isAuthorizedToParseNamespaceElement(const NamespaceStringOrUUID& nss) override;

    bool isAuthorizedToCreateRole(const RoleName& roleName) override;

    bool isAuthorizedToChangeAsUser(const UserName& userName, ActionType actionType) override;

    bool isAuthenticatedAsUserWithRole(const RoleName& roleName) override;

    bool isAuthorizedForPrivilege(const Privilege& privilege) override;

    bool isAuthorizedForPrivileges(const std::vector<Privilege>& privileges) override;

    bool isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                          ActionType action) override;

    bool isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                          const ActionSet& actions) override;

    bool isAuthorizedForActionsOnNamespace(const NamespaceString& ns, ActionType action) override;

    bool isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                           const ActionSet& actions) override;

    bool isAuthorizedForAnyActionOnAnyResourceInDB(StringData dbname) override;

    bool isAuthorizedForAnyActionOnResource(const ResourcePattern& resource) override;

    void setImpersonatedUserData(const UserName& username,
                                 const std::vector<RoleName>& roles) override;

    boost::optional<UserName> getImpersonatedUserName() override;

    RoleNameIterator getImpersonatedRoleNames() override;

    void clearImpersonatedUserData() override;

    bool isCoauthorizedWithClient(Client* opClient, WithLock opClientLock) override;

    bool isCoauthorizedWith(const boost::optional<UserName>& userName) override;

    bool isImpersonating() const override;

    Status checkCursorSessionPrivilege(OperationContext* opCtx,
                                       boost::optional<LogicalSessionId> cursorSessionId) override;

    void verifyContract(const AuthorizationContract* contract) const override;

    bool mayBypassWriteBlockingMode() const override;

    bool isExpired() const override;

protected:
    friend class AuthorizationSessionImplTestHelper;

    // Updates internal cached authorization state, i.e.:
    // - _mayBypassWriteBlockingMode, reflecting whether the connection is authorized for the
    // privilege of bypassing write blocking mode on cluster resource.
    // - _authenticatedRoleNames, which stores all roles held by users who are authenticated on this
    // connection.
    // - _authenticationMode -- we just update this to None if there are no users on the connection.
    // This function is called whenever the user state changes to keep the internal state up to
    // date.
    void _updateInternalAuthorizationState();

    // The User who has been authenticated on this connection.
    boost::optional<UserHandle> _authenticatedUser;

    // What authentication mode we're currently operating in.
    AuthenticationMode _authenticationMode = AuthenticationMode::kNone;

    // The roles of the authenticated users. This vector is generated when the authenticated
    // users set is changed.
    std::vector<RoleName> _authenticatedRoleNames;

private:
    // If any users authenticated on this session are marked as invalid this updates them with
    // up-to-date information. May require a read lock on the "admin" db to read the user data.
    void _refreshUserInfoAsNeeded(OperationContext* opCtx);


    // Checks if this connection is authorized for the given Privilege, ignoring whether or not
    // we should even be doing authorization checks in general.  Note: this may acquire a read
    // lock on the admin database (to update out-of-date user privilege information).
    bool _isAuthorizedForPrivilege(const Privilege& privilege);

    std::tuple<boost::optional<UserName>*, std::vector<RoleName>*> _getImpersonations() override {
        return std::make_tuple(&_impersonatedUserName, &_impersonatedRoleNames);
    }


    // Generates a vector of default privileges that are granted to any user,
    // regardless of which roles that user does or does not possess.
    // If localhost exception is active, the permissions include the ability to create
    // the first user and the ability to run the commands needed to bootstrap the system
    // into a state where the first user can be created.
    PrivilegeVector _getDefaultPrivileges();

private:
    std::unique_ptr<AuthzSessionExternalState> _externalState;

    // A vector of impersonated UserNames and a vector of those users' RoleNames.
    // These are used in the auditing system. They are not used for authz checks.
    boost::optional<UserName> _impersonatedUserName;
    std::vector<RoleName> _impersonatedRoleNames;
    bool _impersonationFlag;

    // A record of privilege checks and other authorization like function calls made on
    // AuthorizationSession. IDL Typed Commands can optionally define a contract declaring the set
    // of authorization checks they perform. After a command completes running, MongoDB verifies the
    // set of checks performed is a subset of the checks declared in the contract.
    AuthorizationContract _contract;

    bool _mayBypassWriteBlockingMode;

    // The expiration time for this session, expressed as a Unix timestamp. After this time passes,
    // the session will be expired and requests will fail until the expiration time is refreshed.
    // If boost::none, then the session never expires (default behavior).
    boost::optional<Date_t> _expirationTime;

    // If the session is expired, this represents the UserName that was formerly authenticated on
    // this connection.
    boost::optional<UserName> _expiredUserName;
};
}  // namespace mongo
