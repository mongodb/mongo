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

class ShardsvrEndMigrationBlockingOperationCommand final
    : public TypedCommand<ShardsvrEndMigrationBlockingOperationCommand> {
public:
    using Request = ShardsvrEndMigrationBlockingOperation;

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
                auto maybeCoordinator = MigrationBlockingOperationCoordinatorV2::get(opCtx, ns());
                if (!maybeCoordinator) {
                    // No coordinator means migrations are not blocked for this namespace, so there
                    // is nothing to end.
                    return;
                }

                auto future = maybeCoordinator.get()->endOperation(opCtx, operationId);
                try {
                    future.get(opCtx);
                } catch (const ExceptionFor<
                         ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp>&) {
                    // The coordinator is already unblocking migrations and tearing down, so this
                    // operation is no longer blocking anything.
                }
                return;
            }

            auto maybeCoordinator = MigrationBlockingOperationCoordinator::get(opCtx, ns());
            if (!maybeCoordinator) {
                return;
            }
            maybeCoordinator.get()->endOperation(opCtx, operationId);
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
MONGO_REGISTER_COMMAND(ShardsvrEndMigrationBlockingOperationCommand).forShard();

}  // namespace
}  // namespace mongo
