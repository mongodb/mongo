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

#include <fstream>
#include <memory>

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/streamable_replica_set_monitor_for_testing.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version_document_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
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
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/ssl_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {

namespace {
constexpr std::int32_t stopFailPointErrorCode = 4880402;
const Timestamp kDefaultStartMigrationTimestamp(1, 1);

OplogEntry makeOplogEntry(OpTime opTime,
                          OpTypeEnum opType,
                          NamespaceString nss,
                          const boost::optional<UUID>& uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2) {
    return {DurableOplogEntry(opTime,                     // optime
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
                              {},                         // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

MutableOplogEntry makeNoOpOplogEntry(OpTime opTime,
                                     NamespaceString nss,
                                     const boost::optional<UUID>& uuid,
                                     BSONObj o,
                                     boost::optional<UUID> migrationUUID) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setOpTime(opTime);
    oplogEntry.setNss(nss);
    oplogEntry.setObject({});
    oplogEntry.setObject2(o);
    oplogEntry.setWallClockTime(Date_t::now());
    if (migrationUUID) {
        oplogEntry.setFromTenantMigration(migrationUUID.value());
    }
    return oplogEntry;
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

        // Fake replSet just for creating consistent URI for monitor
        MockReplicaSet replSet("donorSet", 1, true /* hasPrimary */, true /* dollarPrefixHosts */);
        _rsmMonitor.setup(replSet.getURI());

        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        WaitForMajorityService::get(serviceContext).startup(serviceContext);

        // Automatically mark the state doc garbage collectable after data sync completion.
        globalFailPointRegistry()
            .find("autoRecipientForgetMigration")
            ->setMode(FailPoint::alwaysOn);

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());
            {
                Lock::GlobalWrite lk(opCtx.get());
                OldClientContext ctx(opCtx.get(), NamespaceString::kRsOplogNamespace);
                tenant_migration_util::createOplogViewForTenantMigrations(opCtx.get(), ctx.db());
            }

            // Need real (non-mock) storage for the oplog buffer.
            StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

            // The DropPendingCollectionReaper is required to drop the oplog buffer collection.
            repl::DropPendingCollectionReaper::set(
                serviceContext,
                std::make_unique<repl::DropPendingCollectionReaper>(
                    StorageInterface::get(serviceContext)));

            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(
                std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
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

        // Set the sslMode to allowSSL to avoid validation error.
        sslGlobalParams.sslMode.store(SSLParams::SSLMode_allowSSL);
        // Skipped unless tested explicitly, as we will not receive an FCV document from the donor
        // in these unittests without (unsightly) intervention.
        auto compFp = globalFailPointRegistry().find("skipComparingRecipientAndDonorFCV");
        compFp->setMode(FailPoint::alwaysOn);

        // Skip fetching retryable writes, as we will test this logic entirely in integration
        // tests.
        auto fetchRetryableWritesFp =
            globalFailPointRegistry().find("skipFetchingRetryableWritesEntriesBeforeStartOpTime");
        fetchRetryableWritesFp->setMode(FailPoint::alwaysOn);

        // Skip fetching committed transactions, as we will test this logic entirely in integration
        // tests.
        auto fetchCommittedTransactionsFp =
            globalFailPointRegistry().find("skipFetchingCommittedTransactions");
        fetchCommittedTransactionsFp->setMode(FailPoint::alwaysOn);
    }

    void tearDown() override {
        auto authFp = globalFailPointRegistry().find("skipTenantMigrationRecipientAuth");
        authFp->setMode(FailPoint::off);

        // Unset the sslMode.
        sslGlobalParams.sslMode.store(SSLParams::SSLMode_disabled);

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
    TenantMigrationRecipientServiceTest()
        : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

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
    void insertToNodes(MockReplicaSet* replSet,
                       const std::string& nss,
                       BSONObj obj,
                       const std::vector<HostAndPort>& hosts) {
        for (const auto& host : hosts) {
            replSet->getNode(host.toString())->insert(nss, obj);
        }
    }

    void insertToAllNodes(MockReplicaSet* replSet, const std::string& nss, BSONObj obj) {
        insertToNodes(replSet, nss, obj, replSet->getHosts());
    }

    void clearCollection(MockReplicaSet* replSet,
                         const std::string& nss,
                         const std::vector<HostAndPort>& hosts) {
        for (const auto& host : hosts) {
            replSet->getNode(host.toString())->remove(nss, BSONObj{} /*filter*/);
        }
    }

    void insertTopOfOplog(MockReplicaSet* replSet,
                          const OpTime& topOfOplogOpTime,
                          const std::vector<HostAndPort> hosts = {}) {
        const auto targetHosts = hosts.empty() ? replSet->getHosts() : hosts;
        // The MockRemoteDBService does not actually implement the database, so to make our
        // find work correctly we must make sure there's only one document to find.
        clearCollection(replSet, NamespaceString::kRsOplogNamespace.ns(), targetHosts);
        insertToNodes(replSet,
                      NamespaceString::kRsOplogNamespace.ns(),
                      makeOplogEntry(topOfOplogOpTime,
                                     OpTypeEnum::kNoop,
                                     {} /* namespace */,
                                     boost::none /* uuid */,
                                     BSONObj() /* o */,
                                     boost::none /* o2 */)
                          .getEntry()
                          .toBSON(),
                      targetHosts);
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

    TenantOplogApplier* getTenantOplogApplier(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_tenantOplogApplier.get();
    }

    const TenantMigrationRecipientDocument& getStateDoc(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_stateDoc;
    }

    sdam::MockTopologyManager* getTopologyManager() {
        return _rsmMonitor.getTopologyManager();
    }

    /**
     * Advance the time by millis on both clock source mocks.
     */
    void advanceTime(Milliseconds millis) {
        _clkSource.advance(millis);
    }

    /**
     * Assumes that the times on both clock source mocks is the same.
     */
    Date_t now() {
        return _clkSource.now();
    };

    /*
     * Populates the migration state document to simulate a recipient service restart where cloning
     * has already finished. This requires the oplog buffer to contain an oplog entry with the
     * optime to resume from. Otherwise, oplog application will fail when the OplogBatcher seeks
     * to the resume timestamp.
     */
    void updateStateDocToCloningFinished(TenantMigrationRecipientDocument& initialStateDoc,
                                         OpTime cloneFinishedRecipientOpTime,
                                         OpTime dataConsistentStopDonorOpTime,
                                         OpTime startApplyingDonorOpTime,
                                         OpTime startFetchingDonorOptime) {
        initialStateDoc.setCloneFinishedRecipientOpTime(cloneFinishedRecipientOpTime);
        initialStateDoc.setDataConsistentStopDonorOpTime(dataConsistentStopDonorOpTime);
        initialStateDoc.setStartApplyingDonorOpTime(startApplyingDonorOpTime);
        initialStateDoc.setStartFetchingDonorOpTime(startFetchingDonorOptime);
    }

    /**
     * Sets the FCV on the donor so that it can respond to FCV requests appropriately.
     * (Generic FCV reference): This FCV reference should exist across LTS binary versions.
     */
    void setDonorFCV(
        const TenantMigrationRecipientService::Instance* instance,
        multiversion::FeatureCompatibilityVersion version = multiversion::GenericFCV::kLatest) {
        auto fcvDoc = FeatureCompatibilityVersionDocument(version);
        auto client = getClient(instance);
        client->insert(NamespaceString::kServerConfigurationNamespace.ns(), fcvDoc.toBSON());
    }

    ClockSource* clock() {
        return &_clkSource;
    }

private:
    ClockSourceMock _clkSource;

    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};

    StreamableReplicaSetMonitorForTesting _rsmMonitor;
    RAIIServerParameterControllerForTest _findHostTimeout{"defaultFindReplicaSetHostTimeoutMS", 10};
};

#ifdef MONGO_CONFIG_SSL
TEST_F(TenantMigrationRecipientServiceTest, BasicTenantMigrationRecipientServiceInstanceCreation) {
    stopFailPointEnableBlock fp("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "donor-rs/localhost:12345",
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, InstanceReportsErrorOnFailureWhilePersistingStateDoc) {
    stopFailPointEnableBlock fp("failWhilePersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "donor-rs/localhost:12345",
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Should be able to see the instance task failure error.
    auto status = instance->getDataSyncCompletionFuture().getNoThrow();
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, status.code());
    // Should also fail to mark the state doc garbage collectable if we have failed to persist the
    // state doc at the first place.
    ASSERT_EQ(ErrorCodes::NotWritablePrimary,
              instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_Primary) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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

    // Clients should be connected to primary.
    auto primary = replSet.getHosts()[0].toString();
    ASSERT_EQ(primary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(primary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_Secondary) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::SecondaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_ExcludedPrimaryHostPrimaryOnly) {
    FailPointEnableBlock skipRetriesFp("skipRetriesWhenConnectingToDonorHost");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));
    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    instance->excludeDonorHost_forTest(hosts.at(0), now + Milliseconds(500));

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);

    taskFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_ExcludedPrimaryHostExpires) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost_forTest(hosts.at(0), now + excludeTime);

    // Advance the clock past excludeTime.
    advanceTime(excludeTime + Milliseconds(500));

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_ExcludedAllHostsNearest) {
    FailPointEnableBlock skipRetriesFp("skipRetriesWhenConnectingToDonorHost");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::Nearest));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Mark all hosts as excluded.
    auto hosts = replSet.getHosts();
    for (const auto& host : hosts) {
        const auto now = opCtx->getServiceContext()->getFastClockSource()->now();
        instance->excludeDonorHost_forTest(host, now + Milliseconds(500));
    }

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);

    taskFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_ExcludedPrimaryWithPrimaryPreferred) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost_forTest(hosts.at(0), now + excludeTime);

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_ExcludedPrimaryExpiresWithPrimaryPreferred) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Mark the primary as excluded.
    auto hosts = replSet.getHosts();
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost_forTest(hosts.at(0), now + excludeTime);

    // Advance the clock past excludeTime.
    advanceTime(excludeTime + Milliseconds(500));

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_SecondariesDownOrExcludedSecondaryOnly) {
    FailPointEnableBlock skipRetriesFp("skipRetriesWhenConnectingToDonorHost");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::SecondaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));
    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Shutdown one secondary and mark the other secondary as excluded.
    auto hosts = replSet.getHosts();
    replSet.kill(hosts[1].toString());
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost_forTest(hosts.at(2), now + excludeTime);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);

    taskFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_SecondariesDownOrExcludedSecondaryPreferred) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto taskFpInitialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Hang the migration before attempting to connect to clients.
    auto hangFp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto hangFpInitialTimesEntered = hangFp->setMode(FailPoint::alwaysOn,
                                                     0,
                                                     BSON("action"
                                                          << "hang"));

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    hangFp->waitForTimesEntered(hangFpInitialTimesEntered + 1);

    // Shutdown one secondary and mark the other secondary as excluded.
    auto hosts = replSet.getHosts();
    replSet.kill(hosts[1].toString());
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto excludeTime = Milliseconds(500);
    instance->excludeDonorHost_forTest(hosts.at(2), now + excludeTime);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    hangFp->setMode(FailPoint::off);
    taskFp->waitForTimesEntered(taskFpInitialTimesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to the primary.
    auto primary = replSet.getHosts()[0].toString();
    ASSERT_EQ(primary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(primary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_RemoteMajorityOpTimeBehindStartApplying) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime remoteMajorityOpTime(Timestamp(5, 1), 1);
    const OpTime startApplyingOpTime(Timestamp(6, 1), 1);

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto timesEntered = taskFp->setMode(FailPoint::alwaysOn, 0);

    // Insert the remote majority optime into the oplogs of the first two hosts.
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    const auto hosts = replSet.getHosts();
    const std::vector<HostAndPort> advancedOpTimeHosts = {hosts.begin(), hosts.begin() + 2};

    insertTopOfOplog(&replSet, remoteMajorityOpTime, advancedOpTimeHosts);
    insertTopOfOplog(&replSet, startApplyingOpTime, {hosts.at(2)});

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);
    initialStateDocument.setStartApplyingDonorOpTime(startApplyingOpTime);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    taskFp->waitForTimesEntered(timesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to donor node at index 2.
    auto donorHost = hosts[2].toString();
    ASSERT_EQ(donorHost, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(donorHost, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    taskFp->setMode(FailPoint::off, 0);
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_RemoteMajorityOpTimeBehindStartMigrationDonorTimestamp) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime remoteMajorityOpTime(Timestamp(5, 1), 1);
    const Timestamp startMigrationDonorTimestamp(6, 1);

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto timesEntered = taskFp->setMode(FailPoint::alwaysOn, 0);

    // Insert the remote majority optime into the oplogs of the first two hosts.
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    const auto hosts = replSet.getHosts();
    const std::vector<HostAndPort> advancedOpTimeHosts = {hosts[0], hosts[1]};

    insertTopOfOplog(&replSet, remoteMajorityOpTime, advancedOpTimeHosts);
    insertTopOfOplog(&replSet, OpTime(startMigrationDonorTimestamp, 1), {hosts.at(2)});

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        startMigrationDonorTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    taskFp->waitForTimesEntered(timesEntered + 1);

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to donor node at index 2.
    auto donorHost = hosts[2].toString();
    ASSERT_EQ(donorHost, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(donorHost, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());

    // Since we were using primaryPreferred read preference, we should have tried hosts[0]
    // and had it rejected.
    const auto& excludedHosts = instance->getExcludedDonorHosts_forTest();
    ASSERT_TRUE(std::find_if(excludedHosts.begin(),
                             excludedHosts.end(),
                             [hosts](const std::pair<HostAndPort, Date_t>& a) {
                                 return a.first == hosts[0];
                             }) != excludedHosts.end());

    taskFp->setMode(FailPoint::off, 0);
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_PrimaryFailsOver) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    // Primary is unavailable.
    replSet.kill(replSet.getHosts()[0].toString());
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_BadConnectString) {
    stopFailPointEnableBlock fp("fpAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "broken,connect,string,no,set,name",
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartFetchingDonorOpTime());
    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingDonorOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientGetStartOpTime_Transaction) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime txnStartOpTime(Timestamp(3, 1), 1);
    const OpTime txnLastWriteOpTime(Timestamp(4, 1), 1);
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
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
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    ASSERT_EQ(txnStartOpTime, getStateDoc(instance.get()).getStartFetchingDonorOpTime());
    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingDonorOpTime());
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTimes_RemoteOplogQueryFails) {
    stopFailPointEnableBlock fp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.  Fail to populate the remote oplog mock.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

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
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientStartsCloner) {
    stopFailPointEnableBlock fp("fpBeforeFetchingCommittedTransactions");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    ScopeGuard taskFpGuard([&taskFp] { taskFp->setMode(FailPoint::off); });

    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
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
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogFetcherFailsDuringOplogApplication) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogFetcherResumesFromTopOfOplogBuffer) {
    const UUID migrationUUID = UUID::gen();
    const OpTime initialOpTime(Timestamp(1, 1), 1);
    const OpTime dataConsistentOpTime(Timestamp(4, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, initialOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // We skip cloning here as a way to simulate that the recipient service has detected an existing
    // migration on startup and will resume oplog fetching from the appropriate optime.
    updateStateDocToCloningFinished(
        initialStateDocument, initialOpTime, dataConsistentOpTime, initialOpTime, initialOpTime);

    // Hang after creating the oplog buffer collection but before starting the oplog fetcher.
    const auto hangBeforeFetcherFp =
        globalFailPointRegistry().find("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");
    auto initialTimesEntered = hangBeforeFetcherFp->setMode(FailPoint::alwaysOn,
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

    hangBeforeFetcherFp->waitForTimesEntered(initialTimesEntered + 1);

    const OpTime updatedOpTime(Timestamp(3, 1), 1);
    insertTopOfOplog(&replSet, updatedOpTime);

    const auto oplogBuffer = getDonorOplogBuffer(instance.get());
    OplogBuffer::Batch batch1;
    const OpTime resumeOpTime(Timestamp(2, 1), initialOpTime.getTerm());
    auto resumeOplogBson = makeOplogEntry(resumeOpTime,
                                          OpTypeEnum::kInsert,
                                          NamespaceString("tenantA_foo.bar"),
                                          UUID::gen(),
                                          BSON("doc" << 2),
                                          boost::none /* o2 */)
                               .getEntry()
                               .toBSON();
    batch1.push_back(resumeOplogBson);
    oplogBuffer->push(opCtx.get(), batch1.cbegin(), batch1.cend());
    ASSERT_EQUALS(oplogBuffer->getCount(), 1);

    // Continue the recipient service to hang after starting the oplog applier.
    const auto hangAfterStartingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogApplierMigrationRecipientInstance");
    initialTimesEntered = hangAfterStartingOplogApplier->setMode(FailPoint::alwaysOn,
                                                                 0,
                                                                 BSON("action"
                                                                      << "hang"));
    hangBeforeFetcherFp->setMode(FailPoint::off);
    hangAfterStartingOplogApplier->waitForTimesEntered(initialTimesEntered + 1);

    // The oplog fetcher should exist and be running.
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());
    // The oplog fetcher should have started fetching from resumeOpTime.
    ASSERT_EQUALS(oplogFetcher->getLastOpTimeFetched_forTest(), resumeOpTime);
    ASSERT(oplogFetcher->getStartingPoint_forTest() == OplogFetcher::StartingPoint::kSkipFirstDoc);

    hangAfterStartingOplogApplier->setMode(FailPoint::off);

    // Feed the oplog fetcher the last doc required for us to be considered consistent.
    auto dataConsistentOplogEntry = makeOplogEntry(dataConsistentOpTime,
                                                   OpTypeEnum::kInsert,
                                                   NamespaceString("tenantA_foo.bar"),
                                                   UUID::gen(),
                                                   BSON("doc" << 3),
                                                   boost::none /* o2 */);
    oplogFetcher->receiveBatch(
        1, {dataConsistentOplogEntry.getEntry().toBSON()}, dataConsistentOpTime.getTimestamp());

    LOGV2(5272308,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();
    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogFetcherNoDocInBufferToResumeFrom) {
    const UUID migrationUUID = UUID::gen();
    const OpTime startFetchingOpTime(Timestamp(2, 1), 1);
    const OpTime clonerFinishedOpTime(Timestamp(3, 1), 1);
    const OpTime resumeFetchingOpTime(Timestamp(4, 1), 1);
    const OpTime dataConsistentOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, startFetchingOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // We skip cloning here as a way to simulate that the recipient service has detected an existing
    // migration on startup and will attempt to resume oplog fetching from the appropriate optime.
    updateStateDocToCloningFinished(initialStateDocument,
                                    clonerFinishedOpTime /* clonerFinishedRecipientOpTime */,
                                    dataConsistentOpTime /* dataConsistentStopDonorOpTime */,
                                    startFetchingOpTime /* startApplyingDonorOpTime */,
                                    startFetchingOpTime /* startFetchingDonorOpTime */);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;

    // Hang after creating the oplog buffer collection but before starting the oplog fetcher.
    const auto hangBeforeFetcherFp =
        globalFailPointRegistry().find("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");
    auto initialTimesEntered = hangBeforeFetcherFp->setMode(FailPoint::alwaysOn,
                                                            0,
                                                            BSON("action"
                                                                 << "hang"));
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    hangBeforeFetcherFp->waitForTimesEntered(initialTimesEntered + 1);

    // There are no documents in the oplog buffer to resume fetching from.
    const auto oplogBuffer = getDonorOplogBuffer(instance.get());
    ASSERT_EQUALS(oplogBuffer->getCount(), 0);

    // Continue and hang before starting the oplog applier.
    const auto hangAfterStartingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogApplierMigrationRecipientInstance");
    initialTimesEntered = hangAfterStartingOplogApplier->setMode(FailPoint::alwaysOn,
                                                                 0,
                                                                 BSON("action"
                                                                      << "hang"));
    hangBeforeFetcherFp->setMode(FailPoint::off);
    hangAfterStartingOplogApplier->waitForTimesEntered(initialTimesEntered + 1);

    // The oplog fetcher should exist and be running.
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());
    // The oplog fetcher should have started fetching from 'startFetchingOpTime'. Since no document
    // was found in the oplog buffer, we should have set the 'StartingPoint' to 'kEnqueueFirstDoc'.
    ASSERT_EQUALS(oplogFetcher->getLastOpTimeFetched_forTest(), startFetchingOpTime);
    ASSERT(oplogFetcher->getStartingPoint_forTest() ==
           OplogFetcher::StartingPoint::kEnqueueFirstDoc);

    // Feed the oplog fetcher the last doc required for the recipient to be considered consistent.
    const auto tenantNss = NamespaceString("tenantA_foo.bar");
    auto resumeFetchingOplogEntry = makeOplogEntry(resumeFetchingOpTime,
                                                   OpTypeEnum::kInsert,
                                                   tenantNss,
                                                   UUID::gen(),
                                                   BSON("doc" << 1),
                                                   boost::none /* o2 */);
    auto dataConsistentOplogEntry = makeOplogEntry(dataConsistentOpTime,
                                                   OpTypeEnum::kInsert,
                                                   tenantNss,
                                                   UUID::gen(),
                                                   BSON("doc" << 3),
                                                   boost::none /* o2 */);
    oplogFetcher->receiveBatch(1,
                               {resumeFetchingOplogEntry.getEntry().toBSON(),
                                dataConsistentOplogEntry.getEntry().toBSON()},
                               dataConsistentOpTime.getTimestamp());

    // Allow the service to continue.
    hangAfterStartingOplogApplier->setMode(FailPoint::off);
    LOGV2(5272310,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();
    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogApplierResumesFromLastNoOpOplogEntry) {
    const UUID migrationUUID = UUID::gen();
    // Recipient opTimes
    const OpTime clonerFinishedOpTime(Timestamp(1, 1), 1);
    // Donor opTimes
    const OpTime earlierThanResumeOpTime(Timestamp(2, 1), 1);
    const OpTime resumeOpTime(Timestamp(3, 1), 1);
    const OpTime dataConsistentOpTime(Timestamp(4, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, clonerFinishedOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // We skip cloning here as a way to simulate that the recipient service has detected an existing
    // migration on startup and will attempt to resume oplog fetching from the appropriate optime.
    updateStateDocToCloningFinished(initialStateDocument,
                                    clonerFinishedOpTime /* cloneFinishedRecipientOpTime */,
                                    dataConsistentOpTime /* dataConsistentStopDonorOpTime */,
                                    clonerFinishedOpTime /* startApplyingDonorOpTime */,
                                    clonerFinishedOpTime /* startFetchingDonorOpTime */);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;

    // Hang before reading oplog.
    const auto hangAfterStartingOplogFetcher =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    hangAfterStartingOplogFetcher->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    // Hang before starting the oplog applier.
    const auto hangAfterStartingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogApplierMigrationRecipientInstance");
    auto initialTimesEntered = hangAfterStartingOplogApplier->setMode(FailPoint::alwaysOn,
                                                                      0,
                                                                      BSON("action"
                                                                           << "hang"));

    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }
    // Create and insert two tenant migration no-op entries into the oplog. The oplog applier should
    // resume from the no-op entry with the most recent donor opTime.
    const auto insertNss = NamespaceString("tenantA_foo.bar");
    const auto earlierOplogBson = makeOplogEntry(earlierThanResumeOpTime,
                                                 OpTypeEnum::kInsert,
                                                 insertNss,
                                                 UUID::gen(),
                                                 BSON("doc" << 1),
                                                 boost::none /* o2 */)
                                      .getEntry()
                                      .toBSON();
    const auto resumeOplogBson = makeOplogEntry(resumeOpTime,
                                                OpTypeEnum::kInsert,
                                                insertNss,
                                                UUID::gen(),
                                                BSON("doc" << 2),
                                                boost::none /* o2 */)
                                     .getEntry()
                                     .toBSON();
    auto storage = StorageInterface::get(opCtx->getServiceContext());
    const auto oplogNss = NamespaceString::kRsOplogNamespace;
    const OpTime earlierRecipientOpTime(Timestamp(9, 1), 1);
    const OpTime resumeRecipientOpTime(Timestamp(10, 1), 1);
    auto earlierNoOpEntry = makeNoOpOplogEntry(earlierRecipientOpTime,
                                               insertNss,
                                               UUID::gen(),
                                               earlierOplogBson,
                                               instance->getMigrationUUID());
    auto resumeNoOpEntry = makeNoOpOplogEntry(resumeRecipientOpTime,
                                              insertNss,
                                              UUID::gen(),
                                              resumeOplogBson,
                                              instance->getMigrationUUID());
    ASSERT_OK(
        storage->insertDocument(opCtx.get(),
                                oplogNss,
                                {earlierNoOpEntry.toBSON(), earlierRecipientOpTime.getTimestamp()},
                                earlierRecipientOpTime.getTerm()));
    ASSERT_OK(
        storage->insertDocument(opCtx.get(),
                                oplogNss,
                                {resumeNoOpEntry.toBSON(), resumeRecipientOpTime.getTimestamp()},
                                resumeRecipientOpTime.getTerm()));

    hangAfterStartingOplogFetcher->setMode(FailPoint::off);
    hangAfterStartingOplogApplier->waitForTimesEntered(initialTimesEntered + 1);

    auto oplogFetcher = getDonorOplogFetcher(instance.get());
    auto dataConsistentOplogEntry = makeOplogEntry(dataConsistentOpTime,
                                                   OpTypeEnum::kInsert,
                                                   insertNss,
                                                   UUID::gen(),
                                                   BSON("doc" << 3),
                                                   boost::none /* o2 */);
    // Feed the oplog fetcher the last doc required for the recipient to be considered consistent.
    oplogFetcher->receiveBatch(
        1, {dataConsistentOplogEntry.getEntry().toBSON()}, dataConsistentOpTime.getTimestamp());

    // Allow the service to continue.
    hangAfterStartingOplogApplier->setMode(FailPoint::off);
    LOGV2(5272350,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    // The oplog applier should have started batching and applying at the donor opTime equal to
    // 'resumeOpTime'.
    const auto oplogApplier = getTenantOplogApplier(instance.get());
    ASSERT_EQUALS(resumeOpTime, oplogApplier->getStartApplyingAfterOpTime());
    ASSERT_EQUALS(resumeOpTime.getTimestamp(), oplogApplier->getResumeBatchingTs());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();
    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       OplogApplierResumesBatchingAndApplyingAtDifferentTimestamps) {
    const UUID migrationUUID = UUID::gen();
    // Donor opTimes
    const OpTime startApplyingOpTime(Timestamp(2, 1), 1);
    const OpTime dataConsistentOpTime(Timestamp(4, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, startApplyingOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // We skip cloning here as a way to simulate that the recipient service has detected an existing
    // migration on startup and will attempt to resume oplog fetching from the appropriate optime.
    updateStateDocToCloningFinished(initialStateDocument,
                                    OpTime(Timestamp(10, 1), 1) /* cloneFinishedRecipientOpTime
                                                                 */
                                    ,
                                    dataConsistentOpTime /* dataConsistentStopDonorOpTime */,
                                    startApplyingOpTime /* startApplyingDonorOpTime */,
                                    startApplyingOpTime /* startFetchingDonorOpTime */);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;

    // Hang before creating the oplog applier.
    const auto hangBeforeCreatingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    hangBeforeCreatingOplogApplier->setMode(FailPoint::alwaysOn,
                                            0,
                                            BSON("action"
                                                 << "hang"));
    // Hang after starting the oplog applier.
    const auto hangAfterStartingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogApplierMigrationRecipientInstance");
    auto initialTimesEntered = hangAfterStartingOplogApplier->setMode(FailPoint::alwaysOn,
                                                                      0,
                                                                      BSON("action"
                                                                           << "hang"));

    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    // Create and insert the following into the oplog:
    // - (1) An oplog entry with opTime earlier than 'cloneFinishedRecipientOpTime'.
    // - (2) An oplog entry with opTime greater than 'cloneFinishedRecipientOpTime'.
    // - (3) A no-op oplog entry with an inner donor oplog entry as the 'o2' field. The donor opTime
    //       is less than the 'startApplyingDonorOpTime'. We will resume batching from this
    //       timestamp.
    // - (4) A no-op oplog entry with an inner oplog entry as the 'o2' field but no
    //       'fromTenantMigrate' field. This oplog entry does not satisfy the conditions
    //       for the oplog applier to resume applying from so we default to apply from
    //       'startDonorApplyingOpTime'.
    const auto insertNss = NamespaceString("tenantA_foo.bar");
    const auto beforeStartApplyingOpTime = OpTime(Timestamp(1, 1), 1);
    const auto entryBeforeStartApplyingOpTime = makeOplogEntry(
                                                    beforeStartApplyingOpTime,
                                                    OpTypeEnum::kInsert,
                                                    insertNss,
                                                    UUID::gen(),
                                                    BSON("doc"
                                                         << "before startApplyingDonorOpTime"),
                                                    boost::none /* o2 */)
                                                    .getEntry()
                                                    .toBSON();
    const auto afterStartApplyingOpTime = OpTime(Timestamp(3, 1), 1);
    const auto entryAfterStartApplyingOpTime = makeOplogEntry(
                                                   afterStartApplyingOpTime,
                                                   OpTypeEnum::kInsert,
                                                   insertNss,
                                                   UUID::gen(),
                                                   BSON("doc"
                                                        << "after startApplyingDonorOpTime"),
                                                   boost::none /* o2 */)
                                                   .getEntry()
                                                   .toBSON();
    auto storage = StorageInterface::get(opCtx->getServiceContext());
    const auto oplogNss = NamespaceString::kRsOplogNamespace;
    const auto collUuid = UUID::gen();
    std::vector<DurableOplogEntry> oplogEntries;
    std::vector<MutableOplogEntry> noOpEntries;
    // (1)
    oplogEntries.push_back(makeOplogEntry(OpTime(Timestamp(9, 1), 1),
                                          OpTypeEnum::kInsert,
                                          insertNss,
                                          collUuid,
                                          BSON("doc"
                                               << "before clonerFinishedOpTime"),
                                          boost::none /* o2 */)
                               .getEntry());
    // (2)
    oplogEntries.push_back(makeOplogEntry(OpTime(Timestamp(11, 1), 1),
                                          OpTypeEnum::kInsert,
                                          insertNss,
                                          collUuid,
                                          BSON("doc"
                                               << "after clonerFinishedOpTime"),
                                          boost::none /* o2 */)
                               .getEntry());
    // (3)
    noOpEntries.push_back(makeNoOpOplogEntry(OpTime(Timestamp(12, 1), 1),
                                             insertNss,
                                             collUuid,
                                             entryBeforeStartApplyingOpTime,
                                             instance->getMigrationUUID()));
    // (4)
    noOpEntries.push_back(makeNoOpOplogEntry(OpTime(Timestamp(13, 1), 1),
                                             insertNss,
                                             collUuid,
                                             entryAfterStartApplyingOpTime,
                                             boost::none /* migrationUUID */));
    for (auto entry : oplogEntries) {
        auto opTime = entry.getOpTime();
        ASSERT_OK(storage->insertDocument(
            opCtx.get(), oplogNss, {entry.toBSON(), opTime.getTimestamp()}, opTime.getTerm()));
    }
    for (auto entry : noOpEntries) {
        auto opTime = entry.getOpTime();
        ASSERT_OK(storage->insertDocument(
            opCtx.get(), oplogNss, {entry.toBSON(), opTime.getTimestamp()}, opTime.getTerm()));
    }
    // Move on to the next failpoint to hang after starting the oplog applier.
    hangBeforeCreatingOplogApplier->setMode(FailPoint::off);
    hangAfterStartingOplogApplier->waitForTimesEntered(initialTimesEntered + 1);

    auto dataConsistentOplogEntry = makeOplogEntry(dataConsistentOpTime,
                                                   OpTypeEnum::kInsert,
                                                   NamespaceString("tenantA_foo.bar"),
                                                   UUID::gen(),
                                                   BSON("doc" << 3),
                                                   boost::none /* o2 */);

    auto oplogFetcher = getDonorOplogFetcher(instance.get());
    // Feed the oplog fetcher the last doc required for the recipient to be considered consistent.
    oplogFetcher->receiveBatch(
        1, {dataConsistentOplogEntry.getEntry().toBSON()}, dataConsistentOpTime.getTimestamp());

    // Allow the service to continue.
    hangAfterStartingOplogApplier->setMode(FailPoint::off);
    LOGV2(5272340,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    const auto oplogApplier = getTenantOplogApplier(instance.get());
    // Resume batching from the first migration no-op oplog entry. In this test, this is before
    // the 'startApplyingDonorOpTime'.
    ASSERT_EQUALS(beforeStartApplyingOpTime.getTimestamp(), oplogApplier->getResumeBatchingTs());
    // The oplog applier starts applying from the donor opTime equal to 'beginApplyingOpTime'.
    ASSERT_EQUALS(startApplyingOpTime, oplogApplier->getStartApplyingAfterOpTime());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();
    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogApplierResumesFromStartDonorApplyingOpTime) {
    const UUID migrationUUID = UUID::gen();
    // Donor opTimes
    const OpTime startApplyingOpTime(Timestamp(2, 1), 1);
    const OpTime dataConsistentOpTime(Timestamp(4, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, startApplyingOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // We skip cloning here as a way to simulate that the recipient service has detected an existing
    // migration on startup and will attempt to resume oplog fetching from the appropriate optime.
    updateStateDocToCloningFinished(initialStateDocument,
                                    OpTime(Timestamp(10, 1), 1) /* cloneFinishedRecipientOpTime
                                                                 */
                                    ,
                                    dataConsistentOpTime /* dataConsistentStopDonorOpTime */,
                                    startApplyingOpTime /* startApplyingDonorOpTime */,
                                    startApplyingOpTime /* startFetchingDonorOpTime */);

    auto opCtx = makeOperationContext();
    std::shared_ptr<TenantMigrationRecipientService::Instance> instance;

    // Hang before starting the oplog applier.
    const auto hangAfterStartingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogApplierMigrationRecipientInstance");
    auto initialTimesEntered = hangAfterStartingOplogApplier->setMode(FailPoint::alwaysOn,
                                                                      0,
                                                                      BSON("action"
                                                                           << "hang"));

    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        // Create and start the instance.
        instance = TenantMigrationRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    // Create and insert the following into the oplog:
    // - (1) An oplog entry with opTime earlier than 'cloneFinishedRecipientOpTime'.
    // - (2) An oplog entry with opTime greater than 'cloneFinishedRecipientOpTime'.
    // - (3) A no-op oplog entry with an inner oplog entry as the 'o2' field but no
    //       'fromTenantMigrate' field. This oplog entry does not satisfy the conditions
    //       for the oplog applier to resume applying from so we default to applying and
    //       batching from the start of the buffer collection.
    const auto insertNss = NamespaceString("tenantA_foo.bar");
    const auto afterStartApplyingOpTime = OpTime(Timestamp(3, 1), 1);
    const auto entryAfterStartApplyingOpTime = makeOplogEntry(
                                                   afterStartApplyingOpTime,
                                                   OpTypeEnum::kInsert,
                                                   insertNss,
                                                   UUID::gen(),
                                                   BSON("doc"
                                                        << "after startApplyingDonorOpTime"),
                                                   boost::none /* o2 */)
                                                   .getEntry()
                                                   .toBSON();
    auto storage = StorageInterface::get(opCtx->getServiceContext());
    const auto oplogNss = NamespaceString::kRsOplogNamespace;
    const auto collUuid = UUID::gen();
    std::vector<DurableOplogEntry> oplogEntries;
    std::vector<MutableOplogEntry> noOpEntries;
    // (1)
    oplogEntries.push_back(makeOplogEntry(OpTime(Timestamp(9, 1), 1),
                                          OpTypeEnum::kInsert,
                                          insertNss,
                                          collUuid,
                                          BSON("doc"
                                               << "before clonerFinishedOpTime"),
                                          boost::none /* o2 */)
                               .getEntry());
    // (2)
    oplogEntries.push_back(makeOplogEntry(OpTime(Timestamp(11, 1), 1),
                                          OpTypeEnum::kInsert,
                                          insertNss,
                                          collUuid,
                                          BSON("doc"
                                               << "after clonerFinishedOpTime"),
                                          boost::none /* o2 */)
                               .getEntry());
    // (3)
    const auto laterOpTime = OpTime(Timestamp(13, 1), 1);
    const auto noOpEntry = makeNoOpOplogEntry(laterOpTime,
                                              insertNss,
                                              collUuid,
                                              entryAfterStartApplyingOpTime,
                                              boost::none /* migrationUUID */);

    for (auto entry : oplogEntries) {
        auto opTime = entry.getOpTime();
        ASSERT_OK(storage->insertDocument(
            opCtx.get(), oplogNss, {entry.toBSON(), opTime.getTimestamp()}, opTime.getTerm()));
    }
    ASSERT_OK(storage->insertDocument(opCtx.get(),
                                      oplogNss,
                                      {noOpEntry.toBSON(), laterOpTime.getTimestamp()},
                                      laterOpTime.getTerm()));

    hangAfterStartingOplogApplier->waitForTimesEntered(initialTimesEntered + 1);

    auto dataConsistentOplogEntry = makeOplogEntry(dataConsistentOpTime,
                                                   OpTypeEnum::kInsert,
                                                   NamespaceString("tenantA_foo.bar"),
                                                   UUID::gen(),
                                                   BSON("doc" << 3),
                                                   boost::none /* o2 */);

    auto oplogFetcher = getDonorOplogFetcher(instance.get());
    // Feed the oplog fetcher the last doc required for the recipient to be considered consistent.
    oplogFetcher->receiveBatch(
        1, {dataConsistentOplogEntry.getEntry().toBSON()}, dataConsistentOpTime.getTimestamp());

    // Allow the service to continue.
    hangAfterStartingOplogApplier->setMode(FailPoint::off);
    LOGV2(5394602,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    const auto oplogApplier = getTenantOplogApplier(instance.get());
    // There is no oplog entry to resume batching from, so we treat it as if we are resuming
    // oplog application from the start. The 'resumeBatchingTs' will be a null timestamp.
    ASSERT_EQUALS(Timestamp(), oplogApplier->getResumeBatchingTs());
    // The oplog applier starts applying from the donor opTime equal to 'beginApplyingOpTime'.
    ASSERT_EQUALS(startApplyingOpTime, oplogApplier->getStartApplyingAfterOpTime());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();
    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       OplogFetcherResumesFromStartFetchingOpTimeWithDocInBuffer) {
    const UUID migrationUUID = UUID::gen();
    const OpTime startFetchingOpTime(Timestamp(2, 1), 1);
    const OpTime dataConsistentOpTime(Timestamp(4, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, startFetchingOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // We skip cloning here as a way to simulate that the recipient service has detected an existing
    // migration on startup and will resume oplog fetching from the appropriate optime.
    updateStateDocToCloningFinished(initialStateDocument,
                                    startFetchingOpTime,
                                    dataConsistentOpTime,
                                    startFetchingOpTime,
                                    startFetchingOpTime);

    // Hang after creating the oplog buffer collection but before starting the oplog fetcher.
    const auto hangBeforeFetcherFp =
        globalFailPointRegistry().find("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");
    auto initialTimesEntered = hangBeforeFetcherFp->setMode(FailPoint::alwaysOn,
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

    hangBeforeFetcherFp->waitForTimesEntered(initialTimesEntered + 1);

    // Insert the first document with 'startFetchingOpTime' into the oplog buffer. The fetcher
    // should know to skip this document on service restart.
    const auto oplogBuffer = getDonorOplogBuffer(instance.get());
    OplogBuffer::Batch batch1;
    batch1.push_back(makeOplogEntry(startFetchingOpTime,
                                    OpTypeEnum::kInsert,
                                    NamespaceString("tenantA_foo.bar"),
                                    UUID::gen(),
                                    BSON("doc" << 2),
                                    boost::none /* o2 */)
                         .getEntry()
                         .toBSON());
    oplogBuffer->push(opCtx.get(), batch1.cbegin(), batch1.cend());
    ASSERT_EQUALS(oplogBuffer->getCount(), 1);

    auto dataConsistentOplogEntry = makeOplogEntry(dataConsistentOpTime,
                                                   OpTypeEnum::kInsert,
                                                   NamespaceString("tenantA_foo.bar"),
                                                   UUID::gen(),
                                                   BSON("doc" << 3),
                                                   boost::none /* o2 */);
    // Continue the recipient service to hang before starting the oplog applier.
    const auto hangAfterStartingOplogApplier =
        globalFailPointRegistry().find("fpAfterStartingOplogApplierMigrationRecipientInstance");
    initialTimesEntered = hangAfterStartingOplogApplier->setMode(FailPoint::alwaysOn,
                                                                 0,
                                                                 BSON("action"
                                                                      << "hang"));
    hangBeforeFetcherFp->setMode(FailPoint::off);
    hangAfterStartingOplogApplier->waitForTimesEntered(initialTimesEntered + 1);

    // The oplog fetcher should exist and be running.
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());
    // The oplog fetcher should have started fetching from 'startFetchingOpTime'. However, the
    // fetcher should skip the first doc from being fetched since it already exists in the buffer.
    ASSERT_EQUALS(oplogFetcher->getLastOpTimeFetched_forTest(), startFetchingOpTime);
    ASSERT(oplogFetcher->getStartingPoint_forTest() == OplogFetcher::StartingPoint::kSkipFirstDoc);

    // Feed the oplog fetcher the last doc required for us to be considered consistent.
    oplogFetcher->receiveBatch(
        1, {dataConsistentOplogEntry.getEntry().toBSON()}, dataConsistentOpTime.getTimestamp());

    // Allow the service to continue.
    hangAfterStartingOplogApplier->setMode(FailPoint::off);
    LOGV2(5272317,
          "Waiting for recipient service to reach consistent state",
          "suite"_attr = _agent.getSuiteName(),
          "test"_attr = _agent.getTestName());
    instance->waitUntilMigrationReachesConsistentState(opCtx.get());

    // Stop the oplog applier.
    instance->stopOplogApplier_forTest();
    // Wait for task completion.  Since we're using a test function to cancel the applier,
    // the actual result is not critical.
    ASSERT_NOT_OK(instance->getDataSyncCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, OplogApplierFails) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);
    const OpTime injectedEntryOpTime(Timestamp(6, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, StoppingApplierAllowsCompletion) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientAddResumeTokenNoopsToBuffer) {
    stopFailPointEnableBlock fp("fpBeforeFetchingCommittedTransactions");
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
        ASSERT_EQUALS(noopEntry.getTerm().value(), -1);
        ASSERT_EQUALS(noopEntry.getNss(), NamespaceString(""));
    }

    ASSERT_TRUE(oplogBuffer->isEmpty());

    // Let the recipient service complete.
    oplogFetcherFP->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_BeforeRun) {
    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_EQ(instance->getForgetMigrationDurableFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientForgetMigration_FailToInitializeStateDoc) {
    stopFailPointEnableBlock fp("failWhilePersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    auto instance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());

    ASSERT_THROWS_CODE(instance->onReceiveRecipientForgetMigration(opCtx.get()),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
    // We should fail to mark the state doc garbage collectable if we have failed to initialize and
    // persist the state doc at the first place.
    ASSERT_EQ(instance->getForgetMigrationDurableFuture().getNoThrow(),
              ErrorCodes::NotWritablePrimary);
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
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

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
    ASSERT_TRUE(doc.getExpireAt().value() >
                opCtx->getServiceContext()->getFastClockSource()->now());
    ASSERT_TRUE(doc.getStartApplyingDonorOpTime() == boost::none);
    ASSERT_TRUE(doc.getStartFetchingDonorOpTime() == boost::none);
    ASSERT_TRUE(doc.getDataConsistentStopDonorOpTime() == boost::none);
    ASSERT_TRUE(doc.getCloneFinishedRecipientOpTime() == boost::none);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
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
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

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
    ASSERT_TRUE(doc.getExpireAt().value() >
                opCtx->getServiceContext()->getFastClockSource()->now());
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
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

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

    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

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
        ASSERT_TRUE(doc.getExpireAt().value() >
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

    stopFailPointEnableBlock fp("fpBeforeFetchingCommittedTransactions");
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

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
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

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
        ASSERT_TRUE(doc.getExpireAt().value() >
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
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

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
    ASSERT_EQ(ErrorCodes::NotWritablePrimary,
              instance->getForgetMigrationDurableFuture().getNoThrow().code());

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

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientServiceRecordsFCVAtStart) {
    stopFailPointEnableBlock fp("fpAfterRecordingRecipientPrimaryStartingFCV");

    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    auto doc = getStateDoc(instance.get());
    auto docFCV = doc.getRecipientPrimaryStartingFCV();
    auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();
    LOGV2(5356202, "FCV in doc vs current", "docFCV"_attr = docFCV, "currentFCV"_attr = currentFCV);
    ASSERT(currentFCV == docFCV);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientServiceAlreadyRecordedFCV_Match) {
    stopFailPointEnableBlock fp("fpAfterRecordingRecipientPrimaryStartingFCV");

    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Add an FCV value as if it was from a previous attempt.
    auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();
    initialStateDocument.setRecipientPrimaryStartingFCV(currentFCV);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion.
    // The FCV should match so we should exit with the failpoint code rather than an error.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    auto doc = getStateDoc(instance.get());
    auto docFCV = doc.getRecipientPrimaryStartingFCV();
    LOGV2(5356203, "FCV in doc vs current", "docFCV"_attr = docFCV, "currentFCV"_attr = currentFCV);
    ASSERT(currentFCV == docFCV);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientServiceAlreadyRecordedFCV_Mismatch) {
    stopFailPointEnableBlock fp("fpAfterRecordingRecipientPrimaryStartingFCV");

    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Add an FCV value as if it was from a previous attempt, making sure we set a different
    // version from the one we currently have.
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    initialStateDocument.setRecipientPrimaryStartingFCV(multiversion::GenericFCV::kLastLTS);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion failure.
    // The FCV should differ so we expect to exit with an error.
    std::int32_t expectedCode = 5356201;
    ASSERT_EQ(expectedCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientServiceDonorAndRecipientFCVMismatch) {
    stopFailPointEnableBlock fp("fpAfterComparingRecipientAndDonorFCV");

    // Tests skip this check by default but we are specifically testing it here.
    auto compFp = globalFailPointRegistry().find("skipComparingRecipientAndDonorFCV");
    compFp->setMode(FailPoint::off);

    // Set to allow the donor to respond to FCV requests.
    auto connFp =
        globalFailPointRegistry().find("fpAfterConnectingTenantMigrationRecipientInstance");
    auto initialTimesEntered = connFp->setMode(FailPoint::alwaysOn,
                                               0,
                                               BSON("action"
                                                    << "hang"));

    const UUID migrationUUID = UUID::gen();
    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(kDefaultStartMigrationTimestamp, 1));

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
    auto opCtx = makeOperationContext();
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Set the donor FCV to be different from 'latest'.
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    connFp->waitForTimesEntered(initialTimesEntered + 1);
    setDonorFCV(instance.get(), multiversion::GenericFCV::kLastContinuous);
    connFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    // The FCVs should differ so we expect to exit with an error.
    std::int32_t expectedCode = 5382301;
    ASSERT_EQ(expectedCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(TenantMigrationRecipientServiceTest, WaitUntilMigrationReachesReturnAfterReachingTimestamp) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

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

    instance->waitUntilMigrationReachesConsistentState(opCtx.get());
    checkStateDocPersisted(opCtx.get(), instance.get());

    // Simulate recipient receiving a donor timestamp.
    auto returnAfterReachingTimestamp =
        ReplicationCoordinator::get(getServiceContext())->getMyLastAppliedOpTime().getTimestamp() +
        1;
    const OpTime newOpTime(
        returnAfterReachingTimestamp,
        ReplicationCoordinator::get(getServiceContext())->getMyLastAppliedOpTime().getTerm());

    instance->waitUntilMigrationReachesReturnAfterReachingTimestamp(opCtx.get(),
                                                                    returnAfterReachingTimestamp);

    auto lastAppliedOpTime =
        ReplicationCoordinator::get(getServiceContext())->getMyLastAppliedOpTime();
    ASSERT_GTE(lastAppliedOpTime, newOpTime);
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientReceivesRetriableFetcherError) {
    stopFailPointEnableBlock stopFp("fpBeforeFetchingCommittedTransactions");
    auto fp =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
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

    fp->waitForTimesEntered(initialTimesEntered + 1);
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    auto doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    // Kill the oplog fetcher with a retriable error and wait for the migration to retry.
    const auto retriableErrorCode = ErrorCodes::SocketException;
    ASSERT_TRUE(ErrorCodes::isRetriableError(retriableErrorCode));
    oplogFetcher->shutdownWith({retriableErrorCode, "Injected retriable error"});

    // Skip the cloners in this test, so we provide an empty list of databases.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());

    fp->setMode(FailPoint::off);
    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 1);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientReceivesNonRetriableFetcherError) {
    auto fp =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
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

    fp->waitForTimesEntered(initialTimesEntered + 1);
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    auto doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    // Kill the oplog fetcher with a non-retriable error.
    const auto nonRetriableErrorCode = ErrorCodes::Error(5271901);
    ASSERT_FALSE(ErrorCodes::isRetriableError(nonRetriableErrorCode));
    oplogFetcher->shutdownWith({nonRetriableErrorCode, "Injected non-retriable error"});

    fp->setMode(FailPoint::off);
    // Wait for task completion failure.
    auto status = instance->getDataSyncCompletionFuture().getNoThrow();
    ASSERT_EQ(nonRetriableErrorCode, status.code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientWillNotRetryOnExternalInterrupt) {
    auto fp =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
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

    fp->waitForTimesEntered(initialTimesEntered + 1);
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    auto doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_TRUE(ErrorCodes::isRetriableError(ErrorCodes::SocketException));
    // Interrupt the task with 'skipWaitingForForgetMigration' = true.
    instance->interrupt(
        {ErrorCodes::SocketException, "Test retriable error with external interrupt"});

    fp->setMode(FailPoint::off);
    // Wait for task completion failure.
    ASSERT_EQ(instance->getForgetMigrationDurableFuture().getNoThrow(),
              ErrorCodes::SocketException);

    doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientWillNotRetryOnReceivingForgetMigrationCmd) {
    auto hangAfterStartingOplogFetcherFp =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto hangAfterStartingOplogFetcherFpTimesEntered =
        hangAfterStartingOplogFetcherFp->setMode(FailPoint::alwaysOn,
                                                 0,
                                                 BSON("action"
                                                      << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
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

    hangAfterStartingOplogFetcherFp->waitForTimesEntered(
        hangAfterStartingOplogFetcherFpTimesEntered + 1);
    auto oplogFetcher = checked_cast<OplogFetcherMock*>(getDonorOplogFetcher(instance.get()));
    ASSERT_TRUE(oplogFetcher != nullptr);
    ASSERT_TRUE(oplogFetcher->isActive());

    // Hang the migration before it attempts to retry.
    auto hangMigrationBeforeRetryCheckFp =
        globalFailPointRegistry().find("hangMigrationBeforeRetryCheck");
    auto hangMigrationBeforeRetryCheckFpTimesEntered =
        hangMigrationBeforeRetryCheckFp->setMode(FailPoint::alwaysOn);

    // Make oplog fetcher to fail with a retryable error which will interrupt the migration.
    ASSERT_TRUE(ErrorCodes::isRetriableError(ErrorCodes::SocketException));
    oplogFetcher->shutdownWith({ErrorCodes::SocketException, "Injected retryable error"});
    hangAfterStartingOplogFetcherFp->setMode(FailPoint::off);

    hangMigrationBeforeRetryCheckFp->waitForTimesEntered(
        hangMigrationBeforeRetryCheckFpTimesEntered + 1);

    // After the migration is interrupted successfully, signal migration that we received
    // recipientForgetMigration command. And, that should make the migration not to retry
    // on retryable error.
    instance->onReceiveRecipientForgetMigration(opCtx.get());
    hangMigrationBeforeRetryCheckFp->setMode(FailPoint::off);

    // Wait for task completion failure.
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    auto doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientReceivesRetriableClonerError) {
    stopFailPointEnableBlock stopFp("fpBeforeFetchingCommittedTransactions");
    auto fp =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
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

    fp->waitForTimesEntered(initialTimesEntered + 1);
    auto doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);

    // Have the cloner fail on a retriable error (from the point of view of the recipient service).
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    const auto retriableErrorCode = ErrorCodes::HostUnreachable;
    ASSERT_TRUE(ErrorCodes::isRetriableError(retriableErrorCode));
    _donorServer->setCommandReply("listDatabases",
                                  Status(retriableErrorCode, "Injecting retriable error."));

    auto retryFp =
        globalFailPointRegistry().find("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");
    initialTimesEntered = retryFp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));
    fp->setMode(FailPoint::off);

    retryFp->waitForTimesEntered(initialTimesEntered + 1);
    // Let cloner run successfully on retry.
    _donorServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
    _donorServer->setCommandReply("find", makeFindResponse());
    retryFp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 1);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, RecipientReceivesNonRetriableClonerError) {
    stopFailPointEnableBlock stopFp("fpBeforeFetchingCommittedTransactions");
    auto fp =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    // Create and start the instance.
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

    fp->waitForTimesEntered(initialTimesEntered + 1);
    auto doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);

    // Have the cloner fail on a non-retriable error.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    const auto nonRetriableErrorCode = ErrorCodes::Error(5271902);
    ASSERT_FALSE(ErrorCodes::isRetriableError(nonRetriableErrorCode));
    _donorServer->setCommandReply("listDatabases",
                                  Status(nonRetriableErrorCode, "Injecting non-retriable error."));

    fp->setMode(FailPoint::off);

    // Wait for task completion.
    ASSERT_EQ(nonRetriableErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    doc = getStateDoc(instance.get());
    ASSERT_EQ(doc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(doc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, IncrementNumRestartsDueToRecipientFailureCounter) {
    stopFailPointEnableBlock fp("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(1, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);
    // Starting a migration where the state is not 'kUninitialized' indicates that we are restarting
    // from failover.
    initialStateDocument.setState(TenantMigrationRecipientStateEnum::kStarted);
    ASSERT_EQ(0, initialStateDocument.getNumRestartsDueToRecipientFailure());

    auto opCtx = makeOperationContext();
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto storage = StorageInterface::get(opCtx->getServiceContext());
    const auto status = storage->createCollection(
        opCtx.get(), NamespaceString::kTenantMigrationRecipientsNamespace, collectionOptions);
    if (!status.isOK()) {
        // It's possible to race with the test fixture setup in creating the tenant recipient
        // collection.
        ASSERT_EQ(ErrorCodes::NamespaceExists, status.code());
    }
    ASSERT_OK(storage->insertDocument(opCtx.get(),
                                      NamespaceString::kTenantMigrationRecipientsNamespace,
                                      {initialStateDocument.toBSON()},
                                      0));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    ASSERT_EQ(stopFailPointErrorCode, instance->getDataSyncCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    const auto stateDoc = getStateDoc(instance.get());
    ASSERT_EQ(stateDoc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(stateDoc.getNumRestartsDueToRecipientFailure(), 1);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       RecipientFailureCounterNotIncrementedWhenMigrationForgotten) {
    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(1, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);
    // Starting a migration where the state is not 'kUninitialized' indicates that we are restarting
    // from failover.
    initialStateDocument.setState(TenantMigrationRecipientStateEnum::kStarted);
    // Set the 'expireAt' field to indicate the migration is garbage collectable.
    auto opCtx = makeOperationContext();
    initialStateDocument.setStartAt(opCtx->getServiceContext()->getFastClockSource()->now());
    initialStateDocument.setExpireAt(opCtx->getServiceContext()->getFastClockSource()->now());
    ASSERT_EQ(0, initialStateDocument.getNumRestartsDueToRecipientFailure());

    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto storage = StorageInterface::get(opCtx->getServiceContext());
    const auto status = storage->createCollection(
        opCtx.get(), NamespaceString::kTenantMigrationRecipientsNamespace, collectionOptions);
    if (!status.isOK()) {
        // It's possible to race with the test fixture setup in creating the tenant recipient
        // collection.
        ASSERT_EQ(ErrorCodes::NamespaceExists, status.code());
    }
    ASSERT_OK(storage->insertDocument(opCtx.get(),
                                      NamespaceString::kTenantMigrationRecipientsNamespace,
                                      {initialStateDocument.toBSON()},
                                      0));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    ASSERT_EQ(ErrorCodes::TenantMigrationForgotten,
              instance->getDataSyncCompletionFuture().getNoThrow().code());

    const auto stateDoc = getStateDoc(instance.get());
    ASSERT_EQ(stateDoc.getNumRestartsDueToDonorConnectionFailure(), 0);
    ASSERT_EQ(stateDoc.getNumRestartsDueToRecipientFailure(), 0);
    checkStateDocPersisted(opCtx.get(), instance.get());
}

#endif
}  // namespace repl
}  // namespace mongo
