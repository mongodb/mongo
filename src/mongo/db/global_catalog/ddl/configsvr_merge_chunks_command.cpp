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
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/version_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers to merge a set of chunks.
 *
 * Format:
 * {
 *   _configsvrCommitChunksMerge: <string namespace>,
 *   collEpoch: <OID epoch>,
 *   lowerBound: <BSONObj minKey>,
 *   upperBound:  <BSONObj maxKey>,
 *   shard: <string shard>,
 *   writeConcern: <BSONObj>
 * }
 */

// TODO (SERVER-127253) Remove this command once v9.0 branches out
class ConfigSvrMergeChunksCommand : public TypedCommand<ConfigSvrMergeChunksCommand> {
public:
    using Request = ConfigSvrMergeChunks;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is sent by a shard to the sharding config server. Do "
               "not call directly. Receives, validates, and processes a ConfigSvrMergeChunks";
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

        ConfigSvrMergeResponse typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCommitChunksMerge can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
            // collections under the exclusive _kChunkOpLock happen on the same term.
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const bool isAuthoritative =
                sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                AuthoritativeMetadataAccessLevelEnum::kNone;

            ShardingCatalogManager::ShardAndCollectionPlacementVersions shardAndCollVers;
            if (!isAuthoritative) {
                // Legacy non-authoritative path: keep the pre-existing behavior. No ACR and no
                // dummy write.
                repl::ReadConcernArgs::get(opCtx) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                shardAndCollVers =
                    uassertStatusOK(ShardingCatalogManager::get(opCtx)->commitChunksMerge(
                        opCtx,
                        ns(),
                        request().getEpoch(),
                        request().getTimestamp(),
                        request().getCollectionUUID(),
                        request().getChunkRange(),
                        request().getShard()));
            } else {
                // Authoritative path. The originating chunk-op coordinator on the data-bearing
                // shard attaches a session id and is responsible for installing the new chunk
                // layout into the local shard catalog after this command returns. Use ACR so the
                // session id stays held while the catalog updates run; the trailing dummy write
                // below bumps the txnNumber on the oplog so that an older request on the same
                // session cannot replay onto a newer state.
                {
                    auto newClient =
                        opCtx->getServiceContext()->getService()->makeClient("CommitChunksMerge");
                    AlternativeClientRegion acr(newClient);
                    auto executor = Grid::get(opCtx->getServiceContext())
                                        ->getExecutorPool()
                                        ->getFixedExecutor();
                    auto newOpCtxPtr = CancelableOperationContext(
                        cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                    AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                        ->grantInternalAuthorization();
                    newOpCtxPtr->setWriteConcern(opCtx->getWriteConcern());
                    repl::ReadConcernArgs::get(newOpCtxPtr.get()) =
                        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                    shardAndCollVers =
                        uassertStatusOK(ShardingCatalogManager::get(newOpCtxPtr.get())
                                            ->commitChunksMerge(newOpCtxPtr.get(),
                                                                ns(),
                                                                request().getEpoch(),
                                                                request().getTimestamp(),
                                                                request().getCollectionUUID(),
                                                                request().getChunkRange(),
                                                                request().getShard()));
                }

                // No write happened on this txnNumber in the parent opCtx, so make a dummy write to
                // protect against older requests with old txnNumbers being replayed.
                DBDirectClient client(opCtx);
                client.update(NamespaceString::kServerConfigurationNamespace,
                              BSON("_id" << "commitChunksMergeStats"),
                              BSON("$inc" << BSON("count" << 1)),
                              true /* upsert */,
                              false /* multi */);
            }

            return ConfigSvrMergeResponse{shardAndCollVers.shardPlacementVersion};
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            if (!AuthorizationSession::get(opCtx->getClient())
                     ->isAuthorizedForActionsOnResource(
                         ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                         ActionType::internal)) {
                uasserted(ErrorCodes::Unauthorized, "Unauthorized");
            }
        }
    };
};
MONGO_REGISTER_COMMAND(ConfigSvrMergeChunksCommand).forShard();

}  // namespace
}  // namespace mongo
