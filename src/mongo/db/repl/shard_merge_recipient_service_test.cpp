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

#include <cstddef>
#include <cstdint>
#include <fstream>  // IWYU pragma: keep
#include <iterator>
#include <list>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/sdam/mock_topology_manager.h"
#include "mongo/client/streamable_replica_set_monitor_for_testing.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_fetcher_mock.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/mock_network_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/ssl_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {

namespace {
using namespace mongo::test::mock;
using namespace mongo::executor;

constexpr std::int32_t stopFailPointErrorCode = 4880402;
const Timestamp kDefaultStartMigrationTimestamp(1, 1);
static const UUID kMigrationUUID = UUID::gen();

void advanceClock(NetworkInterfaceMock* net, Milliseconds duration) {
    NetworkInterfaceMock::InNetworkGuard guard(net);
    auto when = net->now() + duration;
    ASSERT_EQ(when, net->runUntil(when));
}

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
                              boost::none,                // checkExistenceForDiffInsert
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

/**
 * Mock OpObserver that tracks storage events.
 */
class ShardMergeRecipientServiceTestOpObserver final : public OpObserverNoop {
public:
    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) final {
        if (shard_merge_utils::isDonatedFilesCollection(coll->ns())) {
            for (auto it = first; it != last; it++) {
                backupCursorFiles.emplace_back(it->doc.getOwned());
            }
            return;
        }
    }

    std::vector<BSONObj> backupCursorFiles;
};

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

        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        WaitForMajorityService::get(serviceContext).startup(serviceContext);

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(
                serviceContext, createServerlessReplSettings());
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
                std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
            opObserverRegistry->addObserver(
                std::make_unique<PrimaryOnlyServiceOpObserver>(serviceContext));
            auto opObserver = std::make_unique<ShardMergeRecipientServiceTestOpObserver>();
            _opObserver = opObserver.get();
            opObserverRegistry->addObserver(std::move(opObserver));

            // Add OpObserver needed by subclasses.
            addOpObserver(opObserverRegistry);

            _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
            std::unique_ptr<ShardMergeRecipientService> service =
                std::make_unique<ShardMergeRecipientService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());

            // Fake replSet just for creating consistent URI for monitor
            MockReplicaSet replSet(
                "donorSet", 1, true /* hasPrimary */, true /* dollarPrefixHosts */);
            _rsmMonitor.setup(replSet.getURI());
        }
        stepUp();

        _service = _registry->lookupServiceByName(
            ShardMergeRecipientService::kShardMergeRecipientServiceName);
        ASSERT(_service);

        // Automatically mark the state doc garbage collectable after data sync completion.
        globalFailPointRegistry()
            .find("autoRecipientForgetMigrationAbort")
            ->setMode(FailPoint::alwaysOn,
                      0,
                      BSON("state"
                           << "aborted"));

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
        auto net = std::make_unique<NetworkInterfaceMock>();
        _net = net.get();
        _mockNet = std::make_unique<MockNetwork>(_net);

        ThreadPoolMock::Options dbThreadPoolOptions;
        dbThreadPoolOptions.onCreateThread = []() {
            Client::initThread("FetchMockTaskExecutor", getGlobalServiceContext()->getService());
        };
        _threadPoolExecutor = makeThreadPoolTestExecutor(std::move(net), dbThreadPoolOptions);
        _threadPoolExecutor->startup();
        _threadPoolExecutorMock = std::make_shared<TaskExecutorMock>(_threadPoolExecutor.get());

        // Setup mock donor replica set.
        _mockDonorRs = std::make_unique<MockReplicaSet>(
            "donorSet", 3, true /* hasPrimary */, true /* dollarPrefixHosts */);
        getTopologyManager()->setTopologyDescription(_mockDonorRs->getTopologyDescription(clock()));
        insertTopOfOplog(_mockDonorRs.get(), OpTime(Timestamp(1, 1), 1));
    }

    void tearDown() override {
        _threadPoolExecutorMock->shutdown();
        _threadPoolExecutorMock->join();

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
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(
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

    struct cursorDataMock {
        const UUID kBackupId =
            UUID(uassertStatusOK(UUID::parse(("2b068e03-5961-4d8e-b47a-d1c8cbd4b835"))));
        const Timestamp kCheckpointTimestamp = Timestamp(1, 1);
        const CursorId kBackupCursorId = 3703253128214665235ll;
        const NamespaceString kNss =
            NamespaceString::createNamespaceString_forTest("admin.$cmd.aggregate");
        const std::vector<std::vector<std::string>> backupCursorFiles{
            {"/data/db/job0/mongorunner/test-1/WiredTiger",
             "/data/db/job0/mongorunner/test-1/WiredTiger.backup",
             "/data/db/job0/mongorunner/test-1/sizeStorer.wt",
             "/data/db/job0/mongorunner/test-1/index-1--3853645825680686061.wt",
             "/data/db/job0/mongorunner/test-1/collection-0--3853645825680686061.wt",
             "/data/db/job0/mongorunner/test-1/_mdb_catalog.wt",
             "/data/db/job0/mongorunner/test-1/WiredTigerHS.wt",
             "/data/db/job0/mongorunner/test-1/journal/WiredTigerLog.0000000001"},
            {"/data/db/job0/mongorunner/test-1/journal/WiredTigerLog.0000000002",
             "/data/db/job0/mongorunner/test-1/journal/WiredTigerLog.0000000003"}};
        const std::vector<std::vector<int>> backupCursorFileSizes{
            {47, 77050, 20480, 20480, 20480, 36864, 4096, 104857600}, {104857600, 104857600}};
        const StringData remoteDbPath = "/data/db/job0/mongorunner/test-1";

        BSONObj getBackupCursorBatches(
            int batchId, boost::optional<const Timestamp&> checkpointTs = boost::none) {
            // Empty last batch.
            if (batchId == -1) {
                return BSON("cursor" << BSON("nextBatch" << BSONArray() << "id" << kBackupCursorId
                                                         << "ns" << kNss.ns_forTest())
                                     << "ok" << 1.0);
            }

            BSONObjBuilder cursor;
            BSONArrayBuilder batch(
                cursor.subarrayStart((batchId == 0) ? "firstBatch" : "nextBatch"));

            // First batch.
            if (batchId == 0) {
                auto metaData =
                    BSON("backupId" << kBackupId << "checkpointTimestamp"
                                    << (checkpointTs ? *checkpointTs : kCheckpointTimestamp)
                                    << "dbpath" << remoteDbPath);
                batch.append(BSON("metadata" << metaData));
            }
            for (int i = 0; i < int(backupCursorFiles[batchId].size()); i++) {
                batch.append(BSON("filename" << backupCursorFiles[batchId][i] << "fileSize"
                                             << backupCursorFileSizes[batchId][i]));
            }
            batch.done();
            cursor.append("id", kBackupCursorId);
            cursor.append("ns", kNss.ns_forTest());
            BSONObjBuilder backupCursorReply;
            backupCursorReply.append("cursor", cursor.obj());
            backupCursorReply.append("ok", 1.0);
            return backupCursorReply.obj();
        }

        BSONObj getkillBackupCursorReply() {
            return BSON("ok" << 1.0 << "cursorsKilled" << BSON_ARRAY(kBackupCursorId));
        }

    } cursorDataMock;

    const BSONObj backupCursorRequest =
        BSON("aggregate" << 1 << "pipeline" << BSON_ARRAY(BSON("$backupCursor" << BSONObj())));
    const BSONObj getMoreRequest = BSON("getMore" << cursorDataMock.kBackupCursorId << "collection"
                                                  << cursorDataMock.kNss.coll());
    const BSONObj killBackupCursorRequest =
        BSON("killCursors" << cursorDataMock.kNss.coll() << "cursors"
                           << BSON_ARRAY(cursorDataMock.kBackupCursorId));

    void expectSuccessfulBackupCursorCall() {
        {
            MockNetwork::InSequence seq(*_mockNet);
            _mockNet
                ->expect(backupCursorRequest,
                         RemoteCommandResponse(
                             {cursorDataMock.getBackupCursorBatches(0), Milliseconds()}))
                .times(1);
            _mockNet
                ->expect(getMoreRequest,
                         RemoteCommandResponse(
                             {cursorDataMock.getBackupCursorBatches(1), Milliseconds()}))
                .times(1);

            _mockNet
                ->expect(getMoreRequest,
                         RemoteCommandResponse(
                             {cursorDataMock.getBackupCursorBatches(-1), Milliseconds()}))
                .times(1);
        }

        _mockNet->runUntilExpectationsSatisfied();
    }

    void expectSuccessfulKillBackupCursorCall() {
        _mockNet
            ->expect(
                killBackupCursorRequest,
                RemoteCommandResponse({cursorDataMock.getkillBackupCursorReply(), Milliseconds()}))
            .times(1);
        _mockNet->runUntilExpectationsSatisfied();
    }

    void expectSuccessfulBackupCursorEmptyGetMore() {
        _mockNet
            ->expect(
                getMoreRequest,
                RemoteCommandResponse({cursorDataMock.getBackupCursorBatches(-1), Milliseconds()}))
            .times(1);
        _mockNet->runUntilExpectationsSatisfied();
    }

    bool verifyCursorFiles() {
        int numOfFiles = 0;

        StringSet returnedFilenames;
        std::for_each(_opObserver->backupCursorFiles.begin(),
                      _opObserver->backupCursorFiles.end(),
                      [this, &returnedFilenames](const BSONObj& metadata) {
                          auto migrationId = uassertStatusOK(
                              UUID::parse(metadata[shard_merge_utils::kMigrationIdFieldName]));
                          ASSERT(migrationId == kMigrationUUID);
                          auto backupId = uassertStatusOK(
                              UUID::parse(metadata[shard_merge_utils::kBackupIdFieldName]));
                          ASSERT(backupId == cursorDataMock.kBackupId);
                          returnedFilenames.insert(metadata["filename"].str());
                      });

        for (int batchId = 0; batchId < int(cursorDataMock.backupCursorFiles.size()); batchId++) {
            for (const auto& file : cursorDataMock.backupCursorFiles[batchId]) {
                numOfFiles++;
                if (!returnedFilenames.contains(file)) {
                    return false;
                }
            }
        }
        return numOfFiles == (int)returnedFilenames.size();
    }

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

    int64_t countLogLinesWithId(int32_t id) {
        return countBSONFormatLogLinesIsSubset(BSON("id" << id));
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

    NetworkInterfaceMock* getNet() {
        return _net;
    }

    const MockReplicaSet* getDonorRs() {
        return _mockDonorRs.get();
    }

    MockNetwork* getMockNet() {
        return _mockNet.get();
    }

    void setInstanceBackupCursorFetcherExecutor(
        std::shared_ptr<ShardMergeRecipientService::Instance> instance) {
        instance->setBackupCursorFetcherExecutor_forTest(_threadPoolExecutorMock);
    }

private:
    virtual void addOpObserver(OpObserverRegistry* opObserverRegistry){};

    ClockSourceMock _clkSource;
    std::shared_ptr<ThreadPoolTaskExecutor> _threadPoolExecutor;
    std::shared_ptr<TaskExecutorMock> _threadPoolExecutorMock;
    std::unique_ptr<MockNetwork> _mockNet;
    std::unique_ptr<MockReplicaSet> _mockDonorRs;
    NetworkInterfaceMock* _net = nullptr;
    ShardMergeRecipientServiceTestOpObserver* _opObserver = nullptr;

    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};

    StreamableReplicaSetMonitorForTesting _rsmMonitor;
    RAIIServerParameterControllerForTest _findHostTimeout{"defaultFindReplicaSetHostTimeoutMS", 10};
};

#ifdef MONGO_CONFIG_SSL

/**
 * This class adds the TenantMigrationRecipientOpObserver to the main test fixture class. The
 * OpObserver uses the TenantFileImporter, which is a ReplicaSetAwareService that creates its own
 * worker thread when a state document is inserted. We need to ensure the ReplicaSetAwareService
 * shutdown procedure is executed in order to properly clean up and join the worker thread.
 */
class ShardMergeRecipientServiceTestInsert : public ShardMergeRecipientServiceTest {
private:
    void addOpObserver(OpObserverRegistry* opObserverRegistry) override {
        opObserverRegistry->addObserver(std::make_unique<ShardMergeRecipientOpObserver>());
    }

    void tearDown() override {
        ShardMergeRecipientServiceTest::tearDown();
        ReplicaSetAwareServiceRegistry::get(getServiceContext()).onShutdown();
    }
};

TEST_F(ShardMergeRecipientServiceTestInsert, TestBlockersAreInsertedWhenInsertingStateDocument) {
    stopFailPointEnableBlock fp("fpBeforeFetchingDonorClusterTimeKeys");

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                                BSON("action"
                                     << "hang"));
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());

        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);

        // Test that access blocker exists.
        for (const auto& tenantId : _tenants) {
            auto blocker = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                               .getTenantMigrationAccessBlockerForTenantId(
                                   tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
            ASSERT(!!blocker);
        }
    }

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, OpenBackupCursorSuccessfully) {
    stopFailPointEnableBlock fp("fpBeforeAdvancingStableTimestamp");

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    expectSuccessfulBackupCursorCall();

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    verifyCursorFiles();
}

TEST_F(ShardMergeRecipientServiceTest, OpenBackupCursorRetriesIfBackupCursorIsTooStale) {
    stopFailPointEnableBlock fp("fpBeforeAdvancingStableTimestamp");

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    startCapturingLogMessages();

    const auto TSOlderThanCursorDataMockCheckpointTS = Timestamp(0, 1);

    {
        MockNetwork::InSequence seq(*getMockNet());
        getMockNet()
            ->expect(backupCursorRequest,
                     RemoteCommandResponse({cursorDataMock.getBackupCursorBatches(
                                                0, TSOlderThanCursorDataMockCheckpointTS),
                                            Milliseconds()}))
            .times(1);
        getMockNet()
            ->expect(
                killBackupCursorRequest,
                RemoteCommandResponse({cursorDataMock.getkillBackupCursorReply(), Milliseconds()}))
            .times(1);
        getMockNet()
            ->expect(
                backupCursorRequest,
                RemoteCommandResponse({cursorDataMock.getBackupCursorBatches(0), Milliseconds()}))
            .times(1);
        getMockNet()
            ->expect(
                getMoreRequest,
                RemoteCommandResponse({cursorDataMock.getBackupCursorBatches(1), Milliseconds()}))
            .times(1);
        getMockNet()
            ->expect(
                getMoreRequest,
                RemoteCommandResponse({cursorDataMock.getBackupCursorBatches(-1), Milliseconds()}))
            .times(1);
    }
    getMockNet()->runUntilExpectationsSatisfied();

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesWithId(7339733));

    verifyCursorFiles();
}

TEST_F(ShardMergeRecipientServiceTest, MergeFailsIfBackupCursorIsAlreadyActiveOnDonor) {
    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    auto backupCursorConflictErrorCode = 50886;
    getMockNet()
        ->expect(backupCursorRequest,
                 RemoteCommandResponse(
                     ErrorCodes::Error(backupCursorConflictErrorCode),
                     "The existing backup cursor must be closed before $backupCursor can succeed.",
                     Milliseconds()))
        .times(1);
    getMockNet()->runUntilExpectationsSatisfied();

    ASSERT_EQ(backupCursorConflictErrorCode,
              instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, MergeFailsIfDonorIsFsyncLocked) {
    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    auto backupCursorConflictWithFsyncErrorCode = 50887;
    getMockNet()
        ->expect(backupCursorRequest,
                 RemoteCommandResponse(ErrorCodes::Error(backupCursorConflictWithFsyncErrorCode),
                                       "The node is currently fsyncLocked.",
                                       Milliseconds()))
        .times(1);
    getMockNet()->runUntilExpectationsSatisfied();

    ASSERT_EQ(backupCursorConflictWithFsyncErrorCode,
              instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, MergeFailsIfBackupCursorNotSupportedOnDonor) {
    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    auto backupCursorNotSupportedErrorCode = 40324;
    getMockNet()
        ->expect(backupCursorRequest,
                 RemoteCommandResponse(ErrorCodes::Error(backupCursorNotSupportedErrorCode),
                                       "Unrecognized pipeline stage name: '$backupCursor'",
                                       Milliseconds()))
        .times(1);
    getMockNet()->runUntilExpectationsSatisfied();

    ASSERT_EQ(backupCursorNotSupportedErrorCode,
              instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, KeepingBackupCursorAlive) {
    stopFailPointEnableBlock stopFp("fpAfterRetrievingStartOpTimesMigrationRecipientInstance");

    auto fp =
        globalFailPointRegistry().find("pauseAfterRetrievingLastTxnMigrationRecipientInstance");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }

    expectSuccessfulBackupCursorCall();
    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Verify that backup cursor alive signal is sent every kBackupCursorKeepAliveIntervalMillis.
    for (int i = 0; i < 5; i++) {
        expectSuccessfulBackupCursorEmptyGetMore();
        advanceClock(getNet(),
                     Milliseconds(shard_merge_utils::kBackupCursorKeepAliveIntervalMillis));
    }

    fp->setMode(FailPoint::off);
    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, TestGarbageCollectionStarted) {
    auto fp = globalFailPointRegistry().find("pauseTenantMigrationRecipientBeforeDeletingStateDoc");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));
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
    auto deletionFp =
        globalFailPointRegistry().find("pauseTenantMigrationRecipientBeforeDeletingStateDoc");
    auto deletionFpTimesEntered = deletionFp->setMode(FailPoint::alwaysOn);

    auto fp =
        globalFailPointRegistry().find("fpAfterPersistingTenantMigrationRecipientInstanceStateDoc");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn,
                                           0,
                                           BSON("action"
                                                << "hang"));

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

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

TEST_F(ShardMergeRecipientServiceTest, ImportQuorumWaitCanBeInterruptedByForgetMigrationCmd) {
    auto hangBeforeImportQuorumWait =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = hangBeforeImportQuorumWait->setMode(FailPoint::alwaysOn,
                                                                   0,
                                                                   BSON("action"
                                                                        << "hang"));
    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }
    expectSuccessfulBackupCursorCall();

    hangBeforeImportQuorumWait->waitForTimesEntered(initialTimesEntered + 1);
    instance->onReceiveRecipientForgetMigration(opCtx.get(), MigrationDecisionEnum::kAborted);
    hangBeforeImportQuorumWait->setMode(FailPoint::off);

    ASSERT_EQ(ErrorCodes::TenantMigrationForgotten,
              instance->getMigrationCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, ImportQuorumWaitCanBeInterruptedByFailover) {
    auto hangBeforeImportQuorumWait =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = hangBeforeImportQuorumWait->setMode(FailPoint::alwaysOn,
                                                                   0,
                                                                   BSON("action"
                                                                        << "hang"));
    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }
    expectSuccessfulBackupCursorCall();

    hangBeforeImportQuorumWait->waitForTimesEntered(initialTimesEntered + 1);
    instance->interrupt(Status(ErrorCodes::InterruptedDueToReplStateChange,
                               "PrimaryOnlyService interrupted due to stepdown"));
    hangBeforeImportQuorumWait->setMode(FailPoint::off);

    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange,
              instance->getMigrationCompletionFuture().getNoThrow());
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange,
              instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, ImportQuorumWaitCanBeInterruptedByWaitTimeoutExpired) {
    RAIIServerParameterControllerForTest voteImportTimeoutSecs{"importQuorumTimeoutSeconds", 1};

    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }
    expectSuccessfulBackupCursorCall();

    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, instance->getMigrationCompletionFuture().getNoThrow());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

TEST_F(ShardMergeRecipientServiceTest, ImportQuorumWaitCanBeInterruptedOnQuorumSatisfied) {
    stopFailPointEnableBlock fp("fpBeforeMarkingCloneSuccess");

    auto hangBeforeImportQuorumWait =
        globalFailPointRegistry().find("fpAfterStartingOplogFetcherMigrationRecipientInstance");
    auto initialTimesEntered = hangBeforeImportQuorumWait->setMode(FailPoint::alwaysOn,
                                                                   0,
                                                                   BSON("action"
                                                                        << "hang"));
    ShardMergeRecipientDocument initialStateDocument(
        kMigrationUUID,
        getDonorRs()->getConnectionString(),
        _tenants,
        kDefaultStartMigrationTimestamp,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto opCtx = makeOperationContext();
    std::shared_ptr<ShardMergeRecipientService::Instance> instance;
    {
        FailPointEnableBlock fp("pauseBeforeRunTenantMigrationRecipientInstance");
        instance = ShardMergeRecipientService::Instance::getOrCreate(
            opCtx.get(), _service, initialStateDocument.toBSON());
        ASSERT(instance.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        setInstanceBackupCursorFetcherExecutor(instance);
        instance->setCreateOplogFetcherFn_forTest(std::make_unique<CreateOplogFetcherMockFn>());
    }
    expectSuccessfulBackupCursorCall();

    hangBeforeImportQuorumWait->waitForTimesEntered(initialTimesEntered + 1);
    instance->onMemberImportedFiles(HostAndPort("localhost:12345"));
    hangBeforeImportQuorumWait->setMode(FailPoint::off);

    expectSuccessfulKillBackupCursorCall();

    ASSERT_EQ(stopFailPointErrorCode, instance->getMigrationCompletionFuture().getNoThrow().code());
    ASSERT_OK(instance->getForgetMigrationDurableFuture().getNoThrow());
}

#endif
}  // namespace repl
}  // namespace mongo
