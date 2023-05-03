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
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_fetcher_mock.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/mock_network_fixture.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_layer_mock.h"
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

}  // namespace

class ShardMergeRecipientServiceTest : public ServiceContextMongoDTest {
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
            .find("autoRecipientForgetMigrationAbort")
            ->setMode(FailPoint::alwaysOn,
                      0,
                      BSON("state"
                           << "aborted"));

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

            // Add OpObserver needed by subclasses.
            addOpObserver(opObserverRegistry);

            _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
            std::unique_ptr<ShardMergeRecipientService> service =
                std::make_unique<ShardMergeRecipientService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
        }
        stepUp();

        _service = _registry->lookupServiceByName(
            ShardMergeRecipientService::kShardMergeRecipientServiceName);
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

        // setup mock networking that will be use to mock the backup cursor traffic.
        auto net = std::make_unique<executor::NetworkInterfaceMock>();
        _net = net.get();

        executor::ThreadPoolMock::Options dbThreadPoolOptions;
        dbThreadPoolOptions.onCreateThread = []() {
            Client::initThread("FetchMockTaskExecutor");
        };

        auto pool = std::make_unique<executor::ThreadPoolMock>(_net, 1, dbThreadPoolOptions);
        _threadpoolTaskExecutor =
            std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
        _threadpoolTaskExecutor->startup();
    }

    void tearDown() override {
        _threadpoolTaskExecutor->shutdown();
        _threadpoolTaskExecutor->join();

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
    ShardMergeRecipientServiceTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    PrimaryOnlyServiceRegistry* _registry;
    PrimaryOnlyService* _service;
    long long _term = 0;

    bool _collCreated = false;
    size_t _numSecondaryIndexesCreated{0};
    size_t _numDocsInserted{0};

    const TenantId _tenantA{OID::gen()};
    const TenantId _tenantB{OID::gen()};
    const std::vector<TenantId> _tenants{_tenantA, _tenantB};

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
                                const ShardMergeRecipientService::Instance* instance) {
        auto memoryStateDoc = getStateDoc(instance);

        // Read the most up to date data.
        ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);
        AutoGetCollectionForRead collection(opCtx, NamespaceString::kShardMergeRecipientsNamespace);
        ASSERT(collection);

        BSONObj result;
        auto foundDoc = Helpers::findOne(
            opCtx, collection.getCollection(), BSON("_id" << memoryStateDoc.getId()), result);
        ASSERT(foundDoc);

        auto persistedStateDoc =
            ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), result);

        ASSERT_BSONOBJ_EQ(memoryStateDoc.toBSON(), persistedStateDoc.toBSON());
    }
    void insertToNodes(MockReplicaSet* replSet,
                       const NamespaceString& nss,
                       BSONObj obj,
                       const std::vector<HostAndPort>& hosts) {
        for (const auto& host : hosts) {
            replSet->getNode(host.toString())->insert(nss, obj);
        }
    }

    void clearCollection(MockReplicaSet* replSet,
                         const NamespaceString& nss,
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
        clearCollection(replSet, NamespaceString::kRsOplogNamespace, targetHosts);
        insertToNodes(replSet,
                      NamespaceString::kRsOplogNamespace,
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
    DBClientConnection* getClient(const ShardMergeRecipientService::Instance* instance) const {
        return instance->_client.get();
    }

    const ShardMergeRecipientDocument& getStateDoc(
        const ShardMergeRecipientService::Instance* instance) const {
        return instance->_stateDoc;
    }

    sdam::MockTopologyManager* getTopologyManager() {
        return _rsmMonitor.getTopologyManager();
    }

    ClockSource* clock() {
        return &_clkSource;
    }

    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }

    executor::NetworkInterfaceMock* _net = nullptr;
    std::shared_ptr<executor::TaskExecutor> _threadpoolTaskExecutor;

    void setInstanceBackupCursorFetcherExecutor(
        std::shared_ptr<ShardMergeRecipientService::Instance> instance) {
        instance->setBackupCursorFetcherExecutor_forTest(_threadpoolTaskExecutor);
    }

private:
    virtual void addOpObserver(OpObserverRegistry* opObserverRegistry){};

    ClockSourceMock _clkSource;

    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};

    StreamableReplicaSetMonitorForTesting _rsmMonitor;
    RAIIServerParameterControllerForTest _findHostTimeout{"defaultFindReplicaSetHostTimeoutMS", 10};
};

#ifdef MONGO_CONFIG_SSL

void waitForReadyRequest(executor::NetworkInterfaceMock* net) {
    while (!net->hasReadyRequests()) {
        net->advanceTime(net->now() + Milliseconds{1});
    }
}

BSONObj createEmptyCursorResponse(const NamespaceString& nss, CursorId backupCursorId) {
    return BSON(
        "cursor" << BSON("nextBatch" << BSONArray() << "id" << backupCursorId << "ns" << nss.ns())
                 << "ok" << 1.0);
}

BSONObj createBackupCursorResponse(const Timestamp& checkpointTimestamp,
                                   const NamespaceString& nss,
                                   CursorId backupCursorId) {
    const UUID backupId =
        UUID(uassertStatusOK(UUID::parse(("2b068e03-5961-4d8e-b47a-d1c8cbd4b835"))));
    StringData remoteDbPath = "/data/db/job0/mongorunner/test-1";
    BSONObjBuilder cursor;
    BSONArrayBuilder batch(cursor.subarrayStart("firstBatch"));
    auto metaData = BSON("backupId" << backupId << "checkpointTimestamp" << checkpointTimestamp
                                    << "dbpath" << remoteDbPath);
    batch.append(BSON("metadata" << metaData));

    batch.done();
    cursor.append("id", backupCursorId);
    cursor.append("ns", nss.ns());
    BSONObjBuilder backupCursorReply;
    backupCursorReply.append("cursor", cursor.obj());
    backupCursorReply.append("ok", 1.0);
    return backupCursorReply.obj();
}

void sendReponseToExpectedRequest(const BSONObj& backupCursorResponse,
                                  const std::string& expectedRequestFieldName,
                                  executor::NetworkInterfaceMock* net) {
    auto noi = net->getNextReadyRequest();
    auto request = noi->getRequest();
    ASSERT_EQUALS(expectedRequestFieldName, request.cmdObj.firstElementFieldNameStringData());
    net->scheduleSuccessfulResponse(
        noi, executor::RemoteCommandResponse(backupCursorResponse, Milliseconds()));
    net->runReadyNetworkOperations();
}

BSONObj createServerAggregateReply() {
    return CursorResponse(NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
                          0 /* cursorId */,
                          {BSON("byteOffset" << 0 << "endOfFile" << true << "data"
                                             << BSONBinData(0, 0, BinDataGeneral))})
        .toBSONAsInitialResponse();
}

/**
 * This class adds the TenantMigrationRecipientOpObserver to the main test fixture class. The
 * OpObserver uses the TenantFileImporter, which is a ReplicaSetAwareService that creates its own
 * worker thread when a state document is inserted. We need to ensure the ReplicaSetAwareService
 * shutdown procedure is executed in order to properly clean up and join the worker thread.
 */
class ShardMergeRecipientServiceTestInsert : public ShardMergeRecipientServiceTest {
private:
    void addOpObserver(OpObserverRegistry* opObserverRegistry) {
        opObserverRegistry->addObserver(std::make_unique<ShardMergeRecipientOpObserver>());
    }

    void tearDown() override {
        ShardMergeRecipientServiceTest::tearDown();
        ReplicaSetAwareServiceRegistry::get(getServiceContext()).onShutdown();
    }
};

TEST_F(ShardMergeRecipientServiceTestInsert, TestBlockersAreInsertedWhenInsertingStateDocument) {
    stopFailPointEnableBlock fp("fpBeforeFetchingDonorClusterTimeKeys");
    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(Timestamp(5, 1), 1));

    // Mock the aggregate response from the donor.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("aggregate", createServerAggregateReply());

    ShardMergeRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        auto fp = globalFailPointRegistry().find(
            "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
        auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                               0,
                                               BSON("action"
                                                    << "hang"));
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        // Test that access blocker exists.
        for (const auto& tenantId : _tenants) {
            auto blocker = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                               .getTenantMigrationAccessBlockerForTenantId(
                                   tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
            ASSERT(!!blocker);
        }
        fp->setMode(FailPoint::off);
    }

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, OpenBackupCursorSuccessfully) {
    stopFailPointEnableBlock fp("fpBeforeAdvancingStableTimestamp");
    const UUID migrationUUID = UUID::gen();
    const CursorId backupCursorId = 12345;
    const NamespaceString aggregateNs =
        NamespaceString::createNamespaceString_forTest("admin.$cmd.aggregate");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(Timestamp(5, 1), 1));

    // Mock the aggregate response from the donor.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("aggregate", createServerAggregateReply());

    ShardMergeRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        auto fp = globalFailPointRegistry().find("pauseBeforeRunTenantMigrationRecipientInstance");
        auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(initialTimesEntered + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
        fp->setMode(FailPoint::off);
    }

    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        waitForReadyRequest(net);
        // Mocking the aggregate command network response of the backup cursor in order to have
        // data to parse.
        sendReponseToExpectedRequest(createBackupCursorResponse(kDefaultStartMigrationTimestamp,
                                                                aggregateNs,
                                                                backupCursorId),
                                     "aggregate",
                                     net);
        sendReponseToExpectedRequest(
            createEmptyCursorResponse(aggregateNs, backupCursorId), "getMore", net);
        sendReponseToExpectedRequest(
            createEmptyCursorResponse(aggregateNs, backupCursorId), "getMore", net);
    }

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    checkStateDocPersisted(opCtx.get(), instance.get());

    taskFp->setMode(FailPoint::off);

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, OpenBackupCursorAndRetriesDueToTs) {
    stopFailPointEnableBlock fp("fpBeforeAdvancingStableTimestamp");
    const UUID migrationUUID = UUID::gen();
    const CursorId backupCursorId = 12345;
    const NamespaceString aggregateNs =
        NamespaceString::createNamespaceString_forTest("admin.$cmd.aggregate");

    auto taskFp = globalFailPointRegistry().find("hangBeforeTaskCompletion");
    auto initialTimesEntered = taskFp->setMode(FailPoint::alwaysOn);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(Timestamp(5, 1), 1));

    // Mock the aggregate response from the donor.
    MockRemoteDBServer* const _donorServer =
        mongo::MockConnRegistry::get()->getMockRemoteDBServer(replSet.getPrimary());
    _donorServer->setCommandReply("aggregate", createServerAggregateReply());

    ShardMergeRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        auto fp = globalFailPointRegistry().find("pauseBeforeRunTenantMigrationRecipientInstance");
        auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(initialTimesEntered + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
        fp->setMode(FailPoint::off);
    }

    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        waitForReadyRequest(net);

        // Mocking the aggregate command network response of the backup cursor in order to have data
        // to parse. In this case we pass a timestamp that is inferior to the
        // startMigrationTimestamp which will cause a retry. We then provide a correct timestamp in
        // the next response and succeed.
        sendReponseToExpectedRequest(
            createBackupCursorResponse(Timestamp(0, 0), aggregateNs, backupCursorId),
            "aggregate",
            net);
        sendReponseToExpectedRequest(createBackupCursorResponse(kDefaultStartMigrationTimestamp,
                                                                aggregateNs,
                                                                backupCursorId),
                                     "killCursors",
                                     net);
        sendReponseToExpectedRequest(
            createEmptyCursorResponse(aggregateNs, backupCursorId), "killCursors", net);
        sendReponseToExpectedRequest(createBackupCursorResponse(kDefaultStartMigrationTimestamp,
                                                                aggregateNs,
                                                                backupCursorId),
                                     "aggregate",
                                     net);
        sendReponseToExpectedRequest(
            createEmptyCursorResponse(aggregateNs, backupCursorId), "getMore", net);
        sendReponseToExpectedRequest(
            createEmptyCursorResponse(aggregateNs, backupCursorId), "getMore", net);
    }

    taskFp->waitForTimesEntered(initialTimesEntered + 1);

    checkStateDocPersisted(opCtx.get(), instance.get());

    taskFp->setMode(FailPoint::off);

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, TestGarbageCollectionStarted) {
    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);

    auto fp = globalFailPointRegistry().find("pauseTenantMigrationRecipientBeforeDeletingStateDoc");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    ShardMergeRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);
    initialStateDocument.setState(ShardMergeRecipientStateEnum::kStarted);

    // Set startGarbageCollect to true to simulate the case where 'recipientForgetMigration' is
    // received before 'recipientSyncData'.
    initialStateDocument.setStartGarbageCollect(true);

    auto opCtx = makeOperationContext();
    auto instance = ShardMergeRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    ASSERT_EQ(ErrorCodes::TenantMigrationForgotten,
              instance->getMigrationCompletionFuture().getNoThrow().code());

    fp->waitForTimesEntered(initialTimesEntered + 1);
    checkStateDocPersisted(opCtx.get(), instance.get());

    auto stateDoc = getStateDoc(instance.get());
    ASSERT_EQ(stateDoc.getState(), ShardMergeRecipientStateEnum::kAborted);

    fp->setMode(FailPoint::off);

    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow().code());
}

TEST_F(ShardMergeRecipientServiceTest, TestForgetMigrationAborted) {
    const UUID migrationUUID = UUID::gen();

    auto deletionFp =
        globalFailPointRegistry().find("pauseTenantMigrationRecipientBeforeDeletingStateDoc");
    auto deletionFpTimesEntered = deletionFp->setMode(FailPoint::alwaysOn);

    auto fp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
    getTopologyManager()->setTopologyDescription(replSet.getTopologyDescription(clock()));
    insertTopOfOplog(&replSet, OpTime(Timestamp(5, 1), 1));


    ShardMergeRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    initialStateDocument.setRecipientCertificateForDonor(kRecipientPEMPayload);

    auto opCtx = makeOperationContext();
    auto instance = ShardMergeRecipientService::Instance::getOrCreate(
        opCtx.get(), _service, initialStateDocument.toBSON());
    ASSERT(instance.get());
    fp->waitForTimesEntered(initialTimesEntered + 1);

    instance->onReceiveRecipientForgetMigration(opCtx.get(), MigrationDecisionEnum::kAborted);

    fp->setMode(FailPoint::off);

    ASSERT_EQ(ErrorCodes::TenantMigrationForgotten,
              instance->getMigrationCompletionFuture().getNoThrow().code());

    deletionFp->waitForTimesEntered(deletionFpTimesEntered + 1);
    checkStateDocPersisted(opCtx.get(), instance.get());
    auto stateDoc = getStateDoc(instance.get());
    ASSERT_EQ(stateDoc.getState(), ShardMergeRecipientStateEnum::kAborted);

    deletionFp->setMode(FailPoint::off);

    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow().code());
}

#endif
}  // namespace repl
}  // namespace mongo
