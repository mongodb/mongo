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

class CommitShardRemovalCmd : public TypedCommand<CommitShardRemovalCmd> {
public:
    using Request = CommitShardRemoval;

    CommitShardRemovalCmd() : TypedCommand(Request::kCommandName) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& target = request().getCommandParameter();

            ConfigSvrCommitShardRemoval configsvrRequest{target};
            configsvrRequest.setRemoveShardRequestBase(request().getRemoveShardRequestBase());
            configsvrRequest.setDbName(request().getDbName());

            const auto cmdResponseWithStatus =
                Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
                    opCtx,
                    kPrimaryOnlyReadPreference,
                    DatabaseName::kAdmin,
                    // TODO SERVER-91373: Remove appendMajorityWriteConcern
                    CommandHelpers::appendMajorityWriteConcern(
                        CommandHelpers::filterCommandRequestForPassthrough(
                            configsvrRequest.toBSON()),
                        opCtx->getWriteConcern()),
                    Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponseWithStatus));
        }


    private:
        bool supportsWriteConcern() const override {
            return true;
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
                            ActionType::removeShard));
        }
    };
    std::string help() const override {
        return "remove a shard from the system.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(CommitShardRemovalCmd).forRouter();

}  // namespace
}  // namespace mongo
