/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/commit_chunk_migration_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Config-server command that commits a chunk migration to the global catalog and returns
 * 'changedChunks': the chunk documents the commit produced (the migrated chunk, any side chunks
 * created by a moveRange split, and the donor control chunk). Participants use this list to update
 * their durable shard catalog and in-memory routing metadata.
 *
 * The commit is idempotent: a retry that finds the migration already applied rebuilds and returns
 * the same 'changedChunks' without committing again.
 *
 * It must be issued as a retryable write: a session id is required, and a dummy write advances the
 * session's txnNumber so a stale request cannot replay onto a newer state.
 */

ChunkType toChunkType(const MigratedChunkType& migratedChunk) {
    ChunkType chunk;
    chunk.setRange({migratedChunk.getMin(), migratedChunk.getMax()});
    chunk.setVersion(migratedChunk.getLastmod());
    return chunk;
}

class ConfigSvrCommitMoveRangeCommand : public TypedCommand<ConfigSvrCommitMoveRangeCommand> {
public:
    using Request = CommitMoveRangeRequest;
    using Response = ConfigSvrCommitMoveRangeResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "should not be calling this directly";
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

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCommitMoveRange can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // The command is always issued as a retryable write, so a session id must be present.
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCommitMoveRange must be run with a session id",
                    opCtx->getLogicalSessionId() && opCtx->getTxnNumber());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Mark the opCtx always-interruptible so the metadata reads and writes all happen on
            // the same term. Take the RSTL and confirm this node can accept writes first, so the
            // opCtx is not left unkilled if a stepdown races with the flag (see SERVER-105214).
            // TODO (SERVER-105181): Remove the RSTL/canAcceptWrites guard once intent registration
            // makes it redundant.
            boost::optional<rss::consensus::WriteIntentGuard> writeGuard;
            if (gFeatureFlagIntentRegistration.isEnabled()) {
                writeGuard.emplace(opCtx);
            }
            {
                repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
                auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Node is not primary",
                        replCoord->canAcceptWritesForDatabase(opCtx, ns().dbName()));
                opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            }

            const NamespaceString nss = ns();
            auto migratedChunk = toChunkType(request().getMigratedChunk());

            std::vector<ChunkType> changedChunks;
            {
                // Run the commit under an alternative client so the caller's session id stays held.
                auto newClient =
                    opCtx->getServiceContext()->getService()->makeClient("CommitMoveRange");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                    ->grantInternalAuthorization();
                newOpCtxPtr->setWriteConcern(opCtx->getWriteConcern());
                repl::ReadConcernArgs::get(newOpCtxPtr.get()) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                changedChunks = uassertStatusOK(
                    ShardingCatalogManager::get(newOpCtxPtr.get())
                        ->commitMoveRange(newOpCtxPtr.get(),
                                          nss,
                                          migratedChunk,
                                          request().getDonorShardVersionPreMigration(),
                                          request().getFromShard(),
                                          request().getToShard()));
            }

            // The commit ran on a separate opCtx, so make a dummy write here to advance the
            // session's txnNumber and fence replays of older requests.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << "commitMoveRangeStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);

            std::vector<BSONObj> changedChunkDocs;
            changedChunkDocs.reserve(changedChunks.size());
            for (const auto& chunk : changedChunks) {
                changedChunkDocs.push_back(chunk.toConfigBSON());
            }

            return Response{std::move(changedChunkDocs)};
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
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
MONGO_REGISTER_COMMAND(ConfigSvrCommitMoveRangeCommand).forShard();

}  // namespace
}  // namespace mongo
