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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"

namespace mongo {
namespace {

class CollectionShardingStateFactoryShard final : public CollectionShardingStateFactory {
public:
    CollectionShardingStateFactoryShard(ServiceContext* serviceContext)
        : CollectionShardingStateFactory(serviceContext) {}

    ~CollectionShardingStateFactoryShard() {
        if (_taskExecutor) {
            _taskExecutor->shutdown();
            _taskExecutor->join();
        }
    }

    std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) override {
        return std::make_unique<CollectionShardingRuntime>(_serviceContext, nss, _getExecutor());
    }

private:
    executor::TaskExecutor* _getExecutor() {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        if (!_taskExecutor) {
            const std::string kExecName("CollectionRangeDeleter-TaskExecutor");

            auto net = executor::makeNetworkInterface(kExecName);
            auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
            auto taskExecutor =
                std::make_unique<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
            taskExecutor->startup();

            _taskExecutor = std::move(taskExecutor);
        }

        return _taskExecutor.get();
    }

    // Serializes the instantiation of the task executor
    stdx::mutex _mutex;
    std::unique_ptr<executor::TaskExecutor> _taskExecutor{nullptr};
};

}  // namespace

ServiceContext::ConstructorActionRegisterer collectionShardingStateFactoryRegisterer{
    "CollectionShardingStateFactory",
    [](ServiceContext* service) {
        CollectionShardingStateFactory::set(
            service, std::make_unique<CollectionShardingStateFactoryShard>(service));
    },
    [](ServiceContext* service) { CollectionShardingStateFactory::clear(service); }};

}  // namespace mongo
