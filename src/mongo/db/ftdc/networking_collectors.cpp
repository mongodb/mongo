// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/networking_collectors.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

void appendDiagnosticInfo(BSONObjBuilder& builder,
                          std::string_view subsectionName,
                          const std::shared_ptr<executor::TaskExecutor>& taskExecutor) {

    BSONObjBuilder subSectionBuilder = builder.subobjStart(subsectionName);
    {
        BSONObjBuilder substats = subSectionBuilder.subobjStart("diagnosticInfo");
        taskExecutor->appendDiagnosticBSON(&substats);
        substats.doneFast();
    }
    subSectionBuilder.doneFast();
}

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

        // Add Search diagnostics.
        if (auto swExec = executor::getMongotTaskExecutor(opCtx->getServiceContext());
            swExec.isOK()) {
            appendDiagnosticInfo(builder, "mongot"sv, swExec.getValue());
        }

        if (auto swExec =
                executor::getSearchIndexManagementTaskExecutor(opCtx->getServiceContext());
            swExec.isOK()) {
            appendDiagnosticInfo(builder, "searchIndex"sv, swExec.getValue());
        }
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
