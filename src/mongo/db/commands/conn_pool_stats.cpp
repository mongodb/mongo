// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor_pool.h"

#include <functional>
#include <string>

namespace mongo {
namespace {

class PoolStats final : public BasicCommand {
public:
    PoolStats() : BasicCommand("connPoolStats") {}

    std::string help() const override {
        return "stats about connections between servers in a replica set or sharded cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                ActionType::connPoolStats)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const mongo::BSONObj& cmdObj,
             mongo::BSONObjBuilder& result) override {
        // Critical to observability and diagnosability, categorize as immediate priority.
        ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
            opCtx, AdmissionContext::Priority::kExempt);

        executor::ConnectionPoolStats stats{};

        // Global connection pool connections.
        globalConnPool.appendConnectionStats(&stats);
        result.appendNumber("numClientConnections", DBClientConnection::getNumConnections());
        result.appendNumber("numAScopedConnections", AScopedConnection::getNumConnections());

        // Replication connections, if we have any
        {
            auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
            if (replCoord && replCoord->getSettings().isReplSet()) {
                replCoord->appendConnectionStats(&stats);
            }
        }

        // Sharding connections, if we have any
        {
            auto const grid = Grid::get(opCtx);
            if (grid->isInitialized()) {
                if (grid->getExecutorPool()) {
                    grid->getExecutorPool()->appendConnectionStats(&stats);
                }

                auto const customConnPoolStatsFn = grid->getCustomConnectionPoolStatsFn();
                if (customConnPoolStatsFn) {
                    customConnPoolStatsFn(&stats);
                }
            }
        }

        // Output to a BSON object.
        stats.appendToBSON(result);

        // Always report all replica sets being tracked.
        ReplicaSetMonitorManager::get()->report(&result);

        return true;
    }
};
MONGO_REGISTER_COMMAND(PoolStats).forRouter().forShard();

}  // namespace
}  // namespace mongo
