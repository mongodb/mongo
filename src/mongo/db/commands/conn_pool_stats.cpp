/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"

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
        ScopedAdmissionPriority skipAdmissionControl(opCtx, AdmissionContext::Priority::kExempt);

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
