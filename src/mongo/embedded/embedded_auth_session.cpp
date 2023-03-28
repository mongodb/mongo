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

#include "mongo/platform/basic.h"

#include "mongo/base/shim.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/embedded/not_implemented.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace embedded {
namespace {
const std::set<RoleName> kEmptyRoleNameSet;

class Impl : public UserNameIterator::Impl {
    bool more() const override {
        return false;
    }
    const UserName& get() const override {
        UASSERT_NOT_IMPLEMENTED;
    }

    const UserName& next() override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Impl* doClone() const override {
        return new Impl(*this);
    }
};

class AuthorizationSession : public mongo::AuthorizationSession {
public:
    explicit AuthorizationSession(AuthorizationManager* const authzManager)
        : _authzManager(authzManager) {}

    AuthorizationManager& getAuthorizationManager() override {
        return *_authzManager;
    }

    void startRequest(OperationContext*) override {
        // It is always okay to start a request in embedded.
    }

    void startContractTracking() override {}

    Status addAndAuthorizeUser(OperationContext*,
                               const UserRequest&,
                               boost::optional<Date_t>) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    User* lookupUser(const UserName&) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    boost::optional<UserHandle> getAuthenticatedUser() override {
        UASSERT_NOT_IMPLEMENTED;
    }

    bool shouldIgnoreAuthChecks() override {
        return true;
    }

    bool isAuthenticated() override {
        // It should always be okay to check whether you're authenticated, but on embedded
        // it should always return false
        return false;
    }

    boost::optional<UserName> getAuthenticatedUserName() override {
        return boost::none;
    }

    RoleNameIterator getAuthenticatedRoleNames() override {
        return makeRoleNameIteratorForContainer(kEmptyRoleNameSet);
    }

    void grantInternalAuthorization(Client* client) override {
        // Always okay to do something, on embedded.
    }

    void grantInternalAuthorization(OperationContext* opCtx) override {
        // Always okay to do something, on embedded.
    }

    void logoutAllDatabases(Client*, StringData) override {
        // Since we didn't actively authorize, we do not actively deauthorize.
    }

    void logoutDatabase(Client*, StringData, StringData) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    StatusWith<PrivilegeVector> checkAuthorizedToListCollections(StringData,
                                                                 const BSONObj&) override {
        return PrivilegeVector();
    }

    bool isUsingLocalhostBypass() override {
        return false;
    }

    bool isAuthorizedToParseNamespaceElement(const BSONElement&) override {
        return true;
    }

    bool isAuthorizedToParseNamespaceElement(const NamespaceStringOrUUID&) override {
        return true;
    }

    bool isAuthorizedToCreateRole(const RoleName&) override {
        return true;
    }

    bool isAuthorizedToChangeAsUser(const UserName&, ActionType) override {
        return true;
    }

    bool isAuthenticatedAsUserWithRole(const RoleName&) override {
        return true;
    }

    bool isAuthorizedForPrivilege(const Privilege&) override {
        return true;
    }

    bool isAuthorizedForPrivileges(const std::vector<Privilege>&) override {
        return true;
    }

    bool isAuthorizedForActionsOnResource(const ResourcePattern&, ActionType) override {
        return true;
    }

    bool isAuthorizedForActionsOnResource(const ResourcePattern&, const ActionSet&) override {
        return true;
    }

    bool isAuthorizedForActionsOnNamespace(const NamespaceString&, ActionType) override {
        return true;
    }

    bool isAuthorizedForActionsOnNamespace(const NamespaceString&, const ActionSet&) override {
        return true;
    }

    void setImpersonatedUserData(const UserName&, const std::vector<RoleName>&) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    boost::optional<UserName> getImpersonatedUserName() override {
        UASSERT_NOT_IMPLEMENTED;
    }

    RoleNameIterator getImpersonatedRoleNames() override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void clearImpersonatedUserData() override {
        UASSERT_NOT_IMPLEMENTED;
    }

    bool isCoauthorizedWithClient(Client*, WithLock opClientLock) override {
        return true;
    }

    bool isCoauthorizedWith(const boost::optional<UserName>&) override {
        return true;
    }

    bool isImpersonating() const override {
        return false;
    }

    Status checkCursorSessionPrivilege(OperationContext*,
                                       boost::optional<LogicalSessionId>) override {
        return Status::OK();
    }

    bool isAuthorizedForAnyActionOnAnyResourceInDB(StringData) override {
        return true;
    }

    bool isAuthorizedForAnyActionOnResource(const ResourcePattern&) override {
        return true;
    }

    void verifyContract(const AuthorizationContract* contract) const override {
        // Do nothing
    }

    AuthenticationMode getAuthenticationMode() const override {
        return AuthenticationMode::kNone;
    }

    void logoutSecurityTokenUser(Client* client) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    bool mayBypassWriteBlockingMode() const override {
        return true;
    }

    bool isExpired() const override {
        return false;
    }

protected:
    std::tuple<boost::optional<UserName>*, std::vector<RoleName>*> _getImpersonations() override {
        UASSERT_NOT_IMPLEMENTED;
    }

private:
    AuthorizationManager* const _authzManager;
};

}  // namespace
}  // namespace embedded

namespace {

std::unique_ptr<AuthorizationSession> authorizationSessionCreateImpl(
    AuthorizationManager* authzManager) {
    return std::make_unique<embedded::AuthorizationSession>(authzManager);
}

auto authorizationSessionCreateRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(AuthorizationSession::create, authorizationSessionCreateImpl);

}  // namespace

}  // namespace mongo
