// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_client_handle.h"

namespace mongo {

StatusWith<UsersInfoReply> AuthorizationClientHandle::sendUsersInfoRequest(
    OperationContext* opCtx, const DatabaseName& dbname, UsersInfoCommand cmd) {
    cmd.setDbName(dbname);
    auto swObj = runAuthorizationReadCommand(opCtx, dbname, cmd.toBSON());
    if (!swObj.isOK()) {
        return swObj.getStatus();
    }

    return UsersInfoReply::parse(swObj.getValue(),
                                 IDLParserContext("AuthzClientHandle::UsersInfoRequest"));
}

StatusWith<RolesInfoReply> AuthorizationClientHandle::sendRolesInfoRequest(
    OperationContext* opCtx, const DatabaseName& dbname, RolesInfoCommand cmd) {
    cmd.setDbName(dbname);
    auto swObj = runAuthorizationReadCommand(opCtx, dbname, cmd.toBSON());
    if (!swObj.isOK()) {
        return swObj.getStatus();
    }

    return RolesInfoReply::parse(swObj.getValue(),
                                 IDLParserContext("AuthzClientHandle::RolesInfoRequest"));
}

}  // namespace mongo
