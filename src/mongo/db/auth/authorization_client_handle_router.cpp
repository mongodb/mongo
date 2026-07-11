// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_client_handle_router.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/sharding_environment/grid.h"

namespace mongo {

StatusWith<BSONObj> AuthorizationClientHandleRouter::runAuthorizationReadCommand(
    OperationContext* opCtx, const DatabaseName& dbname, const BSONObj& command) {
    BSONObjBuilder builder;
    bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, dbname, std::move(command), &builder);
    auto obj = builder.obj();

    if (!ok) {
        return getErrorStatusFromCommandResult(obj);
    }
    return obj;
}

}  // namespace mongo
