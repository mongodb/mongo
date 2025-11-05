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


#include "mongo/db/query/search/search_task_executors.h"

#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_index_options.h"
#include "mongo/executor/connection_pool_controllers.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/pinned_connection_task_executor_registry.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

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
    State() {
        constexpr StringData kMongotExecutorName = "MongotExecutor";
        constexpr StringData kSearchIndexManagementExecutorName = "SearchIndexMgmtExecutor";

        std::unique_ptr<NetworkInterface> mongotExecutorNetworkInterface;
        std::unique_ptr<NetworkInterface> searchIdxNetworkInterface;

        if (globalMongotParams.useGRPC) {
#ifdef MONGO_CONFIG_GRPC
            mongotExecutorNetworkInterface = makeNetworkInterfaceGRPC(kMongotExecutorName);
            searchIdxNetworkInterface =
                makeNetworkInterfaceGRPC(kSearchIndexManagementExecutorName);
#else
            MONGO_UNREACHABLE;
#endif
        } else {
            mongotExecutorNetworkInterface = makeNetworkInterface(
                kMongotExecutorName, nullptr, nullptr, makeMongotConnPoolOptions());

            // Make a separate search index management NetworkInterface that's independently
            // configurable.
            ConnectionPool::Options searchIndexPoolOptions;
            searchIndexPoolOptions.skipAuthentication =
                globalSearchIndexParams.skipAuthToSearchIndexServer;
            searchIdxNetworkInterface = makeNetworkInterface(kSearchIndexManagementExecutorName,
                                                             nullptr,
                                                             nullptr,
                                                             std::move(searchIndexPoolOptions));
        }

        auto mongotThreadPool =
            std::make_unique<NetworkInterfaceThreadPool>(mongotExecutorNetworkInterface.get());
        mongotExecutor = ThreadPoolTaskExecutor::create(std::move(mongotThreadPool),
                                                        std::move(mongotExecutorNetworkInterface));

        auto searchIndexThreadPool =
            std::make_unique<NetworkInterfaceThreadPool>(searchIdxNetworkInterface.get());
        searchIndexMgmtExecutor = ThreadPoolTaskExecutor::create(
            std::move(searchIndexThreadPool), std::move(searchIdxNetworkInterface));
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

Rarely _shutdownLogSampler;

void destroyTaskExecutor(std::shared_ptr<TaskExecutor>& executor) {
    // We want to make sure that the TaskExecutor gets destructed here so that it frees all its
    // resources (e.g. GRPC clients) before continuing. However, any in progress search operations
    // also keep a shared_ptr reference to the object, so resetting the global pointer isn't enough
    // to guarantee that it is destructed. We create a weak pointer here and wait for it to be
    // expired so that we know that all references are gone.
    std::weak_ptr<executor::TaskExecutor> weakReference = executor;
    executor.reset();

    // Tick the log sampler in advance to avoid logging immediately - only log if it's a long wait.
    _shutdownLogSampler.tick();

    while (!weakReference.expired()) {
        if (_shutdownLogSampler.tick()) {
            LOGV2_INFO(11323800, "Waiting for search task executor to be destroyed.");
        }
        sleepFor(Milliseconds(100));
    }
}

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

void shutdownSearchExecutorsIfNeeded(ServiceContext* svc) {
    auto& state = getExecutorHolder(svc);
    if (!globalMongotParams.host.empty()) {
        LOGV2_INFO(10026102, "Shutting down mongot task executor.");
        // The underlying mongot TaskExecutor must outlive any PinnedConnectionTaskExecutor that
        // uses it, so we must drain PCTEs first and then shut down the mongot executor.
        shutdownPinnedExecutors(svc, state.mongotExecutor);
        state.mongotExecutor->shutdown();
        state.mongotExecutor->join();

        destroyTaskExecutor(state.mongotExecutor);
        LOGV2_INFO(11323801, "Finished shutting down mongot task executor.");
    }

    if (!globalSearchIndexParams.host.empty()) {
        LOGV2_INFO(10026103, "Shutting down search index management task executor.");
        shutdownPinnedExecutors(svc, state.searchIndexMgmtExecutor);
        state.searchIndexMgmtExecutor->shutdown();
        state.searchIndexMgmtExecutor->join();

        destroyTaskExecutor(state.searchIndexMgmtExecutor);
        LOGV2_INFO(11323802, "Finished shutting down search index management task executor.");
    }
}

}  // namespace executor
}  // namespace mongo
