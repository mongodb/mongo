// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_router.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class UsersInfoCommand;
class RolesInfoCommand;

class AuthorizationClientHandle {
public:
    AuthorizationClientHandle(const AuthorizationClientHandle&) = delete;
    AuthorizationClientHandle& operator=(const AuthorizationClientHandle&) = delete;

    virtual std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        Client* client) = 0;

    StatusWith<UsersInfoReply> sendUsersInfoRequest(OperationContext* opCtx,
                                                    const DatabaseName& dbname,
                                                    UsersInfoCommand cmd);

    StatusWith<RolesInfoReply> sendRolesInfoRequest(OperationContext* opCtx,
                                                    const DatabaseName& dbname,
                                                    RolesInfoCommand cmd);

    virtual void notifyDDLOperation(OperationContext* opCtx,
                                    AuthorizationRouter* router,
                                    std::string_view op,
                                    const NamespaceString& nss,
                                    const BSONObj& o,
                                    const BSONObj* o2) = 0;

    virtual ~AuthorizationClientHandle() = default;

protected:
    AuthorizationClientHandle() = default;

    virtual StatusWith<BSONObj> runAuthorizationReadCommand(OperationContext* opCtx,
                                                            const DatabaseName& dbname,
                                                            const BSONObj& command) = 0;
};

}  // namespace mongo
