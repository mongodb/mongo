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

#include <absl/container/node_hash_map.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/index_builds/index_builds.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_test_fixture.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

TEST_F(StorageEngineTest, ReconcileIdentsTest) {
    auto opCtx = cc().makeOperationContext();

    // Add a collection, `db.coll1` to both the DurableCatalog and KVEngine. The returned value is
    // the `ident` name given to the collection.
    auto swCollInfo =
        createCollection(opCtx.get(), NamespaceString::createNamespaceString_forTest("db.coll1"));
    ASSERT_OK(swCollInfo.getStatus());

    // Create a table in the KVEngine not reflected in the DurableCatalog. This should be dropped
    // when reconciling.
    ASSERT_OK(
        createCollTable(opCtx.get(), NamespaceString::createNamespaceString_forTest("db.coll2")));

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());

    auto identsVec = getAllKVEngineIdents(opCtx.get());
    auto idents = std::set<std::string>(identsVec.begin(), identsVec.end());

    // There are two idents. `_mdb_catalog` and the ident for `db.coll1`.
    ASSERT_EQUALS(static_cast<const unsigned long>(2), idents.size());
    ASSERT_TRUE(idents.find(swCollInfo.getValue().ident) != idents.end());
    ASSERT_TRUE(idents.find("_mdb_catalog") != idents.end());

    // Drop the `db.coll1` table, while leaving the DurableCatalog entry.
    ASSERT_OK(dropIdent(shard_role_details::getRecoveryUnit(opCtx.get()),
                        swCollInfo.getValue().ident,
                        /*identHasSizeInfo=*/true));
    ASSERT_EQUALS(static_cast<const unsigned long>(1), getAllKVEngineIdents(opCtx.get()).size());

    // Reconciling this should result in an error.
    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_NOT_OK(reconcileStatus.getStatus());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, reconcileStatus.getStatus());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphansAfterUncleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());

    ASSERT_OK(dropIdent(shard_role_details::getRecoveryUnit(opCtx.get()),
                        swCollInfo.getValue().ident,
                        /*identHasSizeInfo=*/true));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the collection has been dropped because the
    // KVEngine was started after an unclean shutdown but not in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeCatalog(opCtx.get());
        _storageEngine->loadCatalog(
            opCtx.get(), boost::none, StorageEngine::LastShutdownState::kUnclean);
    }

    ASSERT(!identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(!collectionExists(opCtx.get(), collNs));
}

TEST_F(StorageEngineTest, TemporaryRecordStoreClustered) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);
    auto trs = makeTemporaryClustered(opCtx.get());
    ASSERT(trs.get());

    auto rs = trs->rs();
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

class StorageEngineReconcileTest : public StorageEngineTest {
protected:
    UUID collectionUUID = UUID::gen();
    UUID buildUUID = UUID::gen();
    std::string resumableIndexFileName = "foo";

    // Makes an empty internal table.
    std::unique_ptr<TemporaryRecordStore> makeInternalTable(OperationContext* opCtx) {
        std::unique_ptr<TemporaryRecordStore> ret;
        {
            Lock::GlobalLock lk(opCtx, MODE_IS);
            ret = makeTemporary(opCtx);
        }
        ASSERT_TRUE(identExists(opCtx, ret->rs()->getIdent()));
        return ret;
    }

    std::string prepareIndexBuild(OperationContext* opCtx, BSONObj& indexSpec) {
        // Creates a collection.
        auto ns = NamespaceString::createNamespaceString_forTest("db.coll1");
        auto swCollInfo = createCollection(opCtx, ns);
        ASSERT_OK(swCollInfo.getStatus());
        auto catalogId = swCollInfo.getValue().catalogId;
        auto uuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, ns);
        ASSERT(uuid);
        collectionUUID = *uuid;

        // Starts an index build on the collection.
        auto indexName = "a_1";
        {
            Lock::GlobalLock lk(opCtx, MODE_IX);
            Lock::DBLock dbLk(opCtx, ns.dbName(), MODE_IX);
            Lock::CollectionLock collLk(opCtx, ns, MODE_X);
            WriteUnitOfWork wuow(opCtx);
            ASSERT_OK(startIndexBuild(opCtx, ns, indexName, buildUUID));
            wuow.commit();
        }
        auto md = _storageEngine->getCatalog()->getParsedCatalogEntry(opCtx, catalogId)->metadata;
        auto offset = md->findIndexOffset(indexName);
        indexSpec = md->indexes[offset].spec;
        return _storageEngine->getCatalog()->getIndexIdent(opCtx, catalogId, indexName);
    }

    // Makes an internal table that contains index-resume metadata, where |pretendSideTable| is an
    // internal table used for that resume.
    std::unique_ptr<TemporaryRecordStore> makeIndexBuildResumeTable(
        OperationContext* opCtx,
        const TemporaryRecordStore& pretendSideTable,
        const BSONObj& indexSpec = {}) {
        std::unique_ptr<TemporaryRecordStore> ret;
        {
            Lock::GlobalLock lk(opCtx, MODE_IX);
            ret = _storageEngine->makeTemporaryRecordStoreForResumableIndexBuild(opCtx,
                                                                                 KeyFormat::Long);
            BSONObj resInfo = makePretendResumeInfo(pretendSideTable, indexSpec);
            WriteUnitOfWork wuow(opCtx);
            ASSERT_OK(
                ret->rs()->insertRecord(opCtx, resInfo.objdata(), resInfo.objsize(), Timestamp()));
            wuow.commit();
        }
        ASSERT_TRUE(identExists(opCtx, ret->rs()->getIdent()));
        return ret;
    }

    // Returns index-resume metadata which would use the given |pretendSideTable| in the index's
    // build.
    BSONObj makePretendResumeInfo(const TemporaryRecordStore& pretendSideTable,
                                  const BSONObj& indexSpec) {
        IndexStateInfo indexInfo;
        indexInfo.setSpec(indexSpec);
        indexInfo.setIsMultikey({});
        indexInfo.setMultikeyPaths({});
        indexInfo.setSideWritesTable(pretendSideTable.rs()->getIdent());
        indexInfo.setFileName(resumableIndexFileName);
        indexInfo.setRanges({{}});
        ResumeIndexInfo resumeInfo;
        resumeInfo.setBuildUUID(buildUUID);
        resumeInfo.setCollectionUUID(collectionUUID);
        resumeInfo.setPhase({});
        resumeInfo.setIndexes(std::vector<IndexStateInfo>{std::move(indexInfo)});
        return resumeInfo.toBSON();
    }
};

TEST_F(StorageEngineReconcileTest, ReconcileDropsAllIdentsForUncleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<TemporaryRecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs);

    // Reconcile will drop all temporary idents when starting up after an unclean shutdown.
    auto reconcileResult = unittest::assertGet(reconcileAfterUncleanShutdown(opCtx.get()));

    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());
    ASSERT_FALSE(identExists(opCtx.get(), irrelevantRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), resumableIndexRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), necessaryRs->rs()->getIdent()));
}

TEST_F(StorageEngineReconcileTest, ReconcileOnlyKeepsNecessaryIdentsForCleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<TemporaryRecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // After clean shutdown, an internal ident should be kept if-and-only-if it is needed to resume
    // an index build.
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(1UL, reconcileResult.indexBuildsToResume.size());
    ASSERT_FALSE(identExists(opCtx.get(), irrelevantRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), resumableIndexRs->rs()->getIdent()));
    ASSERT_TRUE(identExists(opCtx.get(), necessaryRs->rs()->getIdent()));
}

void createTempFiles(const boost::filesystem::path& tempDir,
                     boost::optional<const boost::filesystem::path&> indexFile = boost::none,
                     boost::optional<const boost::filesystem::path&> irrelevantFile = boost::none) {
    boost::filesystem::remove_all(tempDir);
    ASSERT_TRUE(boost::filesystem::create_directory(tempDir));
    if (indexFile) {
        std::ofstream file(indexFile->string());
        ASSERT_TRUE(boost::filesystem::exists(*indexFile));
    }
    if (irrelevantFile) {
        std::ofstream file(irrelevantFile->string());
        ASSERT_TRUE(boost::filesystem::exists(*irrelevantFile));
    }
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryForUncleanShutdown) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<TemporaryRecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs);

    // Test cleanup of temporary directory used by resumable index build
    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    createTempFiles(tempDir);

    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kUnclean);

    // tempDir is not used and completely cleared when starting up after an unclean shutdown.
    ASSERT_FALSE(boost::filesystem::exists(tempDir));

    // Reconcile will drop all temporary idents when starting up after an unclean shutdown.
    ASSERT_FALSE(identExists(opCtx.get(), irrelevantRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), resumableIndexRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), necessaryRs->rs()->getIdent()));
}

// Abort the two-phase index build since it hangs in vote submission, because we are not running
// a full featured mongodb replica set.
void abortIndexBuild(OperationContext* opCtx, const UUID& buildUUID) {
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    // Pretend initial sync mode, otherwise abort is not allowed as a Secondary.
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx)->setFollowerMode(repl::MemberState::RS_STARTUP2));
    ASSERT_TRUE(IndexBuildsCoordinator::get(opCtx)->abortIndexBuildByBuildUUID(
        opCtx, buildUUID, IndexBuildAction::kInitialSyncAbort, "Shutdown"));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryResumableIndexForCleanShutdown) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<TemporaryRecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> necessaryRs = makeInternalTable(opCtx.get());
    BSONObj indexSpec;
    auto indexIdent = prepareIndexBuild(opCtx.get(), indexSpec);
    std::unique_ptr<TemporaryRecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs, indexSpec);

    // Test cleanup of temporary directory used by resumable index build
    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    auto irrelevantFile = tempDir / "garbage";
    // Create a file for resumable index build
    auto indexFile = tempDir / resumableIndexFileName;
    createTempFiles(tempDir, indexFile, irrelevantFile);

    ScopeGuard abortIndexOnExit([this, &opCtx] { abortIndexBuild(opCtx.get(), buildUUID); });
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);

    // tempDir is cleared except for files for resumable builds.
    ASSERT_TRUE(boost::filesystem::exists(tempDir));
    ASSERT_TRUE(boost::filesystem::exists(indexFile));
    ASSERT_FALSE(boost::filesystem::exists(irrelevantFile));

    // After clean shutdown, an internal ident should be kept if-and-only-if it is needed to resume
    // an index build.
    ASSERT_FALSE(identExists(opCtx.get(), irrelevantRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), resumableIndexRs->rs()->getIdent()));
    ASSERT_TRUE(identExists(opCtx.get(), necessaryRs->rs()->getIdent()));
    ASSERT_TRUE(identExists(opCtx.get(), indexIdent));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryResumableIndexFallbackToRestart) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<TemporaryRecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<TemporaryRecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::string indexIdent;
    {
        BSONObj unusedSpec;
        indexIdent = prepareIndexBuild(opCtx.get(), unusedSpec);
    }

    // Use an empty indexSpec which is invalid to resume.
    // The resumable index build will fail and fall back to restart.
    std::unique_ptr<TemporaryRecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs);

    // Test cleanup of temporary directory used by resumable index build
    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    auto indexFile = tempDir / resumableIndexFileName;
    createTempFiles(tempDir, indexFile);

    ScopeGuard abortIndexOnExit([this, &opCtx] { abortIndexBuild(opCtx.get(), buildUUID); });
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);

    ASSERT_TRUE(boost::filesystem::exists(tempDir));
    // When resumable index build fails its temp file is removed.
    ASSERT_FALSE(boost::filesystem::exists(indexFile));

    ASSERT_FALSE(identExists(opCtx.get(), irrelevantRs->rs()->getIdent()));
    ASSERT_FALSE(identExists(opCtx.get(), resumableIndexRs->rs()->getIdent()));
    ASSERT_TRUE(identExists(opCtx.get(), necessaryRs->rs()->getIdent()));
    ASSERT_TRUE(identExists(opCtx.get(), indexIdent));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryRestartIndexForCleanShutdown) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::string indexIdent;
    {
        BSONObj unusedSpec;
        indexIdent = prepareIndexBuild(opCtx.get(), unusedSpec);
    }

    // Test cleanup of temporary directory used by resumable index build
    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    createTempFiles(tempDir);

    ScopeGuard abortIndexOnExit([this, &opCtx] { abortIndexBuild(opCtx.get(), buildUUID); });
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);

    // tempDir is removed because there is no resumable build.
    ASSERT_FALSE(boost::filesystem::exists(tempDir));

    ASSERT(identExists(opCtx.get(), indexIdent));
}

TEST_F(StorageEngineTest, StartupRecoveryBuildMissingIdIndex) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();
    auto ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());

    auto coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), ns);
    ASSERT(coll);
    // _id index is missing initially.
    ASSERT_FALSE(coll->getIndexCatalog()->findIdIndex(opCtx.get()));
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);
    coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), ns);
    ASSERT(coll);
    // _id index is built after recovery.
    ASSERT(coll->getIndexCatalog()->findIdIndex(opCtx.get()));
}

TEST_F(StorageEngineTest, StartupRecoveryClearLocalTempCollections) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();
    // Create a local temp collection.
    auto ns = NamespaceString::createNamespaceString_forTest("local.coll1");
    auto swCollInfo = createTempCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());

    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);
    auto coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), ns);
    // The local temp collection is removed.
    ASSERT_FALSE(coll);
}

TEST_F(StorageEngineTest, TemporaryRecordStoreDoesNotTrackSizeAdjustments) {
    auto opCtx = cc().makeOperationContext();

    const auto insertRecordAndAssertSize = [&](RecordStore* rs, const RecordId& rid) {
        // Verify a temporary record store does not track size adjustments.
        const auto data = "data";

        WriteUnitOfWork wuow(opCtx.get());
        StatusWith<RecordId> s =
            rs->insertRecord(opCtx.get(), rid, data, strlen(data), Timestamp());
        ASSERT_TRUE(s.isOK());
        wuow.commit();

        ASSERT_EQ(rs->numRecords(), 0);
        ASSERT_EQ(rs->dataSize(), 0);
    };

    // Create the temporary record store and get its ident.
    const std::string ident = [&]() {
        std::unique_ptr<TemporaryRecordStore> rs;
        Lock::GlobalLock lk(&*opCtx, MODE_IS);
        rs = makeTemporary(opCtx.get());
        ASSERT(rs.get());

        insertRecordAndAssertSize(rs->rs(), RecordId(1));

        // Keep ident even when TemporaryRecordStore goes out of scope.
        rs->keep();
        return rs->rs()->getIdent();
    }();
    ASSERT(identExists(opCtx.get(), ident));

    std::unique_ptr<TemporaryRecordStore> rs;
    rs = _storageEngine->makeTemporaryRecordStoreFromExistingIdent(
        opCtx.get(), ident, KeyFormat::Long);

    // Verify a temporary record store does not track size adjustments after re-opening.
    insertRecordAndAssertSize(rs->rs(), RecordId(2));
}

class StorageEngineTimestampMonitorTest : public StorageEngineTest {
public:
    void setUp() override {
        StorageEngineTest::setUp();
        _storageEngine->startTimestampMonitor();
    }

    void waitForTimestampMonitorPass() {
        auto timestampMonitor =
            dynamic_cast<StorageEngineImpl*>(_storageEngine)->getTimestampMonitor();
        using TimestampType = StorageEngineImpl::TimestampMonitor::TimestampType;
        using TimestampListener = StorageEngineImpl::TimestampMonitor::TimestampListener;
        auto pf = makePromiseFuture<void>();
        auto listener = TimestampListener(
            TimestampType::kOldest,
            [promise = &pf.promise](OperationContext* opCtx, Timestamp t) mutable {
                promise->emplaceValue();
            });
        timestampMonitor->addListener(&listener);
        pf.future.wait();
        timestampMonitor->removeListener(&listener);
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

    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    auto swCollInfo = createCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());


    // Start a single-phase (i.e. no build UUID) index.
    const boost::optional<UUID> buildUUID = boost::none;
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, buildUUID));
        wuow.commit();
    }

    const auto indexIdent = _storageEngine->getCatalog()->getIndexIdent(
        opCtx.get(), swCollInfo.getValue().catalogId, indexName);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // Reconcile should have to dropped the ident to allow the index to be rebuilt.
    ASSERT(!identExists(opCtx.get(), indexIdent));

    // There are no two-phase builds to resume or restart.
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());
}

TEST_F(StorageEngineTest, ReconcileTwoPhaseIndexBuilds) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexA("a_1");
    const std::string indexB("b_1");

    auto swCollInfo = createCollection(opCtx.get(), ns);
    ASSERT_OK(swCollInfo.getStatus());

    Lock::GlobalLock lk(&*opCtx, MODE_IX);

    // Using a build UUID implies that this index build is two-phase. There is no special behavior
    // on primaries or secondaries.
    auto buildUUID = UUID::gen();

    // Start two indexes with the same buildUUID to simulate building multiple indexes within the
    // same build.
    {
        Lock::DBLock dbLk(opCtx.get(), ns.dbName(), MODE_IX);
        Lock::CollectionLock collLk(opCtx.get(), ns, MODE_X);
        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexA, buildUUID));
            wuow.commit();
        }
        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexB, buildUUID));
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

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());

    // Drop the ident from the storage engine but keep the underlying files.
    _storageEngine->getEngine()->dropIdentForImport(
        *opCtx.get(),
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        swCollInfo.getValue().ident);
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the ident has been recovered because the
    // KVEngine was started in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeCatalog(opCtx.get());
        _storageEngine->loadCatalog(
            opCtx.get(), boost::none, StorageEngine::LastShutdownState::kClean);
    }

    ASSERT(identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}
#endif

TEST_F(StorageEngineRepairTest, ReconcileSucceeds) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());

    ASSERT_OK(dropIdent(shard_role_details::getRecoveryUnit(opCtx.get()),
                        swCollInfo.getValue().ident,
                        /*identHasSizeInfo=*/true));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // Reconcile would normally return an error if a collection existed with a missing ident in the
    // storage engine. When in a repair context, that should not be the case.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToResume.size());

    ASSERT(!identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(0U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphansInCatalog) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());
    ASSERT(collectionExists(opCtx.get(), collNs));

    Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
    AutoGetDb db(opCtx.get(), collNs.dbName(), LockMode::MODE_X);
    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(removeEntry(opCtx.get(), collNs.ns_forTest(), _storageEngine->getCatalog()));
        wuow.commit();
    }

    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a repair context, loadCatalog() recreates catalog entries for orphaned idents.
    _storageEngine->loadCatalog(opCtx.get(), boost::none, StorageEngine::LastShutdownState::kClean);
    auto identNs = swCollInfo.getValue().ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs =
        NamespaceString::createNamespaceString_forTest("local.orphan." + identNs);

    ASSERT(identExists(opCtx.get(), swCollInfo.getValue().ident));
    ASSERT(collectionExists(opCtx.get(), orphanNs));

    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get());
    ASSERT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto swCollInfo = createCollection(opCtx.get(), collNs);
    ASSERT_OK(swCollInfo.getStatus());
    ASSERT(collectionExists(opCtx.get(), collNs));

    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    {
        AutoGetDb db(opCtx.get(), collNs.dbName(), LockMode::MODE_X);
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(removeEntry(opCtx.get(), collNs.ns_forTest(), _storageEngine->getCatalog()));
        wuow.commit();
    }
    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a normal startup context, loadCatalog() does not recreate catalog entries for
    // orphaned idents.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->loadCatalog(
            opCtx.get(), boost::none, StorageEngine::LastShutdownState::kClean);
    }
    // reconcileCatalogAndIdents() drops orphaned idents.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQUALS(0UL, reconcileResult.indexBuildsToRestart.size());

    ASSERT(!identExists(opCtx.get(), swCollInfo.getValue().ident));
    auto identNs = swCollInfo.getValue().ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs =
        NamespaceString::createNamespaceString_forTest("local.orphan." + identNs);
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
    Timestamp getCheckpointTimestamp() const override {
        checkpointTimestamp = std::make_unique<Timestamp>(checkpointTimestamp->getInc() + 1);
        return *checkpointTimestamp;
    }
    Timestamp getOldestTimestamp() const override {
        oldestTimestamp = std::make_unique<Timestamp>(oldestTimestamp->getInc() + 1);
        return *oldestTimestamp;
    }
    Timestamp getStableTimestamp() const override {
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
    void setUp() override {
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

    void tearDown() override {
        _storageEngine->cleanShutdown(getServiceContext());
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
    TimestampListener first(stable, [](OperationContext* opCtx, Timestamp timestamp) {});
    TimestampListener second(oldest, [](OperationContext* opCtx, Timestamp timestamp) {});
    TimestampListener third(stable, [](OperationContext* opCtx, Timestamp timestamp) {});

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

    TimestampListener first(checkpoint, [&](OperationContext* opCtx, Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!changes[0]) {
            changes[0] = true;
            cv.notify_all();
        }
    });

    TimestampListener second(oldest, [&](OperationContext* opCtx, Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!changes[1]) {
            changes[1] = true;
            cv.notify_all();
        }
    });

    TimestampListener third(stable, [&](OperationContext* opCtx, Timestamp timestamp) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!changes[2]) {
            changes[2] = true;
            cv.notify_all();
        }
    });

    TimestampListener fourth(stable, [&](OperationContext* opCtx, Timestamp timestamp) {
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
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] {
            for (auto const& change : changes) {
                if (!change) {
                    return false;
                }
            }
            return true;
        });
    };


    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->removeListener(&second);
    _storageEngine->getTimestampMonitor()->removeListener(&third);
    _storageEngine->getTimestampMonitor()->removeListener(&fourth);
}

TEST_F(TimestampKVEngineTest, TimestampAdvancesOnNotification) {
    Timestamp previous = Timestamp();
    AtomicWord<int> timesNotified{0};

    TimestampListener listener(stable, [&](OperationContext* opCtx, Timestamp timestamp) {
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

    _storageEngine->getTimestampMonitor()->removeListener(&listener);
}

TEST_F(StorageEngineTestNotEphemeral, UseAlternateStorageLocation) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString coll1Ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const NamespaceString coll2Ns = NamespaceString::createNamespaceString_forTest("db.coll2");
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
    getGlobalServiceContext()->getStorageEngine()->notifyStorageStartupRecoveryComplete();
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
    getGlobalServiceContext()->getStorageEngine()->notifyStorageStartupRecoveryComplete();
    ASSERT(StorageEngine::LastShutdownState::kClean == lastShutdownState);
    StorageEngineTest::_storageEngine = getServiceContext()->getStorageEngine();
    ASSERT_TRUE(collectionExists(opCtx.get(), coll1Ns));
    ASSERT_FALSE(collectionExists(opCtx.get(), coll2Ns));
}

}  // namespace
}  // namespace mongo
