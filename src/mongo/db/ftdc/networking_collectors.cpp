/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/ftdc/networking_collectors.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {
namespace {

class ConnPoolStatsCollector : public FTDCCollectorInterface {
public:
    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        executor::ConnectionPoolStats stats{};

        // Global connection pool connections.
        globalConnPool.appendConnectionStats(&stats);

        // Sharding connections.
        {
            auto const grid = Grid::get(opCtx);
            if (grid->isInitialized()) {
                grid->getExecutorPool()->appendConnectionStats(&stats);

                auto const customConnPoolStatsFn = grid->getCustomConnectionPoolStatsFn();
                if (customConnPoolStatsFn) {
                    customConnPoolStatsFn(&stats);
                }
            }
        }

        // Output to a BSON object.
        builder.appendNumber("numClientConnections", DBClientConnection::getNumConnections());
        builder.appendNumber("numAScopedConnections", AScopedConnection::getNumConnections());
        stats.appendToBSON(builder, true /* forFTDC */);

        // All replica sets being tracked.
        ReplicaSetMonitorManager::get()->report(&builder, true /* forFTDC */);
    }

    std::string name() const override {
        return "connPoolStats";
    }
};

class NetworkInterfaceStatsCollector final : public FTDCCollectorInterface {
public:
    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        auto const grid = Grid::get(opCtx);
        auto executor = ReplicaSetMonitorManager::get()->getExecutor();
        if (grid->isInitialized()) {
            grid->getExecutorPool()->appendNetworkInterfaceStats(builder);
        }

        if (executor) {
            executor->appendNetworkInterfaceStats(builder);
        }
    }

    std::string name() const override {
        return "networkInterfaceStats";
    }
};
}  // namespace

void registerNetworkingCollectors(FTDCController* controller) {
    controller->addPeriodicCollector(std::make_unique<ConnPoolStatsCollector>());

    controller->addPeriodicCollector(std::make_unique<NetworkInterfaceStatsCollector>());
}
}  // namespace mongo
