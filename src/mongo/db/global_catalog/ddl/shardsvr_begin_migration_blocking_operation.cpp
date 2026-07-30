// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_v2.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_gen.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterFetchingMigrationBlockingOperationCoordinator);
MONGO_FAIL_POINT_DEFINE(hangAfterCatchingCleanupError);

class ShardsvrBeginMigrationBlockingOperationCommand final
    : public TypedCommand<ShardsvrBeginMigrationBlockingOperationCommand> {
public:
    using Request = ShardsvrBeginMigrationBlockingOperation;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
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

            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            const auto& operationId = request().getOperationId();

            if (request().getAuthoritative()) {
                // The V2 coordinator may be tearing down (unblocking migrations) when we enqueue
                // our request. In that case it responds with
                // MigrationBlockingOperationCoordinatorCleaningUp and we retry against a freshly
                // created instance until the operation is blocked.
                while (true) {
                    auto coordinator =
                        MigrationBlockingOperationCoordinatorV2::getOrCreate(opCtx, ns());
                    hangAfterFetchingMigrationBlockingOperationCoordinator.pauseWhileSet();

                    auto future = coordinator->beginOperation(opCtx, operationId);
                    try {
                        future.get(opCtx);
                        return;
                    } catch (const ExceptionFor<
                             ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp>&) {
                        hangAfterCatchingCleanupError.pauseWhileSet();
                        coordinator->getCompletionFuture().wait(opCtx);
                    }
                }
            }

            auto coordinator = MigrationBlockingOperationCoordinator::getOrCreate(opCtx, ns());
            hangAfterFetchingMigrationBlockingOperationCoordinator.pauseWhileSet();

            try {
                coordinator->beginOperation(opCtx, operationId);
            } catch (
                const ExceptionFor<ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp>&) {
                hangAfterCatchingCleanupError.pauseWhileSet();
                coordinator->getCompletionFuture().wait(opCtx);

                coordinator = MigrationBlockingOperationCoordinator::getOrCreate(opCtx, ns());
                coordinator->beginOperation(opCtx, operationId);
            }
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
MONGO_REGISTER_COMMAND(ShardsvrBeginMigrationBlockingOperationCommand).forShard();

}  // namespace
}  // namespace mongo
