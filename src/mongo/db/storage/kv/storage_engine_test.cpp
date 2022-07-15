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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_test_fixture.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/periodic_runner_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

TEST_F(StorageEngineTest, ReconcileIdentsTest) {
    auto opCtx = cc().makeOperationContext();

    // Add a collection, `db.coll1` to both the DurableCatalog and KVEngine. The returned value is
    // the `ident` name given to the collection.
    auto swCollInfo = createCollection(opCtx.get(), NamespaceString("db.coll1"));
    ASSERT_OK(swCollInfo.getStatus());

    // Create a table in the KVEngine not reflected in the DurableCatalog. This should be dropped
    // when reconciling.
    ASSERT_OK(createCollTable(opCtx.get(), NamespaceString("db.coll2")));

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());

    auto identsVec = getAllKVEngineIdents(opCtx.get());
    auto idents = std::set<std::string>(identsVec.begin(), identsVec.end());

    // There are two idents. `_mdb_catalog` and the ident for `db.coll1`.
    ASSERT_EQUALS(static_cast<const unsigned long>(2), idents.size());
    ASSERT_TRUE(idents.find(swCollInfo.getValue().ident) != idents.end());
    ASSERT_TRUE(idents.find("_mdb_catalog") != idents.end());

    // Drop the `db.coll1` table, while leaving the DurableCatalog entry.
    ASSERT_OK(dropIdent(opCtx.get()->recoveryUnit(), swCollInfo.getValue().ident));
    ASSERT_EQUALS(static_cast<const unsigned long>(1), getAllKVEngineIdents(opCtx.get()).size());

    // Reconciling this should result in an error.
    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_NOT_OK(reconcileStatus.getStatus());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, reconcileStatus.getStatus());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphansAfterUncleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());

    ASSERT_OK(dropIdent(opCtx.get()->recoveryUnit(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the collection has been dropped because the
    // KVEngine was started after an unclean shutdown but not in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeCatalog(opCtx.get());
        _storageEngine->loadCatalog(opCtx.get(), StorageEngine::LastShutdownState::kUnclean);
    }

    ASSERT(!identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(!collectionExists(opCtx.get(), collNs));
}

TEST_F(StorageEngineTest, TemporaryRecordStoreClustered) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    const auto trs = makeTemporaryClustered(opCtx.get());
    ASSERT(trs.get());
    const auto rs = trs->rs();
    ASSERT(identExists(opCtx.get(), rs->getIdent()));

    // Insert record with RecordId of KeyFormat::String.
    const auto id = StringData{"1"};
    const auto rid = RecordId(id.rawData(), id.size());
    const auto data = "data";
    WriteUnitOfWork wuow(opCtx.get());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), rid, data, strlen(data), Timestamp());
    ASSERT_TRUE(s.isOK());
    wuow.commit();

    // Read the record back.
    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx.get(), rid, &rd));
    ASSERT_EQ(0, memcmp(data, rd.data(), strlen(data)));
}

TEST_F(StorageEngineTest, ReconcileDropsTemporary) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    auto rs = makeTemporary(opCtx.get());
    ASSERT(rs.get());
    const std::string ident = rs->rs()->getIdent();

    ASSERT(identExists(opCtx.get(), ident));

    // Reconcile will only drop temporary idents when starting up after an unclean shutdown.
    auto reconcileResult = unittest::assertGet(reconcileAfterUncleanShutdown(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());

    // The storage engine is responsible for dropping its temporary idents.
    ASSERT(!identExists(opCtx.get(), ident));
}

TEST_F(StorageEngineTest, ReconcileKeepsTemporary) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    auto rs = makeTemporary(opCtx.get());
    ASSERT(rs.get());
    const std::string ident = rs->rs()->getIdent();

    ASSERT(identExists(opCtx.get(), ident));

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());

    ASSERT_FALSE(identExists(opCtx.get(), ident));
}

class StorageEngineTimestampMonitorTest : public StorageEngineTest {
public:
    void setUp() {
        StorageEngineTest::setUp();
        _storageEngine->startTimestampMonitor();
    }

    void waitForTimestampMonitorPass() {
        auto timestampMonitor =
            dynamic_cast<StorageEngineImpl*>(_storageEngine)->getTimestampMonitor();
        using TimestampType = StorageEngineImpl::TimestampMonitor::TimestampType;
        using TimestampListener = StorageEngineImpl::TimestampMonitor::TimestampListener;
        auto pf = makePromiseFuture<void>();
        auto listener =
            TimestampListener(TimestampType::kOldest, [promise = &pf.promise](Timestamp t) mutable {
                promise->emplaceValue();
            });
        timestampMonitor->addListener(&listener);
        pf.future.wait();
        timestampMonitor->removeListener_forTestOnly(&listener);
    }
};

TEST_F(StorageEngineTimestampMonitorTest, TemporaryRecordStoreEventuallyDropped) {
    auto opCtx = cc().makeOperationContext();

    std::string ident;
    {
        auto tempRs = _storageEngine->makeTemporaryRecordStore(opCtx.get(), KeyFormat::Long);
        ASSERT(tempRs.get());
        ident = tempRs->rs()->getIdent();

        ASSERT(identExists(opCtx.get(), ident));
    }

    // The temporary record store RAII object should queue itself to be dropped by the storage
    // engine eventually.
    waitForTimestampMonitorPass();
    ASSERT(!identExists(opCtx.get(), ident));
}

TEST_F(StorageEngineTimestampMonitorTest, TemporaryRecordStoreKeep) {
    auto opCtx = cc().makeOperationContext();

    std::string ident;
    {
        auto tempRs = _storageEngine->makeTemporaryRecordStore(opCtx.get(), KeyFormat::Long);
        ASSERT(tempRs.get());
        ident = tempRs->rs()->getIdent();

        ASSERT(identExists(opCtx.get(), ident));
        tempRs->keep();
    }

    // The ident for the record store should still exist even after a pass of the timestamp monitor.
    waitForTimestampMonitorPass();
    ASSERT(identExists(opCtx.get(), ident));
}

TEST_F(StorageEngineTest, ReconcileUnfinishedIndex) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_X);

    const NamespaceString ns("db.coll1");
    const std::string indexName("a_1");

    auto swCollInfo = createCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());


    // Start an non-backgroundSecondary single-phase (i.e. no build UUID) index.
    const bool isBackgroundSecondaryBuild = false;
    const boost::optional<UUID> buildUUID = boost::none;
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(
            startIndexBuild(opCtx.get(), ns, indexName, isBackgroundSecondaryBuild, buildUUID));
        wuow.commit();
    }

    const auto indexIdent = _storageEngine->getCatalog()->getIndexIdent(
        opCtx.get(), swCollInfo.getValue().catalogId, indexName);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // Reconcile should have to dropped the ident to allow the index to be rebuilt.
    ASSERT(!identExists(opCtx.get(), indexIdent));

    // Because this non-backgroundSecondary index is unfinished, reconcile will drop the index and
    // not require it to be rebuilt.
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());

    // There are no two-phase builds to resume or restart.
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());
}

TEST_F(StorageEngineTest, ReconcileUnfinishedBackgroundSecondaryIndex) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IX);

    const NamespaceString ns("db.coll1");
    const std::string indexName("a_1");

    auto swCollInfo = createCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());

    // Start a backgroundSecondary single-phase (i.e. no build UUID) index.
    const bool isBackgroundSecondaryBuild = true;
    const boost::optional<UUID> buildUUID = boost::none;
    {
        Lock::DBLock dbLk(opCtx.get(), ns.dbName(), MODE_IX);
        Lock::CollectionLock collLk(opCtx.get(), ns, MODE_X);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(
            startIndexBuild(opCtx.get(), ns, indexName, isBackgroundSecondaryBuild, buildUUID));
        wuow.commit();
    }

    const auto indexIdent = _storageEngine->getCatalog()->getIndexIdent(
        opCtx.get(), swCollInfo.getValue().catalogId, indexName);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // Reconcile should not have dropped the ident because it expects the caller will drop and
    // rebuild the index.
    ASSERT(identExists(opCtx.get(), indexIdent));

    // Because this backgroundSecondary index is unfinished, reconcile will drop the index and
    // require it to be rebuilt.
    ASSERT_EQUALS(1UL, reconcileResult.indexesToRebuild.size());
    StorageEngine::IndexIdentifier& toRebuild = reconcileResult.indexesToRebuild[0];
    ASSERT_EQUALS(ns.ns(), toRebuild.nss.ns());
    ASSERT_EQUALS(indexName, toRebuild.indexName);

    // There are no two-phase builds to restart or resume.
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());
}

TEST_F(StorageEngineTest, ReconcileTwoPhaseIndexBuilds) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IX);

    const NamespaceString ns("db.coll1");
    const std::string indexA("a_1");
    const std::string indexB("b_1");

    auto swCollInfo = createCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());

    // Using a build UUID implies that this index build is two-phase, so the isBackgroundSecondary
    // field will be ignored. There is no special behavior on primaries or secondaries.
    auto buildUUID = UUID::gen();
    const bool isBackgroundSecondaryBuild = false;

    // Start two indexes with the same buildUUID to simulate building multiple indexes within the
    // same build.
    {
        Lock::DBLock dbLk(opCtx.get(), ns.dbName(), MODE_IX);
        Lock::CollectionLock collLk(opCtx.get(), ns, MODE_X);
        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(
                startIndexBuild(opCtx.get(), ns, indexA, isBackgroundSecondaryBuild, buildUUID));
            wuow.commit();
        }
        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(
                startIndexBuild(opCtx.get(), ns, indexB, isBackgroundSecondaryBuild, buildUUID));
            wuow.commit();
        }
    }

    const auto indexIdentA = _storageEngine->getCatalog()->getIndexIdent(
        opCtx.get(), swCollInfo.getValue().catalogId, indexA);
    const auto indexIdentB = _storageEngine->getCatalog()->getIndexIdent(
        opCtx.get(), swCollInfo.getValue().catalogId, indexB);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // Reconcile should not have dropped the ident to allow the restarted index build to do so
    // transactionally with the start.
    ASSERT(identExists(opCtx.get(), indexIdentA));
    ASSERT(identExists(opCtx.get(), indexIdentB));

    // Because this is an unfinished two-phase index build, reconcile will not require this index to
    // be rebuilt to completion, rather restarted.
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());

    // Only one index build should be indicated as needing to be restarted.
    ASSERT_EQUALS(1UL, reconcileResult.indexBuildsToRestart.size());
    auto& [toRestartBuildUUID, toRestart] = *reconcileResult.indexBuildsToRestart.begin();
    ASSERT_EQ(buildUUID, toRestartBuildUUID);

    // Both specs should be listed within the same build.
    auto& specs = toRestart.indexSpecs;
    ASSERT_EQ(2UL, specs.size());
    ASSERT_EQ(indexA, specs[0]["name"].str());
    ASSERT_EQ(indexB, specs[1]["name"].str());

    // There should be no index builds to resume.
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());
}

#ifndef _WIN32  // WiredTiger does not support orphan file recovery on Windows.
TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());

    // Drop the ident from the storage engine but keep the underlying files.
    _storageEngine->getEngine()->dropIdentForImport(opCtx.get(), swCollInfo.getValue().ident);
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the ident has been recovered because the
    // KVEngine was started in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeCatalog(opCtx.get());
        _storageEngine->loadCatalog(opCtx.get(), StorageEngine::LastShutdownState::kClean);
    }

    ASSERT(identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}
#endif

TEST_F(StorageEngineRepairTest, ReconcileSucceeds) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());

    ASSERT_OK(dropIdent(opCtx.get()->recoveryUnit(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // Reconcile would normally return an error if a collection existed with a missing ident in the
    // storage engine. When in a repair context, that should not be the case.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());

    ASSERT(!identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(0U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphansInCatalog) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());
    ASSERT(collectionExists(opCtx.get(), collNs));

    AutoGetDb db(opCtx.get(), collNs.dbName(), LockMode::MODE_X);
    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(removeEntry(opCtx.get(), collNs.ns(), _storageEngine->getCatalog()));
        wuow.commit();
    }

    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a repair context, loadCatalog() recreates catalog entries for orphaned idents.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->loadCatalog(opCtx.get(), StorageEngine::LastShutdownState::kClean);
    }
    auto identNs = swCollInfo.getValue().ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs = NamespaceString("local.orphan." + identNs);

    ASSERT(identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), orphanNs));

    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());
    ASSERT(collectionExists(opCtx.get(), collNs));

    AutoGetDb db(opCtx.get(), collNs.dbName(), LockMode::MODE_X);
    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(removeEntry(opCtx.get(), collNs.ns(), _storageEngine->getCatalog()));
        wuow.commit();
    }
    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a normal startup context, loadCatalog() does not recreate catalog entries for
    // orphaned idents.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->loadCatalog(opCtx.get(), StorageEngine::LastShutdownState::kClean);
    }
    // reconcileCatalogAndIdents() drops orphaned idents.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexesToRebuild.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());

    ASSERT(!identExists(opCtx.get(), swCollInfo.getValue().ident));
    auto identNs = swCollInfo.getValue().ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs = NamespaceString("local.orphan." + identNs);
    ASSERT(!collectionExists(opCtx.get(), orphanNs));
}

/**
 * A test-only mock storage engine supporting timestamps.
 */
class TimestampMockKVEngine final : public DevNullKVEngine {
public:
    bool supportsRecoveryTimestamp() const override {
        return true;
    }

    // Increment the timestamps each time they are called for testing purposes.
    virtual Timestamp getCheckpointTimestamp() const override {
        checkpointTimestamp = std::make_unique<Timestamp>(checkpointTimestamp->getInc() + 1);
        return *checkpointTimestamp;
    }
    virtual Timestamp getOldestTimestamp() const override {
        oldestTimestamp = std::make_unique<Timestamp>(oldestTimestamp->getInc() + 1);
        return *oldestTimestamp;
    }
    virtual Timestamp getStableTimestamp() const override {
        stableTimestamp = std::make_unique<Timestamp>(stableTimestamp->getInc() + 1);
        return *stableTimestamp;
    }

    // Mutable for testing purposes to increment the timestamp.
    mutable std::unique_ptr<Timestamp> checkpointTimestamp = std::make_unique<Timestamp>();
    mutable std::unique_ptr<Timestamp> oldestTimestamp = std::make_unique<Timestamp>();
    mutable std::unique_ptr<Timestamp> stableTimestamp = std::make_unique<Timestamp>();
};

class TimestampKVEngineTest : public ServiceContextTest {
public:
    using TimestampType = StorageEngineImpl::TimestampMonitor::TimestampType;
    using TimestampListener = StorageEngineImpl::TimestampMonitor::TimestampListener;

    /**
     * Create an instance of the KV Storage Engine so that we have a timestamp monitor operating.
     */
    void setUp() {
        ServiceContextTest::setUp();

        auto opCtx = makeOperationContext();

        auto runner = makePeriodicRunner(getServiceContext());
        getServiceContext()->setPeriodicRunner(std::move(runner));

        StorageEngineOptions options{/*directoryPerDB=*/false,
                                     /*directoryForIndexes=*/false,
                                     /*forRepair=*/false,
                                     /*lockFileCreatedByUncleanShutdown=*/false};
        _storageEngine = std::make_unique<StorageEngineImpl>(
            opCtx.get(), std::make_unique<TimestampMockKVEngine>(), options);
        _storageEngine->startTimestampMonitor();
    }

    void tearDown() {
        _storageEngine->cleanShutdown();
        _storageEngine.reset();

        ServiceContextTest::tearDown();
    }

    std::unique_ptr<StorageEngineImpl> _storageEngine;

    TimestampType checkpoint = TimestampType::kCheckpoint;
    TimestampType oldest = TimestampType::kOldest;
    TimestampType stable = TimestampType::kStable;
};

TEST_F(TimestampKVEngineTest, TimestampMonitorRunning) {
    // The timestamp monitor should only be running if the storage engine supports timestamps.
    if (!_storageEngine->getEngine()->supportsRecoveryTimestamp())
        return;

    ASSERT_TRUE(_storageEngine->getTimestampMonitor()->isRunning_forTestOnly());
}

TEST_F(TimestampKVEngineTest, TimestampListeners) {
    TimestampListener first(stable, [](Timestamp timestamp) {});
    TimestampListener second(oldest, [](Timestamp timestamp) {});
    TimestampListener third(stable, [](Timestamp timestamp) {});

    // Can only register the listener once.
    _storageEngine->getTimestampMonitor()->addListener(&first);

    _storageEngine->getTimestampMonitor()->clearListeners();
    _storageEngine->getTimestampMonitor()->addListener(&first);

    // Can register all three types of listeners.
    _storageEngine->getTimestampMonitor()->addListener(&second);
    _storageEngine->getTimestampMonitor()->addListener(&third);

    _storageEngine->getTimestampMonitor()->clearListeners();
}

TEST_F(TimestampKVEngineTest, TimestampMonitorNotifiesListeners) {
    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    bool changes[4] = {false, false, false, false};

    TimestampListener first(checkpoint, [&](Timestamp timestamp) {
        stdx::lock_guard<Latch> lock(mutex);
        if (!changes[0]) {
            changes[0] = true;
            cv.notify_all();
        }
    });

    TimestampListener second(oldest, [&](Timestamp timestamp) {
        stdx::lock_guard<Latch> lock(mutex);
        if (!changes[1]) {
            changes[1] = true;
            cv.notify_all();
        }
    });

    TimestampListener third(stable, [&](Timestamp timestamp) {
        stdx::lock_guard<Latch> lock(mutex);
        if (!changes[2]) {
            changes[2] = true;
            cv.notify_all();
        }
    });

    TimestampListener fourth(stable, [&](Timestamp timestamp) {
        stdx::lock_guard<Latch> lock(mutex);
        if (!changes[3]) {
            changes[3] = true;
            cv.notify_all();
        }
    });

    _storageEngine->getTimestampMonitor()->addListener(&first);
    _storageEngine->getTimestampMonitor()->addListener(&second);
    _storageEngine->getTimestampMonitor()->addListener(&third);
    _storageEngine->getTimestampMonitor()->addListener(&fourth);

    // Wait until all 4 listeners get notified at least once.
    stdx::unique_lock<Latch> lk(mutex);
    cv.wait(lk, [&] {
        for (auto const& change : changes) {
            if (!change) {
                return false;
            }
        }
        return true;
    });

    _storageEngine->getTimestampMonitor()->clearListeners();
}

TEST_F(TimestampKVEngineTest, TimestampAdvancesOnNotification) {
    Timestamp previous = Timestamp();
    AtomicWord<int> timesNotified{0};

    TimestampListener listener(stable, [&](Timestamp timestamp) {
        ASSERT_TRUE(previous < timestamp);
        previous = timestamp;
        timesNotified.fetchAndAdd(1);
    });
    _storageEngine->getTimestampMonitor()->addListener(&listener);

    // Let three rounds of notifications happen while ensuring that each new notification produces
    // an increasing timestamp.
    while (timesNotified.load() < 3) {
        sleepmillis(100);
    }

    _storageEngine->getTimestampMonitor()->clearListeners();
}

TEST_F(StorageEngineTestNotEphemeral, UseAlternateStorageLocation) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString coll1Ns("db.coll1");
    const NamespaceString coll2Ns("db.coll2");
    auto swCollInfo = createCollection(opCtx.get(), coll1Ns);
    ASSERT_OK(swCollInfo.getStatus());
    ASSERT(collectionExists(opCtx.get(), coll1Ns));
    ASSERT_FALSE(collectionExists(opCtx.get(), coll2Ns));

    LOGV2(5781102, "Starting up storage engine in alternate location");
    const auto oldPath = storageGlobalParams.dbpath;
    const auto newPath = boost::filesystem::path(oldPath).append(".alternate").string();
    boost::filesystem::create_directory(newPath);
    auto lastShutdownState =
        reinitializeStorageEngine(opCtx.get(), StorageEngineInitFlags{}, [&newPath] {
            storageGlobalParams.dbpath = newPath;
        });
    getGlobalServiceContext()->getStorageEngine()->notifyStartupComplete();
    LOGV2(5781103, "Started up storage engine in alternate location");
    ASSERT(StorageEngine::LastShutdownState::kClean == lastShutdownState);
    StorageEngineTest::_storageEngine = getServiceContext()->getStorageEngine();
    // Alternate storage location should have no collections.
    ASSERT_FALSE(collectionExists(opCtx.get(), coll1Ns));
    ASSERT_FALSE(collectionExists(opCtx.get(), coll2Ns));

    swCollInfo = createCollection(opCtx.get(), coll2Ns);
    ASSERT_OK(swCollInfo.getStatus());
    ASSERT_FALSE(collectionExists(opCtx.get(), coll1Ns));
    ASSERT_TRUE(collectionExists(opCtx.get(), coll2Ns));

    LOGV2(5781104, "Starting up storage engine in original location");
    lastShutdownState =
        reinitializeStorageEngine(opCtx.get(), StorageEngineInitFlags{}, [&oldPath] {
            storageGlobalParams.dbpath = oldPath;
        });
    getGlobalServiceContext()->getStorageEngine()->notifyStartupComplete();
    ASSERT(StorageEngine::LastShutdownState::kClean == lastShutdownState);
    StorageEngineTest::_storageEngine = getServiceContext()->getStorageEngine();
    ASSERT_TRUE(collectionExists(opCtx.get(), coll1Ns));
    ASSERT_FALSE(collectionExists(opCtx.get(), coll2Ns));
}
}  // namespace
}  // namespace mongo
