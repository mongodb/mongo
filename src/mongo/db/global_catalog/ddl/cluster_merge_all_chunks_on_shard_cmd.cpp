// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ClusterMergeAllChunksOnShardCommand final
    : public TypedCommand<ClusterMergeAllChunksOnShardCommand> {
public:
    using Request = ClusterMergeAllChunksOnShard;

    std::string help() const override {
        return "Merge all contiguous chunks on a specific shard\n"
               "usage: {mergeAllChunksOnShard: <ns>, shard: <shard>}";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto nss = ns();
            const auto& req = request();
            const auto& shardId = req.getShard();

            // We allow resolving non-shardId identifiers here because the command allows the shard
            // identifier to be a connection string or host and port and we need to be able to
            // target the shard correctly.
            auto targetShard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(
                opCtx, shardId, true /* allowNonShardIdIdentifiers */));

            ShardSvrMergeAllChunksOnShard shardSvrMergeAllChunksOnShard(nss);
            shardSvrMergeAllChunksOnShard.setDbName(DatabaseName::kAdmin);
            shardSvrMergeAllChunksOnShard.setShard(shardId);
            shardSvrMergeAllChunksOnShard.setMaxNumberOfChunksToMerge(
                req.getMaxNumberOfChunksToMerge());
            shardSvrMergeAllChunksOnShard.setMaxTimeProcessingChunksMS(
                req.getMaxTimeProcessingChunksMS());

            auto swCommandResponse =
                targetShard->runCommand(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kAdmin,
                                        shardSvrMergeAllChunksOnShard.toBSON(),
                                        Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(swCommandResponse));

            Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(nss, boost::none);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::splitChunk));
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterMergeAllChunksOnShardCommand).forRouter();

}  // namespace
}  // namespace mongo
