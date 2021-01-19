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

#include <boost/optional/optional_io.hpp>
#include <fstream>
#include <memory>

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_fetcher_mock.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/ssl_util.h"

namespace mongo {
namespace repl {

namespace {
constexpr std::int32_t stopFailPointErrorCode = 4880402;

OplogEntry makeOplogEntry(OpTime opTime,
                          OpTypeEnum opType,
                          NamespaceString nss,
                          OptionalCollectionUUID uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2) {
    return {DurableOplogEntry(opTime,                     // optime
                              boost::none,                // hash
                              opType,                     // opType
                              nss,                        // namespace
                              uuid,                       // uuid
                              boost::none,                // fromMigrate
                              OplogEntry::kOplogVersion,  // version
                              o,                          // o
                              o2,                         // o2
                              {},                         // sessionInfo
                              boost::none,                // upsert
                              Date_t(),                   // wall clock time
                              boost::none,                // statement id
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none)};  // _id
}

/**
 * Generates a listDatabases response for an TenantAllDatabaseCloner to consume.
 */
BSONObj makeListDatabasesResponse(std::vector<std::string> databaseNames) {
    BSONObjBuilder bob;
    {
        BSONArrayBuilder databasesBob(bob.subarrayStart("databases"));
        for (const auto& name : databaseNames) {
            BSONObjBuilder nameBob(databasesBob.subobjStart());
            nameBob.append("name", name);
        }
    }
    bob.append("ok", 1);
    // Set the operation time as if the remote mock server received the
    // 'listDatabases' cmd with '$replData' set to true.
    bob.append(LogicalTime::kOperationTimeFieldName, Timestamp(1, 1));
    return bob.obj();
}

BSONObj makeFindResponse(ErrorCodes::Error code = ErrorCodes::OK) {
    BSONObjBuilder bob;
    if (code != ErrorCodes::OK) {
        bob.append("ok", 0);
        bob.append("code", code);
    } else {
        bob.append("ok", 1);
    }
    return bob.obj();
}

}  // namespace

class TenantMigrationRecipientServiceTest : public ServiceContextMongoDTest {
public:
    class stopFailPointEnableBlock : public FailPointEnableBlock {
    public:
        explicit stopFailPointEnableBlock(StringData failPointName,
                                          std::int32_t error = stopFailPointErrorCode)
            : FailPointEnableBlock(failPointName,
                                   BSON("action"
                                        << "stop"
                                        << "stopErrorCode" << error)) {}
    };

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();

        // Only the ReplicaSetMonitor scanning protocol supports mock connections.
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kScanning);
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        // Automatically mark the state doc garbage collectable after data sync completion.
        globalFailPointRegistry()
            .find("autoRecipientForgetMigration")
            ->setMode(FailPoint::alwaysOn);

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());

            // Need real (non-mock) storage for the oplog buffer.
            StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
            opObserverRegistry->addObserver(
                std::make_unique<PrimaryOnlyServiceOpObserver>(serviceContext));

            _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
            std::unique_ptr<TenantMigrationRecipientService> service =
                std::make_unique<TenantMigrationRecipientService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
        }
        stepUp();

        _service = _registry->lookupServiceByName(
            TenantMigrationRecipientService::kTenantMigrationRecipientServiceName);
        ASSERT(_service);

        // MockReplicaSet uses custom connection string which does not support auth.
        auto authFp = globalFailPointRegistry().find("skipTenantMigrationRecipientAuth");
        authFp->setMode(FailPoint::alwaysOn);

        // Set up clocks.
        serviceContext->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(_clkSource));
        serviceContext->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));

        // Timestamps of "0 seconds" are not allowed, so we must advance our clock mock to the first
        // real second.
        _clkSource->advance(Milliseconds(1000));
    }

    void tearDown() override {
        auto authFp = globalFailPointRegistry().find("skipTenantMigrationRecipientAuth");
        authFp->setMode(FailPoint::off);

        WaitForMajorityService::get(getServiceContext()).shutDown();

        _registry->onShutdown();
        _service = nullptr;

        StorageInterface::set(getServiceContext(), {});

        // Clearing the connection pool is necessary when doing tests which use the
        // ReplicaSetMonitor.  See src/mongo/dbtests/mock/mock_replica_set.h for details.
        ScopedDbConnection::clearPool();
        ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol();
        ServiceContextMongoDTest::tearDown();
    }

    void stepDown() {
        ASSERT_OK(ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(MemberState::RS_SECONDARY));
        _registry->onStepDown();
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        auto replCoord = ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            OpTimeAndWallTime(OpTime(Timestamp(1, 1), _term), Date_t()));

        _registry->onStepUpComplete(opCtx.get(), _term);
    }

protected:
    PrimaryOnlyServiceRegistry* _registry;
    PrimaryOnlyService* _service;
    long long _term = 0;

    bool _collCreated = false;
    size_t _numSecondaryIndexesCreated{0};
    size_t _numDocsInserted{0};

    const TenantMigrationPEMPayload kRecipientPEMPayload = [&] {
        std::ifstream infile("jstests/libs/client.pem");
        std::string buf((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());

        auto swCertificateBlob =
            ssl_util::findPEMBlob(buf, "CERTIFICATE"_sd, 0 /* position */, false /* allowEmpty */);
        ASSERT_TRUE(swCertificateBlob.isOK());

        auto swPrivateKeyBlob =
            ssl_util::findPEMBlob(buf, "PRIVATE KEY"_sd, 0 /* position */, false /* allowEmpty */);
        ASSERT_TRUE(swPrivateKeyBlob.isOK());

        return TenantMigrationPEMPayload{swCertificateBlob.getValue().toString(),
                                         swPrivateKeyBlob.getValue().toString()};
    }();

    void checkStateDocPersisted(OperationContext* opCtx,
                                const TenantMigrationRecipientService::Instance* instance) {
        auto memoryStateDoc = getStateDoc(instance);
        auto persistedStateDocWithStatus =
            tenantMigrationRecipientEntryHelpers::getStateDoc(opCtx, memoryStateDoc.getId());
        ASSERT_OK(persistedStateDocWithStatus.getStatus());
        ASSERT_BSONOBJ_EQ(memoryStateDoc.toBSON(), persistedStateDocWithStatus.getValue().toBSON());
    }
    void insertToAllNodes(MockReplicaSet* replSet, const std::string& nss, BSONObj obj) {
        for (const auto& host : replSet->getHosts()) {
            replSet->getNode(host.toString())->insert(nss, obj);
        }
    }

    void clearCollectionAllNodes(MockReplicaSet* replSet, const std::string& nss) {
        for (const auto& host : replSet->getHosts()) {
            replSet->getNode(host.toString())->remove(nss, Query());
        }
    }

    void insertTopOfOplog(MockReplicaSet* replSet, const OpTime& topOfOplogOpTime) {
        // The MockRemoteDBService does not actually implement the database, so to make our
        // find work correctly we must make sure there's only one document to find.
        clearCollectionAllNodes(replSet, NamespaceString::kRsOplogNamespace.ns());
        insertToAllNodes(replSet,
                         NamespaceString::kRsOplogNamespace.ns(),
                         makeOplogEntry(topOfOplogOpTime,
                                        OpTypeEnum::kNoop,
                                        {} /* namespace */,
                                        boost::none /* uuid */,
                                        BSONObj() /* o */,
                                        boost::none /* o2 */)
                             .getEntry()
                             .toBSON());
    }

    // Accessors to class private members
    DBClientConnection* getClient(const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_client.get();
    }

    DBClientConnection* getOplogFetcherClient(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_oplogFetcherClient.get();
    }

    OplogFetcherMock* getDonorOplogFetcher(
        const TenantMigrationRecipientService::Instance* instance) const {
        return static_cast<OplogFetcherMock*>(instance->_donorOplogFetcher.get());
    }

    OplogBufferCollection* getDonorOplogBuffer(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_donorOplogBuffer.get();
    }

    const TenantMigrationRecipientDocument& getStateDoc(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_stateDoc;
    }

    /**
     * Advance the time by millis on both clock source mocks.
     */
    void advanceTime(Milliseconds millis) {
        _clkSource->advance(millis);
    }

    /**
     * Assumes that the times on both clock source mocks is the same.
     */
    Date_t now() {
        return _clkSource->now();
    }

private:
    std::shared_ptr<ClockSourceMock> _clkSource = std::make_shared<ClockSourceMock>();

    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};
};

#ifdef MONGO_CONFIG_SSL
TEST_F(TenantMigrationRecipientServiceTest, BasicTenantMigrationRecipientServiceInstanceCreation) {
    stopFailPointEnableBlock fp("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        "donor-rs/localhost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}


TEST_F(TenantMigrationRecipientServiceTest, InstanceReportsErrorOnFailureWhilePersisitingStateDoc) {
    stopFailPointEnableBlock fp("failWhilePersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        "donor-rs/localhost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Should be able to see the instance task failure error.
    auto status = instance->getDataSyncCompletionFuture().getNoThrow();
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, status.code());
    // Should also fail to mark the state doc garbage collectable if we have failed to persist the
    // state doc at the first place.
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_Primary) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to primary.
    auto primary = replSet.getHosts()[0].toString();
    ASSERT_EQ(primary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(primary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_Secondary) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2, true /* hasPrimary */, true /* dollarPrefixHosts */);

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::SecondaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to secondary.
    auto secondary = replSet.getHosts()[1].toString();
    ASSERT_EQ(secondary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(secondary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_PrimaryFails) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    FailPointEnableBlock timeoutFp("setTenantMigrationRecipientInstanceHostTimeout",
                                   BSON("findHostTimeoutMillis" << 100));

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    // Primary is unavailable.
    replSet.kill(replSet.getHosts()[0].toString());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    AtomicWord<bool> runReplMonitor{true};
    // Keep scanning the replica set while waiting to reach the failpoint. This would normally
    // be automatic but that doesn't work with mock replica sets.
    stdx::thread replMonitorThread([&] {
        Client::initThread("replMonitorThread");
        while (runReplMonitor.load()) {
            auto monitor = ReplicaSetMonitor::get(replSet.getSetName());
            // Monitor may not have been created yet.
            if (monitor) {
                monitor->runScanForMockReplicaSet();
            }
            mongo::sleepmillis(100);
        }
    });

    taskFp->waitForTimesEntered(initialTimesEntered + 1);
    runReplMonitor.store(false);
    replMonitorThread.join();

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);

    taskFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnect_ExcludedPrimaryHostPrimaryOnly) {
    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    instance->excludeDonorHost(hosts.at(0), now + Milliseconds(500));

    AtomicWord<bool> runReplMonitor{true};
    // Keep scanning the replica set while waiting to reach the failpoint. This would normally
    // be automatic but that doesn't work with mock replica sets.
    stdx::thread replMonitorThread([&] {
        Client::initThread("replMonitorThread");
        while (runReplMonitor.load()) {
            auto monitor = ReplicaSetMonitor::get(replSet.getSetName());
            // Monitor may not have been created yet.
            if (monitor) {
                monitor->runScanForMockReplicaSet();
            }
            mongo::sleepmillis(100);
        }
    });

    taskFp->waitForTimesEntered(initialTimesEntered + 1);
    runReplMonitor.store(false);
    replMonitorThread.join();

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);

    taskFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnect_ExcludedPrimaryHostExpires) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost(hosts.at(0), now + excludeTime);

    // Advance the clock past excludeTime.
    advanceTime(excludeTime + Milliseconds(500));

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to primary.
    auto primary = replSet.getHosts()[0].toString();
    ASSERT_EQ(primary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(primary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnect_ExcludedAllHostsNearest) {
    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::Nearest),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Mark all hosts as excluded.
    auto hosts = replSet.getHosts();
    for (const auto& host : hosts) {
        const auto now = opCtx->getServiceContext()->getFastClockSource()->now();
        instance->excludeDonorHost(host, now + Milliseconds(500));
    }

    AtomicWord<bool> runReplMonitor{true};
    // Keep scanning the replica set while waiting to reach the failpoint. This would normally
    // be automatic but that doesn't work with mock replica sets.
    stdx::thread replMonitorThread([&] {
        Client::initThread("replMonitorThread");
        while (runReplMonitor.load()) {
            auto monitor = ReplicaSetMonitor::get(replSet.getSetName());
            // Monitor may not have been created yet.
            if (monitor) {
                monitor->runScanForMockReplicaSet();
            }
            mongo::sleepmillis(100);
        }
    });

    taskFp->waitForTimesEntered(initialTimesEntered + 1);
    runReplMonitor.store(false);
    replMonitorThread.join();

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);

    taskFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnect_ExcludedPrimaryWithPrimaryPreferred) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost(hosts.at(0), now + excludeTime);

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to secondary.
    auto secondary = replSet.getHosts()[1].toString();
    ASSERT_EQ(secondary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(secondary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnect_ExcludedPrimaryExpiresWithPrimaryPreferred) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost(hosts.at(0), now + excludeTime);

    // Advance the clock past excludeTime.
    advanceTime(excludeTime + Milliseconds(500));

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to primary.
    auto primary = replSet.getHosts()[0].toString();
    ASSERT_EQ(primary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(primary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_PrimaryFailsOver) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2, true /* hasPrimary */, true /* dollarPrefixHosts */);

    // Primary is unavailable.
    replSet.kill(replSet.getHosts()[0].toString());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to secondary.
    auto secondary = replSet.getHosts()[1].toString();
    ASSERT_EQ(secondary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(secondary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_BadConnectString) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "broken,connect,string,no,set,name",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(TenantMigrationRecipientService::Instance::getOrCreate(
                           opCtx.get(), _service, initialStateDocument.toBSON()),
                       DBException,
                       ErrorCodes::FailedToParse);
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_NonSetConnectString) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "localhost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(TenantMigrationRecipientService::Instance::getOrCreate(
                           opCtx.get(), _service, initialStateDocument.toBSON()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientGetStartOpTime_NoTransaction) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartFetchingDonorOpTime());
    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingDonorOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTime_Advances_NoTransaction) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    auto pauseFailPoint =
        globalFailPointRegistry().find("pauseAfterRetrievingLastTxnMigrationRecipientInstance");
    auto timesEntered = pauseFailPoint->setMode(FailPoint::alwaysOn, 0);

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);
    const OpTime newTopOfOplogOpTime(Timestamp(6, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    pauseFailPoint->waitForTimesEntered(timesEntered + 1);
    insertTopOfOplog(&replSet, newTopOfOplogOpTime);
    pauseFailPoint->setMode(FailPoint::off, 0);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartFetchingDonorOpTime());
    ASSERT_EQ(newTopOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingDonorOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientGetStartOpTime_Transaction) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime txnStartOpTime(Timestamp(3, 1), 1);
    const OpTime txnLastWriteOpTime(Timestamp(4, 1), 1);
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);
    SessionTxnRecord lastTxn(makeLogicalSessionIdForTest(), 100, txnLastWriteOpTime, Date_t());
    lastTxn.setStartOpTime(txnStartOpTime);
    lastTxn.setState(DurableTxnStateEnum::kInProgress);
    insertToAllNodes(
        &replSet, NamespaceString::kSessionTransactionsTableNamespace.ns(), lastTxn.toBSON());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(txnStartOpTime, getStateDoc(instance.get()).getStartFetchingDonorOpTime());
    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingDonorOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTime_Advances_Transaction) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    auto pauseFailPoint =
        globalFailPointRegistry().find("pauseAfterRetrievingLastTxnMigrationRecipientInstance");
    auto timesEntered = pauseFailPoint->setMode(FailPoint::alwaysOn, 0);

    const UUID migrationUUID = UUID::gen();
    const OpTime txnStartOpTime(Timestamp(3, 1), 1);
    const OpTime txnLastWriteOpTime(Timestamp(4, 1), 1);
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);
    const OpTime newTopOfOplogOpTime(Timestamp(6, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);
    SessionTxnRecord lastTxn(makeLogicalSessionIdForTest(), 100, txnLastWriteOpTime, Date_t());
    lastTxn.setStartOpTime(txnStartOpTime);
    lastTxn.setState(DurableTxnStateEnum::kInProgress);
    insertToAllNodes(
        &replSet, NamespaceString::kSessionTransactionsTableNamespace.ns(), lastTxn.toBSON());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    pauseFailPoint->waitForTimesEntered(timesEntered + 1);
    insertTopOfOplog(&replSet, newTopOfOplogOpTime);
    pauseFailPoint->setMode(FailPoint::off, 0);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(txnStartOpTime, getStateDoc(instance.get()).getStartFetchingDonorOpTime());
    ASSERT_EQ(newTopOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingDonorOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTimes_RemoteOplogQueryFails) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.  Fail to populate the remote oplog mock.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    // Even though we failed, the memory state should still match the on-disk state.
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientStartOplogFetcher) {
    stopFailPointEnableBlock fp("fpAfterStartingOplogFetcherMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    checkStateDocPersisted(opCtx.get(), instance.get());
    // The oplog fetcher should exist and be running.
    auto oplogFetcher = getDonorOplogFetcher(instance.get());
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientStartsCloner) {
    stopFailPointEnableBlock fp("fpAfterCollectionClonerDone");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpGuard = makeGuard([&taskFp] { taskFp->setMode(FailPoint::off); });

    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    OpTime cloneCompletionRecipientOpTime;
    {
        // This failpoint will stop us just before the cloner starts.
        FailPointEnableBlock fp("fpAfterStartingOplogFetcherMigrationRecipientInstance",
                                BSON("action"
                                     << "hang"));
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());

        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);

        // since the listDatabase would return empty, cloner will not make any new writes. So,
        // it's safe to assume the  donor opime at clone completion point will be the
        // optime at which migration start optimes got persisted.
        cloneCompletionRecipientOpTime =
            ReplicationCoordinator::get(getServiceContext())->getMyLastAppliedOpTime();
    }

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    ASSERT_EQ(OpTime(Timestamp(1, 1), OpTime::kUninitializedTerm),
              getStateDoc(instance.get()).getDataConsistentStopDonorOpTime());
    ASSERT_EQ(cloneCompletionRecipientOpTime,
              getStateDoc(instance.get()).getCloneFinishedRecipientOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());

    taskFpGuard.dismiss();
    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogFetcherFailsDuringOplogApplication) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Setting these causes us to skip cloning.
    initialStateDocument.setCloneFinishedRecipientOpTime(topOfOplogOpTime);
    initialStateDocument.setDataConsistentStopDonorOpTime(topOfOplogOpTime);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    LOGV2(4881201,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());


    checkStateDocPersisted(opCtx.get(), instance.get());
    // The oplog fetcher should exist and be running.
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    // Kill it.
    oplogFetcher->shutdownWith({ErrorCodes::Error(4881203), "Injected error"});

    // Wait for task completion failure.
    auto status = instance->getDataSyncCompletionFuture().getNoThrow();
    ASSERT_EQ(4881203, status.code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogApplierFails) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);
    const OpTime injectedEntryOpTime(Timestamp(6, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Setting these causes us to skip cloning.
    initialStateDocument.setCloneFinishedRecipientOpTime(topOfOplogOpTime);
    initialStateDocument.setDataConsistentStopDonorOpTime(topOfOplogOpTime);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        // Use this failpoint to avoid races between the test thread accessing the oplogFetcher and
        // the migration instance freeing the oplogFetcher on errors.
        FailPointEnableBlock taskFp("hangBeforeTaskCompletion");
        {
            FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
            // Create and start the instance.
            instance = TenantMigrationRecipientService::Instance::getOrCreate(
                opCtx.get(), _service, initialStateDocument.toBSON());
            ASSERT(instance.get());
            instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
        }

        LOGV2(4881208,
              "Waiting for recipient service to reach consistent state",
              "suite"_attr = _agent.getSuiteName(),
              "test"_attr = _agent.getTestName());
        instance->waitUntilMigrationReachesConsistentState(opCtx.get());

        checkStateDocPersisted(opCtx.get(), instance.get());
        // The oplog fetcher should exist and be running.
        auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
        ASSERT_TRUE(oplogFetcher != nullptr);
        ASSERT_TRUE(oplogFetcher->isActive());

        // Send an oplog entry not from our tenant, which should cause the oplog applier to assert.
        auto oplogEntry = makeOplogEntry(injectedEntryOpTime,
                                         OpTypeEnum::kInsert,
                                         NamespaceString("admin.bogus"),
                                         UUID::gen(),
                                         BSON("_id"
                                              << "bad insert"),
                                         boost::none /* o2 */);
        oplogFetcher->receiveBatch(
            1LL, {oplogEntry.getEntry().toBSON()}, injectedEntryOpTime.getTimestamp());
    }

    // Wait for task completion failure.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, StoppingApplierAllowsCompletion) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Setting these causes us to skip cloning.
    initialStateDocument.setCloneFinishedRecipientOpTime(topOfOplogOpTime);
    initialStateDocument.setDataConsistentStopDonorOpTime(topOfOplogOpTime);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    LOGV2(4881209,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    checkStateDocPersisted(opCtx.get(), instance.get());
    // The oplog fetcher should exist and be running.
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();

    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientAddResumeTokenNoopsToBuffer) {
    stopFailPointEnableBlock fp("fpAfterCollectionClonerDone");
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

    // Hang the recipient service after starting the oplog fetcher.
    auto oplogFetcherFP =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = oplogFetcherFP->setMode(FailPoint::alwaysOn,
                                                       0,
                                                       BSON("action"
                                                            << "hang"));

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    // Wait for the oplog fetcher to start.
    oplogFetcherFP->waitForTimesEntered(initialTimesEntered + 1);

    // Feed the oplog fetcher a resume token.
    auto oplogFetcher = getDonorOplogFetcher(instance.get());
    const auto resumeToken1 = topOfOplogOpTime.getTimestamp();
    auto oplogEntry1 = makeOplogEntry(topOfOplogOpTime,
                                      OpTypeEnum::kInsert,
                                      NamespaceString("foo.bar") /* namespace */,
                                      UUID::gen() /* uuid */,
                                      BSON("doc" << 2) /* o */,
                                      boost::none /* o2 */);
    oplogFetcher->receiveBatch(17, {oplogEntry1.getEntry().toBSON()}, resumeToken1);

    const Timestamp oplogEntryTS2 = Timestamp(6, 2);
    const Timestamp resumeToken2 = Timestamp(7, 3);
    auto oplogEntry2 = makeOplogEntry(OpTime(oplogEntryTS2, topOfOplogOpTime.getTerm()),
                                      OpTypeEnum::kInsert,
                                      NamespaceString("foo.bar") /* namespace */,
                                      UUID::gen() /* uuid */,
                                      BSON("doc" << 3) /* o */,
                                      boost::none /* o2 */);
    oplogFetcher->receiveBatch(17, {oplogEntry2.getEntry().toBSON()}, resumeToken2);

    // Receive an empty batch.
    oplogFetcher->receiveBatch(17, {}, resumeToken2);

    auto oplogBuffer = getDonorOplogBuffer(instance.get());
    ASSERT_EQUALS(oplogBuffer->getCount(), 3);

    {
        BSONObj insertDoc;
        ASSERT_TRUE(oplogBuffer->tryPop(opCtx.get(), &insertDoc));
        LOGV2(5124601, "Insert oplog entry", "entry"_attr = insertDoc);
        ASSERT_BSONOBJ_EQ(insertDoc, oplogEntry1.getEntry().toBSON());
    }

    {
        BSONObj insertDoc;
        ASSERT_TRUE(oplogBuffer->tryPop(opCtx.get(), &insertDoc));
        LOGV2(5124602, "Insert oplog entry", "entry"_attr = insertDoc);
        ASSERT_BSONOBJ_EQ(insertDoc, oplogEntry2.getEntry().toBSON());
    }

    {
        BSONObj noopDoc;
        ASSERT_TRUE(oplogBuffer->tryPop(opCtx.get(), &noopDoc));
        LOGV2(5124603, "Noop oplog entry", "entry"_attr = noopDoc);
        OplogEntry noopEntry(noopDoc);
        ASSERT_TRUE(noopEntry.getOpType() == OpTypeEnum::kNoop);
        ASSERT_EQUALS(noopEntry.getTimestamp(), resumeToken2);
        ASSERT_EQUALS(noopEntry.getTerm().get(), -1);
        ASSERT_EQUALS(noopEntry.getNss(), NamespaceString(""));
    }

    ASSERT_TRUE(oplogBuffer->isEmpty());

    // Let the recipient service complete.
    oplogFetcherFP->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_BeforeRun) {
    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    auto fp = globalFailPointRegistry().find("pauseBeforeRunTenantMigrationRecipientInstance");
    fp->setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());

    // The task is interrupted before it start the chain.
    instance->interrupt({ErrorCodes::InterruptedDueToReplStateChange, "Test stepdown"});

    // Test that receiving recipientForgetMigration command after that should result in the same
    // error.
    ASSERT_THROWS_CODE(instance->onReceiveRecipientForgetMigration(opCtx.get()),
                       AssertionException,
                       ErrorCodes::InterruptedDueToReplStateChange);

    fp->setMode(FailPoint::off);

    // We should fail to mark the state doc garbage collectable.
    ASSERT_EQ(instance->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_FailToInitializeStateDoc) {
    stopFailPointEnableBlock fp("failWhilePersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    auto instance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());

    ASSERT_THROWS_CODE(instance->onReceiveRecipientForgetMigration(opCtx.get()),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
    // We should fail to mark the state doc garbage collectable if we have failed to initialize and
    // persist the state doc at the first place.
    ASSERT_EQ(instance->getCompletionFuture().getNoThrow(), ErrorCodes::NotWritablePrimary);
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_WaitUntilStateDocInitialized) {
    // The test fixture forgets the migration automatically, disable the failpoint for this test so
    // the migration continues to wait for the recipientForgetMigration command after persisting the
    // state doc.
    auto autoForgetFp = globalFailPointRegistry().find("autoRecipientForgetMigration");
    autoForgetFp->setMode(FailPoint::off);

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    auto fp = globalFailPointRegistry().find("pauseAfterRunTenantMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Test that onReceiveRecipientForgetMigration waits until the state doc is initialized.
    opCtx->setDeadlineAfterNowBy(Seconds(2), opCtx->getTimeoutError());
    // Advance time past deadline.
    advanceTime(Milliseconds(3000));
    ASSERT_THROWS_CODE(instance->onReceiveRecipientForgetMigration(opCtx.get()),
                       AssertionException,
                       opCtx->getTimeoutError());

    {
        // Hang the chain after persisting the state doc.
        FailPointEnableBlock fpPersistingStateDoc(
            "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
            BSON("action"
                 << "hang"));

        // Unblock the task chain so the state doc can be persisted.
        fp->setMode(FailPoint::off);

        // Make a new opCtx as the old one has expired due to timeout errors.
        opCtx.reset();
        opCtx = makeOperationContext();

        // Test that onReceiveRecipientForgetMigration goes through now that the state doc has been
        // persisted.
        instance->onReceiveRecipientForgetMigration(opCtx.get());
    }

    ASSERT_EQ(instance->getDataSyncCompletionFuture().getNoThrow(),
              ErrorCodes::TenantMigrationForgotten);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    const auto doc = getStateDoc(instance.get());
    LOGV2(4881411,
          "Test migration complete",
          "preStateDoc"_attr = initialStateDocument.toBSON(),
          "postStateDoc"_attr = doc.toBSON());
    ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
    ASSERT_EQ(doc.getTenantId(), "tenantA");
    ASSERT_TRUE(doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
    ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kDone);
    ASSERT_TRUE(doc.getExpireAt() != boost::none);
    ASSERT_TRUE(doc.getExpireAt().get() > opCtx->getServiceContext()->getFastClockSource()->now());
    ASSERT_TRUE(doc.getStartApplyingDonorOpTime() == boost::none);
    ASSERT_TRUE(doc.getStartFetchingDonorOpTime() == boost::none);
    ASSERT_TRUE(doc.getDataConsistentStopDonorOpTime() == boost::none);
    ASSERT_TRUE(doc.getCloneFinishedRecipientOpTime() == boost::none);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_AfterStartOpTimes) {
    auto fp =
        globalFailPointRegistry().find("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));


    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    fp->waitForTimesEntered(initialTimesEntered + 1);
    instance->onReceiveRecipientForgetMigration(opCtx.get());

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

    fp->setMode(FailPoint::off);
    ASSERT_EQ(instance->getDataSyncCompletionFuture().getNoThrow(),
              ErrorCodes::TenantMigrationForgotten);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    const auto doc = getStateDoc(instance.get());
    LOGV2(4881412,
          "Test migration complete",
          "preStateDoc"_attr = initialStateDocument.toBSON(),
          "postStateDoc"_attr = doc.toBSON());
    ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
    ASSERT_EQ(doc.getTenantId(), "tenantA");
    ASSERT_TRUE(doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
    ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kDone);
    ASSERT_TRUE(doc.getExpireAt() != boost::none);
    ASSERT_TRUE(doc.getExpireAt().get() > opCtx->getServiceContext()->getFastClockSource()->now());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_AfterConsistent) {
    // The test fixture forgets the migration automatically, disable the failpoint for this test so
    // the migration continues to wait for the recipientForgetMigration command after reaching data
    // consistent state.
    auto autoForgetFp = globalFailPointRegistry().find("autoRecipientForgetMigration");
    autoForgetFp->setMode(FailPoint::off);

    auto dataConsistentFp =
        globalFailPointRegistry().find("fpAfterDataConsistentMigrationRecipientInstance");
    auto initialTimesEntered = dataConsistentFp->setMode(FailPoint::alwaysOn,
                                                         0,
                                                         BSON("action"
                                                              << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Setting these causes us to skip cloning.
    initialStateDocument.setCloneFinishedRecipientOpTime(topOfOplogOpTime);
    initialStateDocument.setDataConsistentStopDonorOpTime(topOfOplogOpTime);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }
    dataConsistentFp->waitForTimesEntered(initialTimesEntered + 1);

    {
        const auto doc = getStateDoc(instance.get());
        LOGV2(4881413,
              "Test migration after consistent",
              "preStateDoc"_attr = initialStateDocument.toBSON(),
              "postStateDoc"_attr = doc.toBSON());
        ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
        ASSERT_EQ(doc.getTenantId(), "tenantA");
        ASSERT_TRUE(
            doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
        ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kConsistent);
        ASSERT_TRUE(doc.getExpireAt() == boost::none);
        checkStateDocPersisted(opCtx.get(), instance.get());
    }

    instance->onReceiveRecipientForgetMigration(opCtx.get());

    // Test receiving duplicating recipientForgetMigration requests.
    instance->onReceiveRecipientForgetMigration(opCtx.get());

    // Continue after data being consistent.
    dataConsistentFp->setMode(FailPoint::off);

    // The data sync should have completed.
    ASSERT_EQ(instance->getDataSyncCompletionFuture().getNoThrow(),
              ErrorCodes::TenantMigrationForgotten);

    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    {
        const auto doc = getStateDoc(instance.get());
        LOGV2(4881414,
              "Test migration complete",
              "preStateDoc"_attr = initialStateDocument.toBSON(),
              "postStateDoc"_attr = doc.toBSON());
        ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
        ASSERT_EQ(doc.getTenantId(), "tenantA");
        ASSERT_TRUE(
            doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
        ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kDone);
        ASSERT_TRUE(doc.getExpireAt() != boost::none);
        ASSERT_TRUE(doc.getExpireAt().get() >
                    opCtx->getServiceContext()->getFastClockSource()->now());
        checkStateDocPersisted(opCtx.get(), instance.get());
    }
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_AfterFail) {
    // The test fixture forgets the migration automatically, disable the failpoint for this test so
    // the migration continues to wait for the recipientForgetMigration command after getting an
    // error from the migration.
    auto autoForgetFp = globalFailPointRegistry().find("autoRecipientForgetMigration");
    autoForgetFp->setMode(FailPoint::off);

    stopFailPointEnableBlock fp("fpAfterCollectionClonerDone");
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Setting these causes us to skip cloning.
    initialStateDocument.setCloneFinishedRecipientOpTime(topOfOplogOpTime);
    initialStateDocument.setDataConsistentStopDonorOpTime(topOfOplogOpTime);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    ASSERT_THROWS_CODE(instance->waitUntilMigrationReachesConsistentState(opCtx.get()),
                       AssertionException,
                       stopFailPointErrorCode);

    {
        const auto doc = getStateDoc(instance.get());
        LOGV2(4881415,
              "Test migration after collection cloner done",
              "preStateDoc"_attr = initialStateDocument.toBSON(),
              "postStateDoc"_attr = doc.toBSON());
        ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
        ASSERT_EQ(doc.getTenantId(), "tenantA");
        ASSERT_TRUE(
            doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
        ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kStarted);
        ASSERT_TRUE(doc.getExpireAt() == boost::none);
        checkStateDocPersisted(opCtx.get(), instance.get());
    }

    // The data sync should have completed.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());

    // The instance should still be running and waiting for the recipientForgetMigration command.
    instance->onReceiveRecipientForgetMigration(opCtx.get());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    {
        const auto doc = getStateDoc(instance.get());
        LOGV2(4881416,
              "Test migration complete",
              "preStateDoc"_attr = initialStateDocument.toBSON(),
              "postStateDoc"_attr = doc.toBSON());
        ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
        ASSERT_EQ(doc.getTenantId(), "tenantA");
        ASSERT_TRUE(
            doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
        ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kDone);
        ASSERT_TRUE(doc.getExpireAt() != boost::none);
        ASSERT_TRUE(doc.getExpireAt().get() >
                    opCtx->getServiceContext()->getFastClockSource()->now());
        checkStateDocPersisted(opCtx.get(), instance.get());
    }
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_FailToMarkGarbageCollectable) {
    // The test fixture forgets the migration automatically, disable the failpoint for this test so
    // the migration continues to wait for the recipientForgetMigration command after getting an
    // error from the migration.
    auto autoForgetFp = globalFailPointRegistry().find("autoRecipientForgetMigration");
    autoForgetFp->setMode(FailPoint::off);

    stopFailPointEnableBlock fp("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // The data sync should have completed.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());

    // Fail marking the state doc garbage collectable with a different error code, simulating a
    // stepDown.
    stopFailPointEnableBlock fpFailForget("fpAfterReceivingRecipientForgetMigration",
                                          ErrorCodes::NotWritablePrimary);

    // The instance should still be running and waiting for the recipientForgetMigration command.
    instance->onReceiveRecipientForgetMigration(opCtx.get());
    // Check that it fails to mark the state doc garbage collectable.
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, instance->getCompletionFuture().getNoThrow().code());

    {
        const auto doc = getStateDoc(instance.get());
        LOGV2(4881417,
              "Test migration complete",
              "preStateDoc"_attr = initialStateDocument.toBSON(),
              "postStateDoc"_attr = doc.toBSON());
        ASSERT_EQ(doc.getDonorConnectionString(), replSet.getConnectionString());
        ASSERT_EQ(doc.getTenantId(), "tenantA");
        ASSERT_TRUE(
            doc.getReadPreference().equals(ReadPreferenceSetting(ReadPreference::PrimaryOnly)));
        ASSERT_TRUE(doc.getState() == TenantMigrationRecipientStateEnum::kStarted);
        ASSERT_TRUE(doc.getExpireAt() == boost::none);
        checkStateDocPersisted(opCtx.get(), instance.get());
    }
}
#endif
}  // namespace repl
}  // namespace mongo
