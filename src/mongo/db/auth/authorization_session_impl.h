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

#include "authorization_session.h"

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

    Status addAndAuthorizeUser(OperationContext* opCtx, const UserName& userName) override;

    User* lookupUser(const UserName& name) override;

    bool isAuthenticated() override;

    User* getSingleUser() override;

    UserNameIterator getAuthenticatedUserNames() override;

    RoleNameIterator getAuthenticatedRoleNames() override;

    std::string getAuthenticatedUserNamesToken() override;

    void logoutDatabase(const std::string& dbname) override;

    void grantInternalAuthorization() override;

    PrivilegeVector getDefaultPrivileges() override;

    Status checkAuthForFind(const NamespaceString& ns, bool hasTerm) override;

    Status checkAuthForGetMore(const NamespaceString& ns,
                               long long cursorID,
                               bool hasTerm) override;

    Status checkAuthForUpdate(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& query,
                              const BSONObj& update,
                              bool upsert) override;

    Status checkAuthForInsert(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& document) override;

    Status checkAuthForDelete(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& query) override;

    Status checkAuthForKillCursors(const NamespaceString& cursorNss,
                                   UserNameIterator cursorOwner) override;

    Status checkAuthForAggregate(const NamespaceString& ns,
                                 const BSONObj& cmdObj,
                                 bool isMongos) override;

    Status checkAuthForCreate(const NamespaceString& ns,
                              const BSONObj& cmdObj,
                              bool isMongos) override;

    Status checkAuthForCollMod(const NamespaceString& ns,
                               const BSONObj& cmdObj,
                               bool isMongos) override;

    Status checkAuthorizedToGrantPrivilege(const Privilege& privilege) override;

    Status checkAuthorizedToRevokePrivilege(const Privilege& privilege) override;

    bool isUsingLocalhostBypass() override;

    bool isAuthorizedToParseNamespaceElement(const BSONElement& elem) override;

    bool isAuthorizedToCreateRole(const auth::CreateOrUpdateRoleArgs& args) override;

    bool isAuthorizedToGrantRole(const RoleName& role) override;

    bool isAuthorizedToRevokeRole(const RoleName& role) override;

    bool isAuthorizedToChangeAsUser(const UserName& userName, ActionType actionType) override;

    bool isAuthorizedToChangeOwnPasswordAsUser(const UserName& userName) override;

    bool isAuthorizedToListCollections(StringData dbname, const BSONObj& cmdObj) override;

    bool isAuthorizedToChangeOwnCustomDataAsUser(const UserName& userName) override;

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

    void setImpersonatedUserData(std::vector<UserName> usernames,
                                 std::vector<RoleName> roles) override;

    UserNameIterator getImpersonatedUserNames() override;

    RoleNameIterator getImpersonatedRoleNames() override;

    void clearImpersonatedUserData() override;

    bool isCoauthorizedWithClient(Client* opClient) override;

    bool isCoauthorizedWith(UserNameIterator userNameIter) override;

    bool isImpersonating() const override;

    Status checkCursorSessionPrivilege(OperationContext* const opCtx,
                                       boost::optional<LogicalSessionId> cursorSessionId) override;

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
    void _refreshUserInfoAsNeeded(OperationContext* opCtx);


    // Checks if this connection is authorized for the given Privilege, ignoring whether or not
    // we should even be doing authorization checks in general.  Note: this may acquire a read
    // lock on the admin database (to update out-of-date user privilege information).
    bool _isAuthorizedForPrivilege(const Privilege& privilege);

    std::tuple<std::vector<UserName>*, std::vector<RoleName>*> _getImpersonations() override {
        return std::make_tuple(&_impersonatedUserNames, &_impersonatedRoleNames);
    }

    std::unique_ptr<AuthzSessionExternalState> _externalState;

    // A vector of impersonated UserNames and a vector of those users' RoleNames.
    // These are used in the auditing system. They are not used for authz checks.
    std::vector<UserName> _impersonatedUserNames;
    std::vector<RoleName> _impersonatedRoleNames;
    bool _impersonationFlag;
};
}  // namespace mongo
