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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts) : _opCtx(opCtx) {
        _opCtx->recoveryUnit()->abandonSnapshot();
        if (ts.isNull()) {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

/**
 * Observed problems using ShardingMongodTestFixture:
 *
 * - Does not mix with dbtest. Both will initialize a ServiceContext.
 * - By default uses ephemeralForTest. These tests require a storage engine that supports majority
 *   reads.
 * - When run as a unittest (and using WT), the fixture initializes the storage engine for each test
 *   that is run. WT specifically installs a ServerStatusSection. The server status code asserts
 *   that a section is never added after a `serverStatus` command is run. Tests defined in
 *   `migration_manager_test` (part of the `db_s_config_server_test` unittest binary) call a
 *   serverStatus triggerring this assertion.
 */
class ReshardingTest {
public:
    ServiceContext::UniqueOperationContext _opCtxRaii = cc().makeOperationContext();
    OperationContext* _opCtx = _opCtxRaii.get();
    ServiceContext* _svcCtx = _opCtx->getServiceContext();
    VectorClockMutable* _clock = VectorClockMutable::get(_opCtx);
    // A convenience UUID.
    UUID _reshardingUUID = UUID::gen();
    // Timestamp of the first oplog entry which the fixture will set up.
    Timestamp _fetchTimestamp;

    ReshardingTest() {
        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(100 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
        setGlobalReplSettings(replSettings);

        auto replCoordinatorMock =
            std::make_unique<repl::ReplicationCoordinatorMock>(_svcCtx, replSettings);
        replCoordinatorMock->alwaysAllowWrites(true);
        repl::ReplicationCoordinator::set(_svcCtx, std::move(replCoordinatorMock));
        repl::StorageInterface::set(_svcCtx, std::make_unique<repl::StorageInterfaceImpl>());
        repl::ReplicationProcess::set(
            _svcCtx,
            std::make_unique<repl::ReplicationProcess>(
                repl::StorageInterface::get(_svcCtx),
                std::make_unique<repl::ReplicationConsistencyMarkersMock>(),
                std::make_unique<repl::ReplicationRecoveryMock>()));

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx->getClient()).clearLastOp_forTest();

        auto opObsRegistry = std::make_unique<OpObserverRegistry>();
        opObsRegistry->addObserver(std::make_unique<OpObserverImpl>());
        _opCtx->getServiceContext()->setOpObserver(std::move(opObsRegistry));

        // Clean out the oplog and write one no-op entry. The timestamp of this first oplog entry
        // will serve as resharding's `fetchTimestamp`.
        repl::setOplogCollectionName(_svcCtx);
        repl::createOplog(_opCtx);
        reset(NamespaceString::kRsOplogNamespace);
        {
            WriteUnitOfWork wuow(_opCtx);
            Lock::GlobalLock lk(_opCtx, LockMode::MODE_IX);
            _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                _opCtx,
                // Choose a random, irrelevant replicated namespace.
                NamespaceString::kSystemKeysNamespace,
                UUID::gen(),
                BSON("msg"
                     << "Dummy op."),
                boost::none,
                boost::none,
                boost::none,
                boost::none,
                boost::none);
            wuow.commit();
            repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        }
        _fetchTimestamp = queryOplog(BSONObj())["ts"].timestamp();
        std::cout << " Fetch timestamp: " << _fetchTimestamp.toString() << std::endl;

        _clock->tickClusterTimeTo(LogicalTime(Timestamp(1, 0)));
    }

    ~ReshardingTest() {
        try {
            reset(NamespaceString("local.oplog.rs"));
        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }


    /**
     * Walking on ice: resetting the ReplicationCoordinator destroys the underlying
     * `DropPendingCollectionReaper`. Use a truncate/dropAllIndexes to clean out a collection
     * without actually dropping it.
     */
    void reset(NamespaceString nss) const {
        ::mongo::writeConflictRetry(_opCtx, "deleteAll", nss.ns(), [&] {
            // Do not write DDL operations to the oplog. This keeps the initial oplog state for each
            // test predictable.
            repl::UnreplicatedWritesBlock uwb(_opCtx);
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
            AutoGetCollection collRaii(_opCtx, nss, LockMode::MODE_X);

            if (collRaii) {
                WriteUnitOfWork wunit(_opCtx);
                invariant(collRaii.getWritableCollection()->truncate(_opCtx).isOK());
                if (_opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
                    ASSERT_OK(_opCtx->recoveryUnit()->setTimestamp(Timestamp(1, 1)));
                }
                collRaii.getWritableCollection()->getIndexCatalog()->dropAllIndexes(_opCtx, false);
                wunit.commit();
                return;
            }

            AutoGetOrCreateDb dbRaii(_opCtx, nss.db(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            if (_opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
                ASSERT_OK(_opCtx->recoveryUnit()->setTimestamp(Timestamp(1, 1)));
            }
            invariant(dbRaii.getDb()->createCollection(_opCtx, nss));
            wunit.commit();
        });
    }

    void insertDocument(const CollectionPtr& coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool fromMigrate = false;
        ASSERT_OK(coll->insertDocument(_opCtx, stmt, nullOpDebug, fromMigrate));
    }

    BSONObj queryCollection(NamespaceString nss, const BSONObj& query) {
        BSONObj ret;
        ASSERT_TRUE(Helpers::findOne(
            _opCtx, AutoGetCollectionForRead(_opCtx, nss).getCollection(), query, ret))
            << "Query: " << query;
        return ret;
    }

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min());
        return queryCollection(NamespaceString::kRsOplogNamespace, query);
    }

    repl::OpTime getLastApplied() {
        return repl::ReplicationCoordinator::get(_opCtx)->getMyLastAppliedOpTime();
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext() {
        NamespaceString slimNss =
            NamespaceString("local.system.resharding.slimOplogForGraphLookup");

        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(_opCtx, NamespaceString::kRsOplogNamespace));
        expCtx->setResolvedNamespace(NamespaceString::kRsOplogNamespace,
                                     {NamespaceString::kRsOplogNamespace, {}});
        expCtx->setResolvedNamespace(slimNss,
                                     {slimNss, std::vector<BSONObj>{getSlimOplogPipeline()}});
        return expCtx;
    }

    int itcount(NamespaceString nss) {
        OneOffRead oof(_opCtx, Timestamp::min());
        AutoGetCollectionForRead autoColl(_opCtx, nss);
        auto cursor = autoColl.getCollection()->getCursor(_opCtx);

        int ret = 0;
        while (auto rec = cursor->next()) {
            ++ret;
        }

        return ret;
    }

    // Writes five documents to `dataCollectionNss` that are replicated with a `destinedRecipient`
    // followed by the final no-op oplog entry that signals the last oplog entry needed to be
    // applied for resharding to move to the next stage.
    void setupBasic(NamespaceString outputCollectionNss,
                    NamespaceString dataCollectionNss,
                    ShardId destinedRecipient) {
        reset(outputCollectionNss);
        reset(dataCollectionNss);

        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

        // Set a failpoint to tack a `destinedRecipient` onto oplog entries.
        setGlobalFailPoint("addDestinedRecipient",
                           BSON("mode"
                                << "alwaysOn"
                                << "data"
                                << BSON("destinedRecipient" << destinedRecipient.toString())));

        // Insert five documents. Advance the majority point.
        const std::int32_t docsToInsert = 5;
        {
            for (std::int32_t num = 0; num < docsToInsert; ++num) {
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollection(),
                               InsertStatement(BSON("_id" << num << "a" << num)));
                wuow.commit();
            }
        }

        // Write an entry saying that fetching is complete.
        {
            WriteUnitOfWork wuow(_opCtx);
            _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                _opCtx,
                dataColl.getCollection()->ns(),
                dataColl.getCollection()->uuid(),
                BSON("msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                          dataColl.getCollection()->ns().toString())),
                BSON("type"
                     << "reshardFinalOp"
                     << "reshardingUUID" << _reshardingUUID),
                boost::none,
                boost::none,
                boost::none,
                boost::none);
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        _svcCtx->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            getLastApplied().getTimestamp());

        // Disable the failpoint.
        setGlobalFailPoint("addDestinedRecipient",
                           BSON("mode"
                                << "off"));
    }
};

class RunFetchIteration : public ReshardingTest {
public:
    void run() {
        const NamespaceString outputCollectionNss("dbtests.outputCollection");
        reset(outputCollectionNss);
        const NamespaceString dataCollectionNss("dbtests.runFetchIteration");
        reset(dataCollectionNss);

        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

        // Set a failpoint to tack a `destinedRecipient` onto oplog entries.
        setGlobalFailPoint("addDestinedRecipient",
                           BSON("mode"
                                << "alwaysOn"
                                << "data"
                                << BSON("destinedRecipient"
                                        << "shard1")));

        // Insert five documents. Advance the majority point. Insert five more documents.
        const std::int32_t docsToInsert = 5;
        {
            for (std::int32_t num = 0; num < docsToInsert; ++num) {
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollection(),
                               InsertStatement(BSON("_id" << num << "a" << num)));
                wuow.commit();
            }
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        const Timestamp firstFiveLastApplied = getLastApplied().getTimestamp();
        _svcCtx->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            firstFiveLastApplied);
        {
            for (std::int32_t num = docsToInsert; num < 2 * docsToInsert; ++num) {
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollection(),
                               InsertStatement(BSON("_id" << num << "a" << num)));
                wuow.commit();
            }
        }

        // Disable the failpoint.
        setGlobalFailPoint("addDestinedRecipient",
                           BSON("mode"
                                << "off"));

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        const Timestamp latestLastApplied = getLastApplied().getTimestamp();

        // The first call to `iterate` should return the first five inserts and return a
        // `ReshardingDonorOplogId` matching the last applied of those five inserts.
        ReshardingOplogFetcher fetcher(_reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       ShardId("fakeDonorShard"),
                                       ShardId("shard1"),
                                       true,
                                       outputCollectionNss);
        DBDirectClient client(_opCtx);
        boost::optional<ReshardingDonorOplogId> donorOplogId =
            fetcher.iterate(_opCtx,
                            &client,
                            createExpressionContext(),
                            {_fetchTimestamp, _fetchTimestamp},
                            dataColl->uuid(),
                            {"shard1"},
                            true,
                            outputCollectionNss);

        ASSERT(donorOplogId != boost::none);
        ASSERT_EQ(docsToInsert, itcount(outputCollectionNss));
        ASSERT_EQ(firstFiveLastApplied, donorOplogId->getClusterTime());
        ASSERT_EQ(firstFiveLastApplied, donorOplogId->getTs());

        // Advance the committed snapshot. A second `iterate` should return the second batch of five
        // inserts.
        _svcCtx->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            getLastApplied().getTimestamp());

        donorOplogId = fetcher.iterate(_opCtx,
                                       &client,
                                       createExpressionContext(),
                                       {firstFiveLastApplied, firstFiveLastApplied},
                                       dataColl->uuid(),
                                       {"shard1"},
                                       true,
                                       outputCollectionNss);

        ASSERT(donorOplogId != boost::none);
        // Two batches of five inserts entry for the create collection oplog entry.
        ASSERT_EQ((2 * docsToInsert), itcount(outputCollectionNss));
        ASSERT_EQ(latestLastApplied, donorOplogId->getClusterTime());
        ASSERT_EQ(latestLastApplied, donorOplogId->getTs());
    }
};

class RunConsume : public ReshardingTest {
public:
    void run() {
        const NamespaceString outputCollectionNss("dbtests.outputCollection");
        const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

        const ShardId destinationShard("shard1");
        setupBasic(outputCollectionNss, dataCollectionNss, destinationShard);

        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IS);
        ReshardingOplogFetcher fetcher(_reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       ShardId("fakeDonorShard"),
                                       destinationShard,
                                       true,
                                       outputCollectionNss);
        DBDirectClient client(_opCtx);
        fetcher.consume(&client);

        // Six oplog entries should be copied. Five inserts and the final no-op oplog entry.
        ASSERT_EQ(6, fetcher.getNumOplogEntriesCopied());
    }
};

class InterruptConsume : public ReshardingTest {
public:
    void run() {
        const NamespaceString outputCollectionNss("dbtests.outputCollection");
        const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

        const ShardId destinationShard("shard1");
        setupBasic(outputCollectionNss, dataCollectionNss, destinationShard);

        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IS);
        ReshardingOplogFetcher fetcher(_reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       ShardId("fakeDonorShard"),
                                       destinationShard,
                                       true,
                                       outputCollectionNss);

        // Interrupt the fetcher. A fetcher object owns its own client, but interruption does not
        // require the background job to be started.
        fetcher.setKilled();

        DBDirectClient client(_opCtx);
        ASSERT_THROWS(fetcher.consume(&client), ExceptionForCat<ErrorCategory::Interruption>);
    }
};

class AllReshardingTests : public unittest::OldStyleSuiteSpecification {
public:
    AllReshardingTests() : unittest::OldStyleSuiteSpecification("ReshardingTests") {}

    // Must be evaluated at test run() time, not static-init time.
    static bool shouldSkip() {
        // Only run on storage engines that support snapshot reads.
        auto storageEngine = cc().getServiceContext()->getStorageEngine();
        if (!storageEngine->supportsReadConcernSnapshot() ||
            !mongo::serverGlobalParams.enableMajorityReadConcern) {
            LOGV2(5123009,
                  "Skipping this test because the configuration does not support majority reads.",
                  "storageEngine"_attr = storageGlobalParams.engine,
                  "enableMajorityReadConcern"_attr =
                      mongo::serverGlobalParams.enableMajorityReadConcern);
            return true;
        }
        return false;
    }

    template <typename T>
    void addIf() {
        addNameCallback(nameForTestClass<T>(), [] {
            if (!shouldSkip())
                T().run();
        });
    }

    void setupTests() {
        addIf<RunFetchIteration>();
        addIf<RunConsume>();
        addIf<InterruptConsume>();
    }
};

unittest::OldStyleSuiteInitializer<AllReshardingTests> allReshardingTests;

}  // namespace
}  // namespace mongo
