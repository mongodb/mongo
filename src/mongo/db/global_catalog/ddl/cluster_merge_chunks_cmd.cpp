// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {
class ClusterMergeChunksCommand : public TypedCommand<ClusterMergeChunksCommand> {
public:
    using Request = ClusterMergeChunks;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Merge Chunks command\n"
               "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ] }";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            auto bounds = request().getBounds();
            uassertStatusOK(ChunkRange::validate(bounds));

            BSONObj minKey = bounds[0];
            BSONObj maxKey = bounds[1];

            sharding::router::CollectionRouter router(opCtx, ns());
            router.route(
                definition()->getName(),
                [&](OperationContext* opCtx, const CollectionRoutingInfo& _) {
                    const auto cri =
                        getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(opCtx, ns());

                    const auto& cm = cri.getChunkManager();

                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "shard key bounds "
                                          << "[" << minKey << "," << maxKey << ")"
                                          << " are not valid for shard key pattern "
                                          << cm.getShardKeyPattern().toBSON(),
                            (cm.getShardKeyPattern().isShardKey(minKey) &&
                             cm.getShardKeyPattern().isShardKey(maxKey)));

                    BSONObj normalizedMinKey = cm.getShardKeyPattern().normalizeShardKey(minKey);
                    BSONObj normalizedMaxKey = cm.getShardKeyPattern().normalizeShardKey(maxKey);

                    const auto firstChunk =
                        cm.findIntersectingChunkWithSimpleCollation(normalizedMinKey);
                    ChunkVersion placementVersion = cm.getVersion(firstChunk.getShardId());

                    ShardsvrMergeChunks req(ns());
                    req.setDbName(DatabaseName::kAdmin);
                    req.setBounds(bounds);
                    req.setEpoch(placementVersion.epoch());
                    req.setTimestamp(placementVersion.getTimestamp());

                    // Throws, but handled at level above.  Don't want to rewrap to preserve
                    // exception formatting.
                    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(
                        opCtx, firstChunk.getShardId()));

                    auto response = uassertStatusOK(
                        shard->runCommand(opCtx,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          DatabaseName::kAdmin,
                                          req.toBSON(),
                                          Shard::RetryPolicy::kNotIdempotent));
                    uassertStatusOK(response.commandStatus);

                    Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(ns(), boost::none);
                });
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
MONGO_REGISTER_COMMAND(ClusterMergeChunksCommand).forRouter();

}  // namespace
}  // namespace mongo
