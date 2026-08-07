// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ConfigSvrCommitMergeAllPrecomputedChunksOnShardCommand final
    : public TypedCommand<ConfigSvrCommitMergeAllPrecomputedChunksOnShardCommand> {
public:
    using Request = ConfigSvrCommitMergeAllPrecomputedChunksOnShard;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command only invokable on the config server. Do not call directly. "
               "Must be invoked by a shard";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        ConfigsvrMergeAllPrecomputedChunksOnShardResponse typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " can only be run on the config server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            uassert(12834407,
                    "Expected to be called within a retryable write",
                    opCtx->isRetryableWrite() && TransactionParticipant::get(opCtx));

            // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
            // collections under the exclusive _kChunkOpLock happen on the same term.
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            std::vector<ChunkType> newChunks;
            newChunks.reserve(request().getNewChunks().size());
            for (const auto& chunk : request().getNewChunks()) {
                auto chunkBson = uassertStatusOK(
                    ChunkType::parseFromNetworkRequest(chunk, true /*acceptMissingVersion*/));
                newChunks.emplace_back(std::move(chunkBson));
            }

            ShardingCatalogManager::ShardAndCollectionPlacementVersions placementVersions;
            std::vector<mongo::ChunkType> changedChunks;

            {
                auto newClient = opCtx->getServiceContext()->getService()->makeClient(
                    "CommitMergeAllPrecomputedChunksOnShard");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);
                auto* const newOpCtx = newOpCtxPtr.get();

                AuthorizationSession::get(newOpCtx->getClient())->grantInternalAuthorization();
                newOpCtx->setWriteConcern(opCtx->getWriteConcern());
                repl::ReadConcernArgs::get(newOpCtx) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                std::tie(placementVersions, changedChunks) = uassertStatusOK(
                    ShardingCatalogManager::get(newOpCtx)->commitMergeAllPrecomputedChunksOnShard(
                        newOpCtx, ns(), request().getShard(), std::move(newChunks)));
            }

            std::vector<BSONObj> changedChunkDocs;
            changedChunkDocs.reserve(changedChunks.size());
            for (const auto& chunk : changedChunks) {
                changedChunkDocs.push_back(chunk.toConfigBSON());
            }

            // No write happened on this txnNumber in the parent opCtx, so make a dummy write to
            // protect against older requests with old txnNumbers being replayed.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << "commitMergeAllPrecomputedChunksOnShardStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);

            return {placementVersions.collectionPlacementVersion, std::move(changedChunkDocs)};
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ConfigSvrCommitMergeAllPrecomputedChunksOnShardCommand).forShard();

}  // namespace
}  // namespace mongo
