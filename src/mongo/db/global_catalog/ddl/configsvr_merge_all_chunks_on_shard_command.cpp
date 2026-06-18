/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ConfigSvrCommitMergeAllChunksOnShardCommand final
    : public TypedCommand<ConfigSvrCommitMergeAllChunksOnShardCommand> {
public:
    using Request = ConfigSvrCommitMergeAllChunksOnShard;

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

        MergeAllChunksOnShardResponse typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " can only be run on the config server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
            // collections under the exclusive _kChunkOpLock happen on the same term.
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const bool isAuthoritative =
                sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                AuthoritativeMetadataAccessLevelEnum::kNone;

            std::pair<ShardingCatalogManager::ShardAndCollectionPlacementVersions, int> response;
            if (!isAuthoritative) {
                // Legacy non-authoritative path: keep the pre-existing behavior. No ACR and no
                // dummy write.
                repl::ReadConcernArgs::get(opCtx) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                response =
                    uassertStatusOK(ShardingCatalogManager::get(opCtx)->commitMergeAllChunksOnShard(
                        opCtx,
                        ns(),
                        request().getShard(),
                        request().getMaxNumberOfChunksToMerge(),
                        request().getMaxTimeProcessingChunksMS()));
            } else {
                // Authoritative path. The originating chunk-op coordinator on the data-bearing
                // shard attaches a session id and is responsible for installing the new chunk
                // layout into the local shard catalog after this command returns. Use ACR so the
                // session id stays held while the catalog updates run; the trailing dummy write
                // below bumps the txnNumber on the oplog so that an older request on the same
                // session cannot replay onto a newer state.
                {
                    auto newClient = opCtx->getServiceContext()->getService()->makeClient(
                        "CommitMergeAllChunksOnShard");
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

                    response = uassertStatusOK(ShardingCatalogManager::get(newOpCtxPtr.get())
                                                   ->commitMergeAllChunksOnShard(
                                                       newOpCtxPtr.get(),
                                                       ns(),
                                                       request().getShard(),
                                                       request().getMaxNumberOfChunksToMerge(),
                                                       request().getMaxTimeProcessingChunksMS()));
                }

                // No write happened on this txnNumber in the parent opCtx, so make a dummy write to
                // protect against older requests with old txnNumbers being replayed.
                DBDirectClient client(opCtx);
                client.update(NamespaceString::kServerConfigurationNamespace,
                              BSON("_id" << "commitMergeAllChunksOnShardStats"),
                              BSON("$inc" << BSON("count" << 1)),
                              true /* upsert */,
                              false /* multi */);
            }

            const auto& [shardAndCollVers, numMergedChunks] = response;
            return MergeAllChunksOnShardResponse{shardAndCollVers.shardPlacementVersion,
                                                 numMergedChunks};
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
MONGO_REGISTER_COMMAND(ConfigSvrCommitMergeAllChunksOnShardCommand).forShard();

}  // namespace
}  // namespace mongo
