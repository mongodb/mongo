// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

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

        uassert(ErrorCodes::InvalidOptions,
                "Capped collection size must be greater than zero",
                size > 0);

        ShardsvrConvertToCappedRequest req;
        req.setSize(size);

        ShardsvrConvertToCapped shardSvrConvertToCappedCommand(nss);
        shardSvrConvertToCappedCommand.setDbName(dbName);
        shardSvrConvertToCappedCommand.setShardsvrConvertToCappedRequest(std::move(req));
        generic_argument_util::setMajorityWriteConcern(shardSvrConvertToCappedCommand,
                                                       &opCtx->getWriteConcern());

        sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
        router.route(getName(), [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
            auto cmdResponse = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                opCtx,
                dbName,
                dbInfo,
                shardSvrConvertToCappedCommand.toBSON(),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);
            const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
        });


        return true;
    }
};
MONGO_REGISTER_COMMAND(ConvertToCappedCmd).forRouter();

}  // namespace
}  // namespace mongo
