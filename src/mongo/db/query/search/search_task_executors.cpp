/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "search_task_executors.h"

#include <utility>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/connection_pool_controllers.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

#include "mongot_options.h"
#include "search_index_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace executor {

namespace {

ConnectionPool::Options makeMongotConnPoolOptions() {
    ConnectionPool::Options mongotOptions;
    mongotOptions.skipAuthentication = globalMongotParams.skipAuthToMongot;
    mongotOptions.controllerFactory = [] {
        return std::make_shared<DynamicLimitController>(
            [] { return globalMongotParams.minConnections.load(); },
            [] { return globalMongotParams.maxConnections.load(); },
            "MongotDynamicLimitController");
    };
    return mongotOptions;
}

struct State {
    State() : mongotExecStarted(false), searchIndexMgmtExecStarted(false) {
        // Make the MongotExecutor and associated NetworkInterface.
        auto mongotExecutorNetworkInterface =
            makeNetworkInterface("MongotExecutor", nullptr, nullptr, makeMongotConnPoolOptions());
        auto mongotThreadPool =
            std::make_unique<NetworkInterfaceThreadPool>(mongotExecutorNetworkInterface.get());
        mongotExecutor = std::make_shared<ThreadPoolTaskExecutor>(
            std::move(mongotThreadPool), std::move(mongotExecutorNetworkInterface));

        // Make a separate searchIndexMgmtExecutor that's independently configurable.
        ConnectionPool::Options searchIndexPoolOptions;
        searchIndexPoolOptions.skipAuthentication =
            globalSearchIndexParams.skipAuthToSearchIndexServer;
        std::shared_ptr<NetworkInterface> searchIdxNI = makeNetworkInterface(
            "SearchIndexMgmtExecutor", nullptr, nullptr, std::move(searchIndexPoolOptions));
        auto searchIndexThreadPool =
            std::make_unique<NetworkInterfaceThreadPool>(searchIdxNI.get());
        searchIndexMgmtExecutor = std::make_shared<ThreadPoolTaskExecutor>(
            std::move(searchIndexThreadPool), std::move(searchIdxNI));
    }

    auto getMongotExecutorPtr() {
        invariant(mongotExecutor);

        if (!mongotExecStarted.load() && !mongotExecStarted.swap(true)) {
            mongotExecutor->startup();
        }
        return mongotExecutor;
    }

    auto getSearchIndexMgmtExecutorPtr() {
        invariant(searchIndexMgmtExecutor);

        if (!searchIndexMgmtExecStarted.load() && !searchIndexMgmtExecStarted.swap(true)) {
            searchIndexMgmtExecutor->startup();
        }
        return searchIndexMgmtExecutor;
    }

    AtomicWord<bool> mongotExecStarted;
    std::shared_ptr<TaskExecutor> mongotExecutor;

    AtomicWord<bool> searchIndexMgmtExecStarted;
    std::shared_ptr<TaskExecutor> searchIndexMgmtExecutor;
};

const auto getExecutorHolder = ServiceContext::declareDecoration<State>();

ServiceContext::ConstructorActionRegisterer searchExecutorsCAR{
    "SearchTaskExecutors",
    [](ServiceContext* service) {},
    [](ServiceContext* service) {
        // Destruction implicitly performs the needed shutdown and join()
        getExecutorHolder(service).mongotExecutor.reset();
        getExecutorHolder(service).searchIndexMgmtExecutor.reset();
    }};

// TODO (SERVER-126178): Remove this server status section and add to connPoolStats.
class SearchTaskExecutorServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder bob;
        auto* svc = opCtx->getServiceContext();

        auto appendStats = [&](StringData key,
                               const StatusWith<std::shared_ptr<TaskExecutor>>& sw) {
            if (!sw.isOK())
                return;
            auto& exec = sw.getValue();
            BSONObjBuilder sub = bob.subobjStart(key);
            {
                BSONObjBuilder diagnosticInfo = sub.subobjStart("diagnosticInfo");
                exec->appendDiagnosticBSON(&diagnosticInfo);
            }
            {
                BSONObjBuilder networkInterface = sub.subobjStart("networkInterface");
                exec->appendNetworkInterfaceStats(networkInterface);
            }
            {
                BSONObjBuilder connectionPool = sub.subobjStart("connectionPool");
                executor::ConnectionPoolStats poolStats{};
                exec->appendConnectionStats(&poolStats);
                poolStats.appendToBSON(connectionPool);
            }
        };

        appendStats("mongot", getMongotTaskExecutor(svc));
        appendStats("searchIndex", getSearchIndexManagementTaskExecutor(svc));
        return bob.obj();
    }
};

const auto& searchTaskExecutorSection =
    *ServerStatusSectionBuilder<SearchTaskExecutorServerStatusSection>("searchTaskExecutorMetrics");

}  // namespace

std::shared_ptr<TaskExecutor> getMongotTaskExecutor(ServiceContext* svc) {
    auto& state = getExecutorHolder(svc);
    invariant(state.mongotExecutor);
    return state.getMongotExecutorPtr();
}

std::shared_ptr<TaskExecutor> getSearchIndexManagementTaskExecutor(ServiceContext* svc) {
    auto& state = getExecutorHolder(svc);
    invariant(state.searchIndexMgmtExecutor);
    return state.getSearchIndexMgmtExecutorPtr();
}

}  // namespace executor
}  // namespace mongo
