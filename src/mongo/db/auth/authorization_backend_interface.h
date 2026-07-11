// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

namespace mongo::auth {

class [[MONGO_MOD_PUBLIC]] AuthorizationBackendInterface {
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
