// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCommitCollModCollectionMetadataCommand final
    : public TypedCommand<ShardsvrCommitCollModCollectionMetadataCommand> {
public:
    using Request = ShardsvrCommitCollModCollectionMetadata;

    bool skipApiVersionCheck() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command to commit collMod collection metadata into the shard catalog";
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

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(12591500,
                    "expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            // Use an AlternativeClientRegion to perform the shard catalog writes outside the
            // retryable write session. The shard catalog commit contains its own idempotency
            // logic, and running inside the parent session would conflict with the dummy write.
            {
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrCommitCollModCollectionMetadata");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                bool isPrimaryDbShard =
                    ShardingState::get(newOpCtx.get())->shardId() == request().getPrimaryShardId();
                shard_catalog_commit::commitCollectionMetadataLocally(
                    newOpCtx.get(), ns(), isPrimaryDbShard);
            }

            LOGV2_INFO(12591501,
                       "Persisted collMod collection metadata locally on shard",
                       "ns"_attr = ns());

            // Since no write happened on this txnNumber, we need to make a dummy write so that
            // secondaries can be aware of this txn.
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
MONGO_REGISTER_COMMAND(ShardsvrCommitCollModCollectionMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
