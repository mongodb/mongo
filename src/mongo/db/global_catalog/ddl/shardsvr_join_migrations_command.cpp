// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_migrations_request_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>


namespace mongo {
namespace {
class ShardsvrJoinMigrationsCommand final : public TypedCommand<ShardsvrJoinMigrationsCommand> {
public:
    using Request = ShardsvrJoinMigrations;

    bool skipApiVersionCheck() const override {
        // Internal command (config -> shard).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked by the config server to join any chunk migration activity "
               "executed by the shard";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            {
                Lock::GlobalLock lk(opCtx, MODE_IX);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Not primary while trying to join chunk migration",
                        repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary());
            }

            auto& activeMigrationRegistry = ActiveMigrationsRegistry::get(opCtx);
            activeMigrationRegistry.lock(opCtx, kRegistryLockReason);
            activeMigrationRegistry.unlock(kRegistryLockReason);
        }

    private:
        static constexpr char kRegistryLockReason[] = "Running _shardsvrJoinMigrations";

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
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
MONGO_REGISTER_COMMAND(ShardsvrJoinMigrationsCommand).forShard();

}  // namespace
}  // namespace mongo
