// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrFetchCollMetadataCommand final
    : public TypedCommand<ShardsvrFetchCollMetadataCommand> {
public:
    using Request = ShardsvrFetchCollMetadata;

    bool skipApiVersionCheck() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command. Fetches collection and chunk metadata for a specific namespace "
               "from the global catalog, persists it locally in the shard catalog, installs "
               "it authoritatively on this node's in-memory CollectionShardingRuntime, and "
               "invalidates the collection metadata on secondaries.";
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
            // Ensure shard is ready to accept sharded commands.
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            // Ensure interruption on step down/up.
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Check command write concern.
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Assert that the command is running in a retryable write context.
            uassert(10303100,
                    "_shardsvrFetchCollMetadata expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            const auto nss = ns();

            // Use an AlternativeClientRegion to perform the shard catalog writes outside the
            // retryable write session. The shard catalog commit contains its own idempotency
            // logic, and running inside the parent session would conflict with the dummy write
            // we issue below to mark the txn on secondaries.
            {
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrFetchCollMetadata");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                const bool isPrimaryDbShard =
                    ShardingState::get(newOpCtx.get())->shardId() == request().getPrimaryShardId();

                // The clone does not change placement, so every node's CSR is either already up to
                // date or kUnknown and recovers from the durable catalog once the cluster is fully
                // upgraded. Installing the metadata in-memory or emitting an invalidate would only
                // trigger a redundant and expensive refresh.
                shard_catalog_commit::cloneCollectionMetadataLocally(
                    newOpCtx.get(), nss, isPrimaryDbShard);
            }

            LOGV2_INFO(10140202, "Persisted metadata locally on shard", "ns"_attr = nss);

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
MONGO_REGISTER_COMMAND(ShardsvrFetchCollMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
