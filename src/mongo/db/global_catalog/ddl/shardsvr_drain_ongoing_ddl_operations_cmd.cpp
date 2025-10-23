/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/local_catalog/ddl/direct_connection_ddl_hook.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_killer.h"

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
