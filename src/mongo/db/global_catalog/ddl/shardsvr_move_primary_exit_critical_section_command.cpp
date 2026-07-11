// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <string>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrMovePrimaryExitCriticalSectionCommand final
    : public TypedCommand<ShardsvrMovePrimaryExitCriticalSectionCommand> {
public:
    using Request = ShardsvrMovePrimaryExitCriticalSection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::InvalidOptions,
                    fmt::format("{} expected to be called within a retryable write ",
                                Request::kCommandName),
                    TransactionParticipant::get(opCtx));

            {
                // Using the original operation context, the write operation to exit the critical
                // section would fail since retryable writes cannot have limit=0. A tactical
                // solution is to use an alternative client as well as a new operation context.
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrMovePrimaryExitCriticalSection");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                const auto& dbName = request().getCommandParameter();
                const auto& csReason = request().getReason();

                // The critical section may have been entered by another operation, which implies
                // that the one with the specified reason has already been exited. Consequently,
                // this skips the requirement that reason must match.
                ShardingRecoveryService::get(newOpCtx.get())
                    ->releaseRecoverableCriticalSection(
                        newOpCtx.get(),
                        NamespaceString(dbName),
                        csReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        ShardingRecoveryService::FilteringMetadataClearer(),
                        false /* throwIfReasonDiffers */);
            }

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

private:
    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool supportsRetryableWrite() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrMovePrimaryExitCriticalSectionCommand).forShard();

}  // namespace
}  // namespace mongo
