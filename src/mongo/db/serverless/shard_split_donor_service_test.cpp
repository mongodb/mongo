/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <memory>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/streamable_replica_set_monitor_for_testing.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"


namespace mongo {

namespace {
constexpr std::int32_t stopFailPointErrorCode = 9822402;

sdam::TopologyDescriptionPtr createTopologyDescription(const MockReplicaSet& set) {
    std::shared_ptr<TopologyDescription> topologyDescription =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration(
            set.getHosts(), sdam::TopologyType::kReplicaSetNoPrimary, set.getSetName()));

    for (auto& server : set.getHosts()) {
        auto serverDescription = sdam::ServerDescriptionBuilder()
                                     .withAddress(server)
                                     .withSetName(set.getSetName())
                                     .instance();
        topologyDescription->installServerDescription(serverDescription);
    }

    return topologyDescription;
}

}  // namespace

std::ostringstream& operator<<(std::ostringstream& builder,
                               const mongo::ShardSplitDonorStateEnum state) {
    switch (state) {
        case mongo::ShardSplitDonorStateEnum::kUninitialized:
            builder << "kUninitialized";
            break;
        case mongo::ShardSplitDonorStateEnum::kAborted:
            builder << "kAborted";
            break;
        case mongo::ShardSplitDonorStateEnum::kBlocking:
            builder << "kBlocking";
            break;
        case mongo::ShardSplitDonorStateEnum::kCommitted:
            builder << "kCommitted";
            break;
        case mongo::ShardSplitDonorStateEnum::kDataSync:
            builder << "kDataSync";
            break;
    }

    return builder;
}

class ShardSplitDonorServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();

        // Fake replSet just for creating consistent URI for monitor
        _rsmMonitor.setup(_validRepl.getURI());

        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        // Set up clocks.
        serviceContext->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(_clkSource));
        serviceContext->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));

        WaitForMajorityService::get(serviceContext).startup(serviceContext);

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
            repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());
            {
                Lock::GlobalWrite lk(opCtx.get());
                OldClientContext ctx(opCtx.get(), NamespaceString::kRsOplogNamespace.ns());
                tenant_migration_util::createOplogViewForTenantMigrations(opCtx.get(), ctx.db());
            }

            // Need real (non-mock) storage for the oplog buffer.
            repl::StorageInterface::set(serviceContext,
                                        std::make_unique<repl::StorageInterfaceImpl>());

            // The DropPendingCollectionReaper is required to drop the oplog buffer collection.
            repl::DropPendingCollectionReaper::set(
                serviceContext,
                std::make_unique<repl::DropPendingCollectionReaper>(
                    repl::StorageInterface::get(serviceContext)));

            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::PrimaryOnlyServiceOpObserver>(serviceContext));

            _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
            std::unique_ptr<ShardSplitDonorService> service =
                std::make_unique<ShardSplitDonorService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
        }
        stepUp();

        _service = _registry->lookupServiceByName(ShardSplitDonorService::kServiceName);
        ASSERT(_service);

        // Timestamps of "0 seconds" are not allowed, so we must advance our clock mock to the first
        // real second.
        _clkSource->advance(Milliseconds(1000));
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        _registry->onShutdown();
        _service = nullptr;

        repl::StorageInterface::set(getServiceContext(), {});

        ServiceContextMongoDTest::tearDown();
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            repl::OpTimeAndWallTime(repl::OpTime(Timestamp(1, 1), _term), Date_t()));

        _registry->onStepUpComplete(opCtx.get(), _term);
    }

    void stepDown() {
        ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_SECONDARY));
        _registry->onStepDown();
    }

protected:
    repl::PrimaryOnlyServiceRegistry* _registry;
    repl::PrimaryOnlyService* _service;
    MockReplicaSet _validRepl{
        "replInScope", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    long long _term = 0;

private:
    std::shared_ptr<ClockSourceMock> _clkSource = std::make_shared<ClockSourceMock>();
    StreamableReplicaSetMonitorForTesting _rsmMonitor;
};

TEST_F(ShardSplitDonorServiceTest, BasicShardSplitDonorServiceInstanceCreation) {
    const UUID migrationUUID = UUID::gen();

    ShardSplitDonorDocument initialStateDocument(migrationUUID);

    initialStateDocument.setRecipientConnectionString(StringData(_validRepl.getConnectionString()));
    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(migrationUUID, serviceInstance->getId());

    auto completionFuture = serviceInstance->completionFuture();

    std::shared_ptr<TopologyDescription> topologyDescriptionOld =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration());
    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        createTopologyDescription(_validRepl);

    // construct task executor
    std::shared_ptr<executor::NetworkInterface> networkInterface = executor::makeNetworkInterface(
        "SplitReplicaSetObserter-TestExecutor",
        std::make_unique<ReplicaSetMonitorManagerNetworkConnectionHook>(),
        std::make_unique<rpc::EgressMetadataHookList>());

    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(networkInterface.get());
    auto executor =
        std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), networkInterface);
    executor->startup();

    // Wait until the RSM has been created by the instance.
    auto replicaSetMonitorCreatedFuture = serviceInstance->replicaSetMonitorCreatedFuture();
    replicaSetMonitorCreatedFuture.wait(opCtx.get());

    // Retrieve monitor installed by _rsmMonitor.setup(...)
    auto monitor = std::dynamic_pointer_cast<StreamableReplicaSetMonitor>(
        ReplicaSetMonitor::createIfNeeded(_validRepl.getURI()));
    invariant(monitor);
    auto publisher = monitor->getEventsPublisher();

    publisher->onTopologyDescriptionChangedEvent(topologyDescriptionOld, topologyDescriptionNew);

    completionFuture.wait();

    auto result = completionFuture.get();
    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);
}

TEST_F(ShardSplitDonorServiceTest, Abort) {
    const UUID migrationUUID = UUID::gen();
    ShardSplitDonorDocument initialStateDocument(migrationUUID);
    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance;

    {
        FailPointEnableBlock fp("pauseShardSplitAfterInitialSync");
        auto initialTimesEntered = fp.initialTimesEntered();

        serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        serviceInstance->tryAbort();
    }

    auto result = serviceInstance->completionFuture().get(opCtx.get());

    ASSERT(!!result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::TenantMigrationAborted);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);
}

TEST_F(ShardSplitDonorServiceTest, StepDownTest) {
    const UUID migrationUUID = UUID::gen();
    ShardSplitDonorDocument initialStateDocument(migrationUUID);
    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance;

    {
        FailPointEnableBlock fp("pauseShardSplitAfterInitialSync");
        auto initialTimesEntered = fp.initialTimesEntered();

        serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        stepDown();
    }

    auto result = serviceInstance->completionFuture().getNoThrow();
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange, result.getStatus());
}

class SplitReplicaSetObserverTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        _rsmMonitor.setup(_validRepl.getURI());
        _otherRsmMonitor.setup(_invalidRepl.getURI());

        _executor = repl::makeTestExecutor();

        // Retrieve monitor installed by _rsmMonitor.setup(...)
        auto monitor = std::dynamic_pointer_cast<StreamableReplicaSetMonitor>(
            ReplicaSetMonitor::createIfNeeded(_validRepl.getURI()));
        invariant(monitor);
        _publisher = monitor->getEventsPublisher();
    }

protected:
    MockReplicaSet _validRepl{
        "replInScope", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    MockReplicaSet _invalidRepl{
        "replNotInScope", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};

    StreamableReplicaSetMonitorForTesting _rsmMonitor;
    StreamableReplicaSetMonitorForTesting _otherRsmMonitor;
    std::shared_ptr<executor::TaskExecutor> _executor;
    std::shared_ptr<sdam::TopologyEventsPublisher> _publisher;
};

TEST_F(SplitReplicaSetObserverTest, SupportsCancellation) {
    CancellationSource source;
    auto future = getRecipientAcceptSplitFuture(
        _executor, source.token(), MongoURI::parse(_validRepl.getConnectionString()).getValue());

    ASSERT_FALSE(future.isReady());
    source.cancel();

    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::CallbackCanceled);
}

TEST_F(SplitReplicaSetObserverTest, GetRecipientAcceptSplitFutureTest) {
    CancellationSource source;

    auto future = getRecipientAcceptSplitFuture(
        _executor, source.token(), MongoURI::parse(_validRepl.getConnectionString()).getValue());

    std::shared_ptr<TopologyDescription> topologyDescriptionOld =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration());
    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        createTopologyDescription(_validRepl);

    _publisher->onTopologyDescriptionChangedEvent(topologyDescriptionOld, topologyDescriptionNew);

    future.wait();
}

TEST_F(SplitReplicaSetObserverTest, FutureNotReadyMissingNodes) {
    auto uri = MongoURI::parse(_validRepl.getConnectionString()).getValue();

    auto predicate =
        makeRecipientAcceptSplitPredicate(uri.getReplicaSetName(), uri.getServers().size());

    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        createTopologyDescription(_validRepl);
    topologyDescriptionNew->removeServerDescription(_validRepl.getHosts()[0]);

    ASSERT_FALSE(predicate(topologyDescriptionNew->getServers()));
}

TEST_F(SplitReplicaSetObserverTest, FutureNotReadyWrongSet) {
    auto uri = MongoURI::parse(_validRepl.getConnectionString()).getValue();

    auto predicate =
        makeRecipientAcceptSplitPredicate(uri.getReplicaSetName(), uri.getServers().size());

    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        createTopologyDescription(_invalidRepl);

    ASSERT_FALSE(predicate(topologyDescriptionNew->getServers()));
}

TEST_F(SplitReplicaSetObserverTest, ExecutorCanceled) {
    CancellationSource source;

    auto future = getRecipientAcceptSplitFuture(
        _executor, source.token(), MongoURI::parse(_validRepl.getConnectionString()).getValue());

    _executor->shutdown();
    _executor->join();

    ASSERT_FALSE(future.isReady());

    // Ensure the test does not hang.
    source.cancel();
    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::ShutdownInProgress);
}

}  // namespace mongo
