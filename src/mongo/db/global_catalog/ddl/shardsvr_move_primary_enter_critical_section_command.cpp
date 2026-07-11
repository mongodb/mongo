// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
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
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrMovePrimaryEnterCriticalSectionCommand final
    : public TypedCommand<ShardsvrMovePrimaryEnterCriticalSectionCommand> {
public:
    using Request = ShardsvrMovePrimaryEnterCriticalSection;

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
                // Using the original operation context, the two write operations to enter the
                // critical section (acquire and promote) would use the same txnNumber, which would
                // cause the failure of the second operation.
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrMovePrimaryEnterCriticalSection");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                const auto& dbName = request().getCommandParameter();
                const auto& csReason = request().getReason();

                // Waiting for the critical section on the database to complete is necessary to
                // avoid the risk of invariant by attempting to enter the critical section as a
                // dropDatabase operation may have already entered it.
                waitForCriticalSectionToComplete(opCtx, dbName, csReason);

                ShardingRecoveryService::get(newOpCtx.get())
                    ->acquireRecoverableCriticalSectionBlockWrites(
                        newOpCtx.get(),
                        NamespaceString(dbName),
                        csReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        true /* clearShardCatalogCache */);
                ShardingRecoveryService::get(newOpCtx.get())
                    ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                        newOpCtx.get(),
                        NamespaceString(dbName),
                        csReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
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

        void waitForCriticalSectionToComplete(OperationContext* opCtx,
                                              const DatabaseName& dbName,
                                              const BSONObj& movePrimaryReason) {
            auto criticalSectionSignal = [&]() -> boost::optional<CriticalSectionSignal> {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
                const auto scopedDsr =
                    DatabaseShardingRuntime::assertDbLockedAndAcquireShared(opCtx, dbName);

                auto optCritSectionSignalSignal =
                    scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite);
                if (optCritSectionSignalSignal) {
                    auto optCritSectionReason = scopedDsr->getCriticalSectionReason();
                    tassert(7578800, "Found critical section without reason", optCritSectionReason);
                    if (movePrimaryReason.woCompare(*optCritSectionReason) == 0) {
                        return boost::none;
                    }
                }
                return optCritSectionSignalSignal;
            }();

            if (criticalSectionSignal) {
                criticalSectionSignal->get(opCtx);
            }
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
MONGO_REGISTER_COMMAND(ShardsvrMovePrimaryEnterCriticalSectionCommand).forShard();

}  // namespace
}  // namespace mongo
