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
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_engine.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/storage_engine_impl.h"
#include "mongo/db/storage/kv/storage_engine_test_fixture.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/unclean_shutdown.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/periodic_runner_factory.h"

namespace mongo {
namespace {

TEST_F(StorageEngineTest, ReconcileIdentsTest) {
    auto opCtx = cc().makeOperationContext();

    // Add a collection, `db.coll1` to both the KVCatalog and KVEngine. The returned value is the
    // `ident` name given to the collection.
    auto swIdentName = createCollection(opCtx.get(), NamespaceString("db.coll1"));
    ASSERT_OK(swIdentName);
    // Create a table in the KVEngine not reflected in the KVCatalog. This should be dropped when
    // reconciling.
    ASSERT_OK(createCollTable(opCtx.get(), NamespaceString("db.coll2")));
    ASSERT_OK(reconcile(opCtx.get()).getStatus());
    auto identsVec = getAllKVEngineIdents(opCtx.get());
    auto idents = std::set<std::string>(identsVec.begin(), identsVec.end());
    // There are two idents. `_mdb_catalog` and the ident for `db.coll1`.
    ASSERT_EQUALS(static_cast<const unsigned long>(2), idents.size());
    ASSERT_TRUE(idents.find(swIdentName.getValue()) != idents.end());
    ASSERT_TRUE(idents.find("_mdb_catalog") != idents.end());

    // Create a catalog entry for the `_id` index. Drop the created the table.
    ASSERT_OK(createIndex(
        opCtx.get(), NamespaceString("db.coll1"), "_id", false /* isBackgroundSecondaryBuild */));
    ASSERT_OK(dropIndexTable(opCtx.get(), NamespaceString("db.coll1"), "_id"));
    // The reconcile response should include this index as needing to be rebuilt.
    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_OK(reconcileStatus.getStatus());
    ASSERT_EQUALS(static_cast<const unsigned long>(1), reconcileStatus.getValue().size());
    StorageEngine::CollectionIndexNamePair& toRebuild = reconcileStatus.getValue()[0];
    ASSERT_EQUALS("db.coll1", toRebuild.first);
    ASSERT_EQUALS("_id", toRebuild.second);

    // Now drop the `db.coll1` table, while leaving the KVCatalog entry.
    ASSERT_OK(dropIdent(opCtx.get(), swIdentName.getValue()));
    ASSERT_EQUALS(static_cast<const unsigned long>(1), getAllKVEngineIdents(opCtx.get()).size());

    // Reconciling this should result in an error.
    reconcileStatus = reconcile(opCtx.get());
    ASSERT_NOT_OK(reconcileStatus.getStatus());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, reconcileStatus.getStatus());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphansAfterUncleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swIdentName = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swIdentName);

    ASSERT_OK(dropIdent(opCtx.get(), swIdentName.getValue()));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the collection has been dropped because the
    // KVEngine was started after an unclean shutdown but not in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeCatalog(opCtx.get());
        startingAfterUncleanShutdown(getGlobalServiceContext()) = true;
        _storageEngine->loadCatalog(opCtx.get());
    }

    ASSERT(!identExists(opCtx.get(), swIdentName.getValue()));
    ASSERT(!collectionExists(opCtx.get(), collNs));
}

TEST_F(StorageEngineTest, ReconcileDropsTemporary) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    auto rs = makeTemporary(opCtx.get());
    ASSERT(rs.get());
    const std::string ident = rs->rs()->getIdent();

    ASSERT(identExists(opCtx.get(), ident));

    ASSERT_OK(reconcile(opCtx.get()).getStatus());

    // The storage engine is responsible for dropping its temporary idents.
    ASSERT(!identExists(opCtx.get(), ident));

    rs->deleteTemporaryTable(opCtx.get());
}

TEST_F(StorageEngineTest, TemporaryDropsItself) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    std::string ident;
    {
        auto rs = makeTemporary(opCtx.get());
        ASSERT(rs.get());
        ident = rs->rs()->getIdent();

        ASSERT(identExists(opCtx.get(), ident));

        rs->deleteTemporaryTable(opCtx.get());
    }

    // The temporary record store RAII class should drop itself.
    ASSERT(!identExists(opCtx.get(), ident));
}

TEST_F(StorageEngineTest, ReconcileDoesNotDropIndexBuildTempTables) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    const NamespaceString ns("db.coll1");
    const std::string indexName("a_1");

    auto swIdentName = createCollection(opCtx.get(), ns);
    ASSERT_OK(swIdentName);

    const bool isBackgroundSecondaryBuild = false;
    ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, isBackgroundSecondaryBuild));

    auto sideWrites = makeTemporary(opCtx.get());
    auto constraintViolations = makeTemporary(opCtx.get());

    const auto indexIdent = _storageEngine->getCatalog()->getIndexIdent(opCtx.get(), ns, indexName);

    indexBuildScan(opCtx.get(),
                   ns,
                   indexName,
                   sideWrites->rs()->getIdent(),
                   constraintViolations->rs()->getIdent());
    indexBuildDrain(opCtx.get(), ns, indexName);

    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_OK(reconcileStatus.getStatus());
    ASSERT(!identExists(opCtx.get(), indexIdent));

    // Because this non-backgroundSecondary index is unfinished, reconcile will drop the index.
    ASSERT_EQUALS(0UL, reconcileStatus.getValue().size());

    // The owning index was dropped, and so should its temporary tables.
    ASSERT(!identExists(opCtx.get(), sideWrites->rs()->getIdent()));
    ASSERT(!identExists(opCtx.get(), constraintViolations->rs()->getIdent()));

    sideWrites->deleteTemporaryTable(opCtx.get());
    constraintViolations->deleteTemporaryTable(opCtx.get());
}

TEST_F(StorageEngineTest, ReconcileDoesNotDropIndexBuildTempTablesBackgroundSecondary) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);

    const NamespaceString ns("db.coll1");
    const std::string indexName("a_1");

    auto swIdentName = createCollection(opCtx.get(), ns);
    ASSERT_OK(swIdentName);

    const bool isBackgroundSecondaryBuild = true;
    ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, isBackgroundSecondaryBuild));

    auto sideWrites = makeTemporary(opCtx.get());
    auto constraintViolations = makeTemporary(opCtx.get());

    const auto indexIdent = _storageEngine->getCatalog()->getIndexIdent(opCtx.get(), ns, indexName);

    indexBuildScan(opCtx.get(),
                   ns,
                   indexName,
                   sideWrites->rs()->getIdent(),
                   constraintViolations->rs()->getIdent());
    indexBuildDrain(opCtx.get(), ns, indexName);

    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_OK(reconcileStatus.getStatus());
    ASSERT(identExists(opCtx.get(), indexIdent));

    // Because this backgroundSecondary index is unfinished, reconcile will identify that it should
    // be rebuilt.
    ASSERT_EQUALS(1UL, reconcileStatus.getValue().size());
    StorageEngine::CollectionIndexNamePair& toRebuild = reconcileStatus.getValue()[0];
    ASSERT_EQUALS(ns.toString(), toRebuild.first);
    ASSERT_EQUALS(indexName, toRebuild.second);

    // Because these temporary idents were associated with an in-progress index build, they are not
    // dropped.
    ASSERT(identExists(opCtx.get(), sideWrites->rs()->getIdent()));
    ASSERT(identExists(opCtx.get(), constraintViolations->rs()->getIdent()));

    sideWrites->deleteTemporaryTable(opCtx.get());
    constraintViolations->deleteTemporaryTable(opCtx.get());
}

TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swIdentName = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swIdentName);

    ASSERT_OK(dropIdent(opCtx.get(), swIdentName.getValue()));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the ident has been recovered because the
    // KVEngine was started in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeCatalog(opCtx.get());
        _storageEngine->loadCatalog(opCtx.get());
    }

    ASSERT(identExists(opCtx.get(), swIdentName.getValue()));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineRepairTest, ReconcileSucceeds) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swIdentName = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swIdentName);

    ASSERT_OK(dropIdent(opCtx.get(), swIdentName.getValue()));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // Reconcile would normally return an error if a collection existed with a missing ident in the
    // storage engine. When in a repair context, that should not be the case.
    ASSERT_OK(reconcile(opCtx.get()).getStatus());

    ASSERT(!identExists(opCtx.get(), swIdentName.getValue()));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(0U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphansInCatalog) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swIdentName = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swIdentName);
    ASSERT(collectionExists(opCtx.get(), collNs));

    AutoGetDb db(opCtx.get(), collNs.db(), LockMode::MODE_X);
    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    ASSERT_OK(removeEntry(opCtx.get(), collNs.ns(), _storageEngine->getCatalog()));
    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a repair context, loadCatalog() recreates catalog entries for orphaned idents.
    _storageEngine->loadCatalog(opCtx.get());
    auto identNs = swIdentName.getValue();
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs = NamespaceString("local.orphan." + identNs);

    ASSERT(identExists(opCtx.get(), swIdentName.getValue()));
    ASSERT(collectionExists(opCtx.get(), orphanNs));

    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs("db.coll1");
    auto swIdentName = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swIdentName);
    ASSERT(collectionExists(opCtx.get(), collNs));

    AutoGetDb db(opCtx.get(), collNs.db(), LockMode::MODE_X);
    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    ASSERT_OK(removeEntry(opCtx.get(), collNs.ns(), _storageEngine->getCatalog()));
    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a normal startup context, loadCatalog() does not recreate catalog entries for
    // orphaned idents.
    _storageEngine->loadCatalog(opCtx.get());
    // reconcileCatalogAndIdents() drops orphaned idents.
    ASSERT_OK(reconcile(opCtx.get()).getStatus());

    ASSERT(!identExists(opCtx.get(), swIdentName.getValue()));
    auto identNs = swIdentName.getValue();
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

class TimestampKVEngineTest : public ServiceContextMongoDTest {
public:
    using TimestampType = StorageEngineImpl::TimestampMonitor::TimestampType;
    using TimestampListener = StorageEngineImpl::TimestampMonitor::TimestampListener;

    /**
     * Create an instance of the KV Storage Engine so that we have a timestamp monitor operating.
     */
    TimestampKVEngineTest() {
        StorageEngineOptions options{
            /*directoryPerDB=*/false, /*directoryForIndexes=*/false, /*forRepair=*/false};
        _storageEngine = std::make_unique<StorageEngineImpl>(new TimestampMockKVEngine, options);
        _storageEngine->finishInit();
    }

    ~TimestampKVEngineTest() {
        _storageEngine->cleanShutdown();
        _storageEngine.reset();
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

    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->addListener(&first);

    // Can register all three types of listeners.
    _storageEngine->getTimestampMonitor()->addListener(&second);
    _storageEngine->getTimestampMonitor()->addListener(&third);

    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->removeListener(&second);
    _storageEngine->getTimestampMonitor()->removeListener(&third);
}

TEST_F(TimestampKVEngineTest, TimestampMonitorNotifiesListeners) {
    stdx::mutex mutex;
    stdx::condition_variable cv;

    bool changes[4] = {false, false, false, false};

    TimestampListener first(checkpoint, [&](Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!changes[0]) {
            changes[0] = true;
            cv.notify_all();
        }
    });

    TimestampListener second(oldest, [&](Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!changes[1]) {
            changes[1] = true;
            cv.notify_all();
        }
    });

    TimestampListener third(stable, [&](Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!changes[2]) {
            changes[2] = true;
            cv.notify_all();
        }
    });

    TimestampListener fourth(stable, [&](Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
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
    stdx::unique_lock<stdx::mutex> lk(mutex);
    cv.wait(lk, [&] {
        for (auto const& change : changes) {
            if (!change) {
                return false;
            }
        }
        return true;
    });

    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->removeListener(&second);
    _storageEngine->getTimestampMonitor()->removeListener(&third);
    _storageEngine->getTimestampMonitor()->removeListener(&fourth);
}

TEST_F(TimestampKVEngineTest, TimestampAdvancesOnNotification) {
    Timestamp previous = Timestamp();
    int timesNotified = 0;

    TimestampListener listener(stable, [&](Timestamp timestamp) {
        ASSERT_TRUE(previous < timestamp);
        previous = timestamp;
        timesNotified++;
    });
    _storageEngine->getTimestampMonitor()->addListener(&listener);

    // Let three rounds of notifications happen while ensuring that each new notification produces
    // an increasing timestamp.
    while (timesNotified < 3) {
        sleepmillis(100);
    }

    _storageEngine->getTimestampMonitor()->removeListener(&listener);
}

}  // namespace
}  // namespace mongo
