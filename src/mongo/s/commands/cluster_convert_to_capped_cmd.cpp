/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ConvertToCappedCmd : public BasicCommand {
public:
    ConvertToCappedCmd() : BasicCommand("convertToCapped") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::convertToCapped)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        const long long size = cmdObj.getField("size").safeNumberLong();

        ShardsvrConvertToCappedRequest req;
        req.setSize(size);

        ShardsvrConvertToCapped shardSvrConvertToCappedCommand(nss);
        shardSvrConvertToCappedCommand.setDbName(dbName);
        shardSvrConvertToCappedCommand.setShardsvrConvertToCappedRequest(std::move(req));

        const CachedDatabaseInfo dbInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.dbName()));
        auto cmdResponse = executeCommandAgainstDatabasePrimary(
            opCtx,
            dbName,
            dbInfo,
            CommandHelpers::appendMajorityWriteConcern(shardSvrConvertToCappedCommand.toBSON({}),
                                                       opCtx->getWriteConcern()),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);

        const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
        uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

        return true;
    }
};
MONGO_REGISTER_COMMAND(ConvertToCappedCmd).forRouter();

}  // namespace
}  // namespace mongo
