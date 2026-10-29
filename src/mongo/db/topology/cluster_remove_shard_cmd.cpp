// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/remove_shard_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class RemoveShardCmd final : public BasicCommandWithRequestParser<RemoveShardCmd> {
public:
    using Request = RemoveShard;

    RemoveShardCmd() : BasicCommandWithRequestParser() {}

    std::string help() const override {
        return "[deprecated] remove a shard from the system.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::removeShard)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName&,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        const auto& request = requestParser.request();
        const ShardId target = request.getCommandParameter();

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << Request::kCommandName
                              << " command should be run on admin database",
                request.getDbName().isAdminDB());

        LOGV2_WARNING(10949600,
                      "'removeShard' command is deprecated. Please use the new shard removal API "
                      "(startShardDraining, shardDrainingStatus, commitShardRemoval) instead!");

        ConfigSvrRemoveShard configsvrRequest{target};
        configsvrRequest.setRemoveShardRequestBase(request.getRemoveShardRequestBase());
        configsvrRequest.setDbName(request.getDbName());
        configsvrRequest.setGenericArguments(request.getGenericArguments());
        generic_argument_util::setMajorityWriteConcern(configsvrRequest, &opCtx->getWriteConcern());

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponseWithStatus = uassertStatusOK(configShard->runCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            DatabaseName::kAdmin,
            CommandHelpers::filterCommandRequestForPassthrough(configsvrRequest.toBSON()),
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponseWithStatus));

        CommandHelpers::filterCommandReplyForPassthrough(cmdResponseWithStatus.response, &result);

        return true;
    }

    void validateResult(const BSONObj& resultObj) final {}
};
MONGO_REGISTER_COMMAND(RemoveShardCmd).forRouter();

}  // namespace
}  // namespace mongo
