// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/shard_role/shard_catalog/commit_database_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

class ShardsvrCommitDropDatabaseMetadataCommand final
    : public TypedCommand<ShardsvrCommitDropDatabaseMetadataCommand> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command. This command aims to commit a dropDatabase operation to the "
               "shard catalog.";
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    using Request = ShardsvrCommitDropDatabaseMetadata;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::InvalidOptions,
                    fmt::format("{} expected to be called within a retryable write ",
                                Request::kCommandName),
                    TransactionParticipant::get(opCtx));

            const auto dbName = request().getDbName();

            LOGV2(10105902,
                  "About to commit dropDatabase metadata in the shard catalog",
                  "dbName"_attr = dbName);

            {
                // Using the original operation context, the write operations to update the
                // shard catalog would fail since retryable writes are not compatible with
                // applying the WriteUnitOfWork as a transaction (kGroupForTransaction). A tactical
                // solution is to use an alternative client as well as a new operation context.

                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrCommitDropDatabaseMetadata");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                auto newOpCtxPtr = newOpCtx.get();

                shard_catalog_commit::commitDropDatabaseMetadataLocally(newOpCtxPtr, dbName);
            }

            LOGV2(10105903,
                  "Committed dropDatabase metadata in the shard catalog",
                  "dbName"_attr = dbName);

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
                            BSON("_id" << Request::kCommandName),
                            BSON("$inc" << BSON("count" << 1)),
                            true /* upsert */,
                            false /* multi */);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
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

MONGO_REGISTER_COMMAND(ShardsvrCommitDropDatabaseMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
