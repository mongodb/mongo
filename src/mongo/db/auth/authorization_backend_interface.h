/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/tenant_id.h"

namespace mongo::auth {

class AuthorizationBackendInterface {
public:
    class CmdUMCPassthrough;

    static AuthorizationBackendInterface* get(Service* service);
    static void set(Service* service,
                    std::unique_ptr<AuthorizationBackendInterface> backendInterface);

    virtual ~AuthorizationBackendInterface() = default;

    virtual Status initialize(OperationContext* opCtx) {
        return Status::OK();
    }

    /**
     * Return type for resolveRoles().
     * Each member will be populated ONLY IF their corresponding Option flag was specifed.
     * Otherwise, they will be equal to boost::none.
     */
    struct ResolvedRoleData {
        boost::optional<stdx::unordered_set<RoleName>> roles;
        boost::optional<PrivilegeVector> privileges;
        boost::optional<RestrictionDocuments> restrictions;
    };

    using ResolveRoleOption = auth::ResolveRoleOption;

protected:
    virtual Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) = 0;

    virtual Status getUserDescription(OperationContext* opCtx,
                                      const UserRequest& user,
                                      BSONObj* result,
                                      const SharedUserAcquisitionStats& userAcquisitionStats) = 0;

    virtual StatusWith<User> getUserObject(
        OperationContext* opCtx,
        const UserRequest& userReq,
        const SharedUserAcquisitionStats& userAcquisitionStats) = 0;

    virtual StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                                      const std::vector<RoleName>& roleNames,
                                                      ResolveRoleOption option) = 0;

    virtual UsersInfoReply lookupUsers(OperationContext* opCtx, const UsersInfoCommand& cmd) = 0;
    virtual RolesInfoReply lookupRoles(OperationContext* opCtx, const RolesInfoCommand& cmd) = 0;
};
}  // namespace mongo::auth
