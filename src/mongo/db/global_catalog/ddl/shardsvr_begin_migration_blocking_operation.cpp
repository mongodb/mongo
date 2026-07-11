// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_gen.h"
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
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            auto coordinator = MigrationBlockingOperationCoordinator::getOrCreate(opCtx, ns());
            hangAfterFetchingMigrationBlockingOperationCoordinator.pauseWhileSet();

            const auto& operationId = request().getOperationId();
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
