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

#include "mongo/executor/connection_pool_controllers.h"
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
    mongotOptions.singleUseConnections = globalMongotParams.useGRPC;
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
    State() {
        transport::TransportProtocol protocol = globalMongotParams.useGRPC
            ? transport::TransportProtocol::GRPC
            : transport::TransportProtocol::MongoRPC;

        // Make the MongotExecutor and associated NetworkInterface.
        auto mongotExecutorNetworkInterface = makeNetworkInterface(
            "MongotExecutor", nullptr, nullptr, makeMongotConnPoolOptions(), protocol);
        auto mongotThreadPool =
            std::make_unique<NetworkInterfaceThreadPool>(mongotExecutorNetworkInterface.get());
        mongotExecutor = ThreadPoolTaskExecutor::create(std::move(mongotThreadPool),
                                                        std::move(mongotExecutorNetworkInterface));

        // Make a separate searchIndexMgmtExecutor that's independently configurable.
        ConnectionPool::Options searchIndexPoolOptions;
        searchIndexPoolOptions.singleUseConnections = globalMongotParams.useGRPC;
        searchIndexPoolOptions.skipAuthentication =
            globalSearchIndexParams.skipAuthToSearchIndexServer;
        std::shared_ptr<NetworkInterface> searchIdxNI =
            makeNetworkInterface("SearchIndexMgmtExecutor",
                                 nullptr,
                                 nullptr,
                                 std::move(searchIndexPoolOptions),
                                 protocol);
        auto searchIndexThreadPool =
            std::make_unique<NetworkInterfaceThreadPool>(searchIdxNI.get());
        searchIndexMgmtExecutor = ThreadPoolTaskExecutor::create(std::move(searchIndexThreadPool),
                                                                 std::move(searchIdxNI));
    }

    auto getMongotExecutorPtr() {
        invariant(mongotExecutor);
        return mongotExecutor;
    }

    auto getSearchIndexMgmtExecutorPtr() {
        invariant(searchIndexMgmtExecutor);
        return searchIndexMgmtExecutor;
    }

    std::shared_ptr<TaskExecutor> mongotExecutor;
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

void startupSearchExecutorsIfNeeded(ServiceContext* svc) {
    if (!globalMongotParams.host.empty()) {
        LOGV2_INFO(8267400, "Starting up mongot task executor.");
        getMongotTaskExecutor(svc)->startup();
    }

    if (!globalSearchIndexParams.host.empty()) {
        LOGV2_INFO(8267401, "Starting up search index management task executor.");
        getSearchIndexManagementTaskExecutor(svc)->startup();
    }
}

}  // namespace executor
}  // namespace mongo
