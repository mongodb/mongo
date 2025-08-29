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

#include "mongo/db/rss/attached_storage/attached_service_lifecycle.h"

#include "mongo/db/admission/flow_control.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

namespace mongo::rss {
namespace {
// Checkpoint every 60 seconds by default.
constexpr double kDefaultAttachedSyncDelaySeconds = 60.0;

ServiceContext::ConstructorActionRegisterer registerAttachedServiceLifecycle{
    "AttachedServiceLifecycle", [](ServiceContext* service) {
        auto& rss = ReplicatedStorageService::get(service);
        rss.setServiceLifecycle(std::make_unique<AttachedServiceLifecycle>());
    }};

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = "ReplCoord-";
    tpOptions.poolName = "ReplCoordThreadPool";
    tpOptions.maxThreads = 50;
    tpOptions.onCreateThread = [serviceContext](const std::string& threadName) {
        Client::initThread(threadName,
                           serviceContext->getService(ClusterRole::ShardServer),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{false});
    };
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(serviceContext));
    return executor::ThreadPoolTaskExecutor::create(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface("ReplNetwork", nullptr, std::move(hookList)));
}
}  // namespace

AttachedServiceLifecycle::AttachedServiceLifecycle()
    : _initializedUsingDefaultSyncDelay{[]() {
          if (storageGlobalParams.syncdelay.load() < 0.0) {
              storageGlobalParams.syncdelay.store(kDefaultAttachedSyncDelaySeconds);
              return true;
          }  // namespace mongo::rss
          return false;
      }()} {}

void AttachedServiceLifecycle::initializeFlowControl(ServiceContext* svcCtx) {
    FlowControl::set(
        svcCtx, std::make_unique<FlowControl>(svcCtx, repl::ReplicationCoordinator::get(svcCtx)));
}

void AttachedServiceLifecycle::initializeStorageEngineExtensions(ServiceContext*) {}

std::unique_ptr<repl::ReplicationCoordinator>
AttachedServiceLifecycle::initializeReplicationCoordinator(ServiceContext* svcCtx) {
    auto storageInterface = repl::StorageInterface::get(svcCtx);
    auto replicationProcess = repl::ReplicationProcess::get(svcCtx);

    repl::TopologyCoordinator::Options topoCoordOptions;
    topoCoordOptions.maxSyncSourceLagSecs = Seconds(repl::maxSyncSourceLagSecs);
    topoCoordOptions.clusterRole = serverGlobalParams.clusterRole;

    return std::make_unique<repl::ReplicationCoordinatorImpl>(
        svcCtx,
        getGlobalReplSettings(),
        std::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            svcCtx, storageInterface, replicationProcess),
        makeReplicationExecutor(svcCtx),
        std::make_unique<repl::TopologyCoordinator>(topoCoordOptions),
        replicationProcess,
        storageInterface,
        SecureRandom().nextInt64());
}

void AttachedServiceLifecycle::initializeStateRequiredForStorageAccess(ServiceContext*) {}

void AttachedServiceLifecycle::shutdownStateRequiredForStorageAccess(ServiceContext*,
                                                                     BSONObjBuilder*) {}

bool AttachedServiceLifecycle::initializedUsingDefaultSyncDelay() const {
    return _initializedUsingDefaultSyncDelay;
}

bool AttachedServiceLifecycle::shouldKeepThreadAliveUntilStorageEngineHasShutDown(
    const StringData) const {
    return false;
}

}  // namespace mongo::rss
