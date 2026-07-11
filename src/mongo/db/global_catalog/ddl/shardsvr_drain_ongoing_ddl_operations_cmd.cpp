// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/shard_role/ddl/direct_connection_ddl_hook.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"

namespace mongo {
namespace {

class ShardsvrDrainOngoingDDLOperationsCommand
    : public TypedCommand<ShardsvrDrainOngoingDDLOperationsCommand> {
public:
    using Request = ShardsvrDrainOngoingDDLOperations;

    bool skipApiVersionCheck() const override {
        // Internal command (config -> shard).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked by the config server to drain any ongoing DDL operation "
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
            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrDrainOngoingDDLOperations can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            // Kill ongoing multi-doc transactions in case they have already run a DDL operation
            // like create collection but have not yet committed. We do this before waiting for DDLs
            // so that we don't bother waiting on a DDL which is part of a transaction that we will
            // kill anyways.
            killOngoingTransactions(opCtx);
            // Drain any ongoing DDL operations. This should be fast since replica set level DDL
            // operations are generally short.
            waitForOngoingDDLOperationsToComplete(opCtx);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        // The command parameter happens to be string so it's historically been interpreted
        // by parseNs as a collection. Continuing to do so here for unexamined compatibility.
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        void killOngoingTransactions(OperationContext* opCtx) {
            SessionKiller::Matcher matcherAllSessions(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
            killSessionsAbortUnpreparedTransactions(
                opCtx, matcherAllSessions, ErrorCodes::InterruptedDueToAddShard);
        }

        void waitForOngoingDDLOperationsToComplete(OperationContext* opCtx) {
            auto tracker = ReplicaSetDDLTracker::get(opCtx->getServiceContext());
            auto directConnectionDDLhook = static_cast<DirectConnectionDDLHook*>(
                tracker->lookupHookByName(DirectConnectionDDLHook::kDirectConnectionDDLHookName));
            auto futureToWait = directConnectionDDLhook->getWaitForDrainedFuture(opCtx);
            futureToWait.get(opCtx);
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrDrainOngoingDDLOperationsCommand).forShard();
}  // namespace
}  // namespace mongo
