/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_initialization.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"

namespace mongo {

namespace {

using executor::NetworkInterface;
using executor::ThreadPoolTaskExecutor;

std::unique_ptr<ThreadPoolTaskExecutor> makeTaskExecutor(std::unique_ptr<NetworkInterface> net) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "ShardWork";
    return stdx::make_unique<ThreadPoolTaskExecutor>(stdx::make_unique<ThreadPool>(tpOptions),
                                                     std::move(net));
}

}  // namespace

Status initializeGlobalShardingState(const ConnectionString& configCS) {
    auto network = executor::makeNetworkInterface();
    auto networkPtr = network.get();
    auto shardRegistry(
        stdx::make_unique<ShardRegistry>(stdx::make_unique<RemoteCommandTargeterFactoryImpl>(),
                                         makeTaskExecutor(std::move(network)),
                                         networkPtr,
                                         configCS));

    std::unique_ptr<CatalogManager> catalogManager;
    if (configCS.type() == ConnectionString::SET) {
        auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>(
            shardRegistry.get(), ReplSetDistLockManager::kDistLockWriteConcernTimeout);

        const std::string distLockProcessId = str::stream()
            << getHostName() << ":" << serverGlobalParams.port << ":" << time(0) << ":" << rand();
        auto distLockManager = stdx::make_unique<ReplSetDistLockManager>(
            getGlobalServiceContext(),
            distLockProcessId,
            std::move(distLockCatalog),
            ReplSetDistLockManager::kDistLockPingInterval,
            ReplSetDistLockManager::kDistLockExpirationTime);

        auto catalogManagerReplicaSet =
            stdx::make_unique<CatalogManagerReplicaSet>(std::move(distLockManager));

        catalogManager = std::move(catalogManagerReplicaSet);
    } else {
        auto catalogManagerLegacy = stdx::make_unique<CatalogManagerLegacy>();
        Status status = catalogManagerLegacy->init(configCS);
        if (!status.isOK()) {
            return status;
        }

        catalogManager = std::move(catalogManagerLegacy);
    }

    shardRegistry->startup();
    grid.init(std::move(catalogManager),
              std::move(shardRegistry),
              stdx::make_unique<ClusterCursorManager>(getGlobalServiceContext()->getClockSource()));

    return Status::OK();
}

}  // namespace mongo
