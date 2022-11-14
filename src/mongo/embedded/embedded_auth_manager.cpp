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
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/embedded/not_implemented.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace embedded {
namespace {
class AuthorizationManager : public mongo::AuthorizationManager {
public:
    std::unique_ptr<AuthorizationSession> makeAuthorizationSession() override {
        return AuthorizationSession::create(this);
    }

    void setShouldValidateAuthSchemaOnStartup(const bool check) override {
        _shouldValidate = check;
    }
    bool shouldValidateAuthSchemaOnStartup() override {
        return _shouldValidate;
    }
    void setAuthEnabled(const bool state) override {
        invariant(!state);
    }

    bool isAuthEnabled() const override {
        return false;
    }

    Status getAuthorizationVersion(OperationContext*, int*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    OID getCacheGeneration() override {
        UASSERT_NOT_IMPLEMENTED;
    }

    bool hasAnyPrivilegeDocuments(OperationContext*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status getUserDescription(OperationContext*, const UserName&, BSONObj*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status rolesExist(OperationContext*, const std::vector<RoleName>&) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    StatusWith<ResolvedRoleData> resolveRoles(OperationContext*,
                                              const std::vector<RoleName>&,
                                              ResolveRoleOption) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status getRolesDescription(OperationContext*,
                               const std::vector<RoleName>&,
                               PrivilegeFormat,
                               AuthenticationRestrictionsFormat,
                               std::vector<BSONObj>*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status getRolesAsUserFragment(OperationContext*,
                                  const std::vector<RoleName>&,
                                  AuthenticationRestrictionsFormat,
                                  BSONObj*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status getRoleDescriptionsForDB(OperationContext*,
                                    const DatabaseName&,
                                    PrivilegeFormat,
                                    AuthenticationRestrictionsFormat,
                                    bool,
                                    std::vector<BSONObj>*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    StatusWith<UserHandle> acquireUser(OperationContext*, const UserName&) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    StatusWith<UserHandle> reacquireUser(OperationContext*, const UserHandle&) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void invalidateUserByName(OperationContext*, const UserName& user) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void invalidateUsersFromDB(OperationContext*, const DatabaseName& dbname) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void invalidateUsersByTenant(OperationContext*, const boost::optional<TenantId>&) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status refreshExternalUsers(OperationContext* opCtx) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    Status initialize(OperationContext* opCtx) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void invalidateUserCache(OperationContext*) override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void logOp(OperationContext*,
               StringData,
               const NamespaceString&,
               const BSONObj&,
               const BSONObj*) override { /* do nothing*/
    }

    std::vector<AuthorizationManager::CachedUserInfo> getUserCacheInfo() const override {
        UASSERT_NOT_IMPLEMENTED;
    }

    void updatePinnedUsersList(std::vector<UserName>) override {
        UASSERT_NOT_IMPLEMENTED;
    }

private:
    bool _shouldValidate = false;
};

}  // namespace
}  // namespace embedded

namespace {

std::unique_ptr<AuthorizationManager> authorizationManagerCreateImpl(ServiceContext*) {
    return std::make_unique<embedded::AuthorizationManager>();
}

auto authorizationManagerCreateRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(AuthorizationManager::create, authorizationManagerCreateImpl);

}  // namespace

}  // namespace mongo
