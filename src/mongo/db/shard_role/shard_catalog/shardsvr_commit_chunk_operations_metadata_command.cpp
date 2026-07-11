// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCommitChunkOperationsMetadataCommand final
    : public TypedCommand<ShardsvrCommitChunkOperationsMetadataCommand> {
public:
    using Request = ShardsvrCommitChunkOperationsMetadata;

    bool skipApiVersionCheck() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command to commit chunk operations metadata into the shard catalog";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            boost::optional<rss::consensus::WriteIntentGuard> writeGuard;
            if (gFeatureFlagIntentRegistration.isEnabled()) {
                writeGuard.emplace(opCtx);
            }

            // TODO (SERVER-105181): Remove the RSTL/canAcceptWrites guard once intent registration
            // makes it redundant.
            {
                repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
                auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Node is not primary",
                        replCoord->canAcceptWritesForDatabase(opCtx, ns().dbName()));
                opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            }

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(12698801,
                    "expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            // Use an AlternativeClientRegion to perform the shard catalog writes outside the
            // retryable write session. The shard catalog commit contains its own idempotency
            // logic, and running inside the parent session would conflict with the dummy write.
            {
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrCommitChunkOperationsMetadata");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());

                shard_catalog_commit::commitChunkOperationsMetadataLocally(
                    newOpCtx.get(),
                    ns(),
                    request().getNewChunks(),
                    request().getReceivingFirstChunk());
            }

            LOGV2_INFO(12698802,
                       "Committed chunk operations metadata locally on shard",
                       "ns"_attr = ns(),
                       "newChunks"_attr = request().getNewChunks().size());

            // Since no write happened on this txnNumber, make a dummy write so secondaries can
            // observe the retryable write.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << Request::kCommandName),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
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
MONGO_REGISTER_COMMAND(ShardsvrCommitChunkOperationsMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
