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

#include <functional>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

/**
 * The implementation of AuthzManagerExternalState functionality for mongos.
 */
class AuthzManagerExternalStateMongos final : public AuthzManagerExternalState {
    AuthzManagerExternalStateMongos(const AuthzManagerExternalStateMongos&) = delete;
    AuthzManagerExternalStateMongos& operator=(const AuthzManagerExternalStateMongos&) = delete;

public:
    AuthzManagerExternalStateMongos();
    ~AuthzManagerExternalStateMongos() final;

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) final;
    Status getStoredAuthorizationVersion(OperationContext* opCtx, int* outVersion) override;
    Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) final;
    StatusWith<User> getUserObject(OperationContext* opCtx, const UserRequest& userReq) final;
    Status getUserDescription(OperationContext* opCtx,
                              const UserRequest& user,
                              BSONObj* result) final;
    StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                              const std::vector<RoleName>& roleNames,
                                              ResolveRoleOption option) final {
        return {ErrorCodes::NotImplemented, "AuthzMongos::resolveRoles"};
    }

    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roles,
                               PrivilegeFormat showPrivileges,
                               AuthenticationRestrictionsFormat,
                               std::vector<BSONObj>* result) final {
        return {ErrorCodes::NotImplemented, "AuthzMongos::getRolesDescription"};
    }
    Status getRolesAsUserFragment(OperationContext* opCtx,
                                  const std::vector<RoleName>& roles,
                                  AuthenticationRestrictionsFormat,
                                  BSONObj* result) final {
        return {ErrorCodes::NotImplemented, "AuthzMongos::getRolesAsUserFragment"};
    }
    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    StringData dbname,
                                    PrivilegeFormat showPrivileges,
                                    AuthenticationRestrictionsFormat,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result) final {
        return {ErrorCodes::NotImplemented, "AuthzMongos::getRoleDescriptionsForDB"};
    }

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) final;
};

}  // namespace mongo
