// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/remove_shard_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class ShardDrainingStatusCmd : public TypedCommand<ShardDrainingStatusCmd> {
public:
    using Request = ShardDrainingStatus;
    using Response = RemoveShardResponse;

    ShardDrainingStatusCmd() : TypedCommand(Request::kCommandName) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& target = request().getCommandParameter();

            ConfigSvrShardDrainingStatus configsvrRequest{target};
            configsvrRequest.setRemoveShardRequestBase(request().getRemoveShardRequestBase());
            configsvrRequest.setDbName(request().getDbName());

            const auto cmdResponseWithStatus =
                Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
                    opCtx,
                    kPrimaryOnlyReadPreference,
                    DatabaseName::kAdmin,
                    CommandHelpers::filterCommandRequestForPassthrough(configsvrRequest.toBSON()),
                    Shard::RetryPolicy::kIdempotent);

            const auto cmdResponse = uassertStatusOK(cmdResponseWithStatus);
            uassertStatusOK(cmdResponseWithStatus.getValue().commandStatus);
            return Response::parse(cmdResponse.response, IDLParserContext("removeShardResponse"));
        }


    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::shardDrainingStatus));
        }
    };
    std::string help() const override {
        return "get the draining status of a shard from the system.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardDrainingStatusCmd).forRouter();

}  // namespace
}  // namespace mongo
