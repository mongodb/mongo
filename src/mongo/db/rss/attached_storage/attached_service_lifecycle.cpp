// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <string_view>

namespace mongo::rss {
namespace {

ServiceContext::ConstructorActionRegisterer registerAttachedServiceLifecycle{
    "AttachedServiceLifecycle", [](ServiceContext* service) {
        auto& rss = ReplicatedStorageService::get(service);
        rss.setServiceLifecycle(std::make_unique<AttachedServiceLifecycle>());
    }};

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(serviceContext));
    return executor::ThreadPoolTaskExecutor::create(
        ThreadPool::make({
            .poolName = "ReplCoordThreadPool",
            .threadNamePrefix = "ReplCoord-",
            .maxThreads = 50,
            .onCreateThread =
                [serviceContext](const std::string& threadName) {
                    Client::initThread(threadName,
                                       serviceContext->getService(),
                                       Client::noSession(),
                                       ClientOperationKillableByStepdown{false});
                },
        }),
        executor::makeNetworkInterface("ReplNetwork", nullptr, std::move(hookList)));
}
}  // namespace

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

void AttachedServiceLifecycle::initializeStateRequiredForOfflineValidation(OperationContext*) {}

bool AttachedServiceLifecycle::shouldKeepThreadAliveUntilStorageEngineHasShutDown(
    const std::string_view) const {
    return false;
}

}  // namespace mongo::rss
