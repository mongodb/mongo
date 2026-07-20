// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_engine.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/index_builds/index_builds.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_control.h"
#include "mongo/db/shard_role/shard_catalog/catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_direct_crud.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_test_fixture.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

void callbackMock() {}

TEST_F(StorageEngineTest, DirectWritesInsertTest) {
    auto opCtx = cc().makeOperationContext();
    auto ru = shard_role_details::getRecoveryUnit(opCtx.get());

    const int64_t intKey{1};
    const std::span<const char> strKey{"key"sv};
    const std::span<const char> value{"test"sv};

    auto intRs = makeTemporary(opCtx.get());           // KeyFormat::Long
    auto strRs = makeTemporaryClustered(opCtx.get());  // KeyFormat::String
    ASSERT(intRs.get());
    ASSERT(strRs.get());

    const auto intIdent = intRs->getIdent();
    const auto strIdent = strRs->getIdent();

    // Perform direct writes.
    {
        StorageWriteTransaction txn(*ru);
        ASSERT_OK(
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value));
        ASSERT_OK(
            storage_engine_direct_crud::insert(*_storageEngine, *ru, strIdent, strKey, value));
        txn.commit();
    }

    // Verify results.
    auto intOut = storage_engine_direct_crud::get(*_storageEngine, *ru, intIdent, intKey);
    auto strOut = storage_engine_direct_crud::get(*_storageEngine, *ru, strIdent, strKey);

    ASSERT_OK(intOut);
    ASSERT_OK(strOut);
    EXPECT_EQ(value.size(), intOut.getValue().capacity());
    EXPECT_EQ(value.size(), strOut.getValue().capacity());
    EXPECT_EQ(0, std::memcmp(value.data(), intOut.getValue().get(), value.size()));
    EXPECT_EQ(0, std::memcmp(value.data(), strOut.getValue().get(), value.size()));
}

TEST_F(StorageEngineTest, DirectWritesDeleteTest) {
    auto opCtx = cc().makeOperationContext();
    auto ru = shard_role_details::getRecoveryUnit(opCtx.get());

    const int64_t intKey{1};
    const std::span<const char> strKey{"key"sv};
    const std::span<const char> value{"test"sv};

    auto intRs = makeTemporary(opCtx.get());           // KeyFormat::Long
    auto strRs = makeTemporaryClustered(opCtx.get());  // KeyFormat::String
    ASSERT(intRs.get());
    ASSERT(strRs.get());

    const auto intIdent = intRs->getIdent();
    const auto strIdent = strRs->getIdent();

    // Initial insertions.
    {
        StorageWriteTransaction txn(*ru);
        ASSERT_OK(
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value));
        ASSERT_OK(
            storage_engine_direct_crud::insert(*_storageEngine, *ru, strIdent, strKey, value));
        txn.commit();
    }

    // Verify initial insertions.
    auto intOut = storage_engine_direct_crud::get(*_storageEngine, *ru, intIdent, intKey);
    auto strOut = storage_engine_direct_crud::get(*_storageEngine, *ru, strIdent, strKey);

    ASSERT_OK(intOut);
    ASSERT_OK(strOut);
    EXPECT_EQ(value.size(), intOut.getValue().capacity());
    EXPECT_EQ(value.size(), strOut.getValue().capacity());
    EXPECT_EQ(0, std::memcmp(value.data(), intOut.getValue().get(), value.size()));
    EXPECT_EQ(0, std::memcmp(value.data(), strOut.getValue().get(), value.size()));


    // Perform deletes.
    {
        StorageWriteTransaction txn(*ru);
        ASSERT_OK(storage_engine_direct_crud::remove(*_storageEngine, *ru, intIdent, intKey));
        ASSERT_OK(storage_engine_direct_crud::remove(*_storageEngine, *ru, strIdent, strKey));
        txn.commit();
    }

    // Check for successful deletes.
    auto s1 = storage_engine_direct_crud::get(*_storageEngine, *ru, intIdent, intKey);
    ASSERT_NOT_OK(s1);
    EXPECT_EQ(ErrorCodes::NoSuchKey, s1.getStatus().code());
    auto s2 = storage_engine_direct_crud::get(*_storageEngine, *ru, strIdent, strKey);
    ASSERT_NOT_OK(s2);
    EXPECT_EQ(ErrorCodes::NoSuchKey, s2.getStatus().code());
}


TEST_F(StorageEngineTest, DirectWritesFailures) {
    auto opCtx = cc().makeOperationContext();
    auto ru = shard_role_details::getRecoveryUnit(opCtx.get());

    const int64_t intKey{1};
    const int64_t nonExistentIntKey{2};
    const std::span<const char> strKey{"key"sv};
    const std::span<const char> nonExistentStrKey{"nonExistentKey"sv};
    const std::span<const char> value1{"test1"sv};
    const std::span<const char> value2{"test2"sv};

    auto intRs = makeTemporary(opCtx.get());           // KeyFormat::Long
    auto strRs = makeTemporaryClustered(opCtx.get());  // KeyFormat::String
    ASSERT(intRs.get());
    ASSERT(strRs.get());

    const auto intIdent = intRs->getIdent();
    const auto strIdent = strRs->getIdent();

    // Initial insertions.
    {
        StorageWriteTransaction txn(*ru);
        ASSERT_OK(
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value1));
        ASSERT_OK(
            storage_engine_direct_crud::insert(*_storageEngine, *ru, strIdent, strKey, value1));
        txn.commit();
    }


    // Duplicate key insertion will return DuplicateKey.
    {
        StorageWriteTransaction txn(*ru);
        auto s1 =
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value1);
        ASSERT_NOT_OK(s1);
        EXPECT_EQ(ErrorCodes::KeyExists, s1.code());
        auto s2 =
            storage_engine_direct_crud::insert(*_storageEngine, *ru, strIdent, strKey, value1);
        ASSERT_NOT_OK(s2);
        EXPECT_EQ(ErrorCodes::KeyExists, s2.code());
    }


    // Duplicate keys with different values will also return DuplicateKey.
    {
        StorageWriteTransaction txn(*ru);
        auto s1 =
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value2);
        ASSERT_NOT_OK(s1);
        EXPECT_EQ(ErrorCodes::KeyExists, s1.code());
        auto s2 =
            storage_engine_direct_crud::insert(*_storageEngine, *ru, strIdent, strKey, value2);
        ASSERT_NOT_OK(s2);
        EXPECT_EQ(ErrorCodes::KeyExists, s2.code());
    }

    // Deleting non-existent keys will return NoSuchKey.
    {
        StorageWriteTransaction txn(*ru);
        auto s1 =
            storage_engine_direct_crud::remove(*_storageEngine, *ru, intIdent, nonExistentIntKey);
        ASSERT_NOT_OK(s1);
        EXPECT_EQ(ErrorCodes::NoSuchKey, s1.code());

        auto s2 =
            storage_engine_direct_crud::remove(*_storageEngine, *ru, strIdent, nonExistentStrKey);
        ASSERT_NOT_OK(s2);
        EXPECT_EQ(ErrorCodes::NoSuchKey, s2.code());
    }
}

using StorageEngineTestDeathTest = StorageEngineTest;
DEATH_TEST_F(StorageEngineTestDeathTest,
             DirectWritesInsertRequiresStorageTransaction,
             "invariant") {
    auto opCtx = cc().makeOperationContext();
    auto ru = shard_role_details::getRecoveryUnit(opCtx.get());

    const char* key = "key";
    const char* valueToStore = "test";
    const int64_t intKey{1};
    const std::span<const char> value{valueToStore, std::strlen(valueToStore)};
    const std::span<const char> strKey{key, std::strlen(key)};

    auto intRs = makeTemporary(opCtx.get());           // KeyFormat::Long
    auto strRs = makeTemporaryClustered(opCtx.get());  // KeyFormat::String
    ASSERT(intRs.get());
    ASSERT(strRs.get());

    const auto intIdent = intRs->getIdent();

    // This should fail an invariant from missing a storage transaction.
    {
        auto status =
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value);
    }
}

DEATH_TEST_F(StorageEngineTestDeathTest,
             DirectWritesDeleteRequiresStorageTransaction,
             "invariant") {
    auto opCtx = cc().makeOperationContext();
    auto ru = shard_role_details::getRecoveryUnit(opCtx.get());

    const char* key = "key";
    const char* valueToStore = "test";
    const int64_t intKey{1};
    const std::span<const char> value{valueToStore, std::strlen(valueToStore)};
    const std::span<const char> strKey{key, std::strlen(key)};

    auto intRs = makeTemporary(opCtx.get());           // KeyFormat::Long
    auto strRs = makeTemporaryClustered(opCtx.get());  // KeyFormat::String
    ASSERT(intRs.get());
    ASSERT(strRs.get());

    const auto intIdent = intRs->getIdent();

    {
        StorageWriteTransaction txn(*ru);
        auto status =
            storage_engine_direct_crud::insert(*_storageEngine, *ru, intIdent, intKey, value);
        ASSERT_OK(status);
        txn.commit();
    }

    // This should fail an invariant from missing a storage transaction.
    {
        auto status = storage_engine_direct_crud::remove(*_storageEngine, *ru, intIdent, intKey);
    }
}

TEST_F(StorageEngineTest, ReconcileIdentsTest) {
    auto opCtx = cc().makeOperationContext();

    // Add a collection, `db.coll1` to both the DurableCatalog and KVEngine. The returned value is
    // the `ident` name given to the collection.
    auto collInfo =
        createCollection(opCtx.get(), NamespaceString::createNamespaceString_forTest("db.coll1"));

    // Create a table in the KVEngine not reflected in the DurableCatalog. This should be dropped
    // when reconciling.
    ASSERT_OK(
        createCollTable(opCtx.get(), NamespaceString::createNamespaceString_forTest("db.coll2")));

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToRestart.size());

    auto identsVec = getAllKVEngineIdents(opCtx.get());
    auto idents = std::set<std::string, std::less<>>(identsVec.begin(), identsVec.end());

    // There are two idents. `_mdb_catalog` and the ident for `db.coll1`.
    EXPECT_EQ(static_cast<const unsigned long>(2), idents.size());
    EXPECT_TRUE(idents.find(collInfo.ident) != idents.end());
    EXPECT_TRUE(idents.find(ident::kMdbCatalog) != idents.end());

    // Drop the `db.coll1` table, while leaving the MDBCatalog entry.
    ASSERT_OK(dropIdent(*shard_role_details::getRecoveryUnit(opCtx.get()),
                        collInfo.ident,
                        /*identHasSizeInfo=*/true));
    EXPECT_EQ(static_cast<const unsigned long>(1), getAllKVEngineIdents(opCtx.get()).size());

    // Reconciling this should result in an error.
    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_NOT_OK(reconcileStatus.getStatus());
    EXPECT_EQ(ErrorCodes::UnrecoverableRollbackError, reconcileStatus.getStatus());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphansAfterUncleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto collInfo = createCollection(opCtx.get(), collNs);

    ASSERT_OK(dropIdent(*shard_role_details::getRecoveryUnit(opCtx.get()),
                        collInfo.ident,
                        /*identHasSizeInfo=*/true));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the collection has been dropped because the
    // KVEngine was started after an unclean shutdown but not in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        catalog::closeCatalog(opCtx.get());
        _storageEngine->loadMDBCatalog(opCtx.get(), StorageEngine::LastShutdownState::kUnclean);
        catalog::initializeCollectionCatalog(
            opCtx.get(), _storageEngine, catalog::InitMode::kStartup, boost::none);
    }

    ASSERT(!identExists(opCtx.get(), collInfo.ident));
    ASSERT(!collectionExists(opCtx.get(), collNs));
}

TEST_F(StorageEngineTest, InternalRecordStoreClustered) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_IS);
    auto rs = makeTemporaryClustered(opCtx.get());
    ASSERT(rs.get());

    ASSERT(identExists(opCtx.get(), rs->getIdent()));

    // Insert record with RecordId of KeyFormat::String.
    const auto id = std::string_view{"1"};
    const auto rid = RecordId(id);
    const auto data = "data";
    WriteUnitOfWork wuow(opCtx.get());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(),
                                              *shard_role_details::getRecoveryUnit(opCtx.get()),
                                              rid,
                                              data,
                                              strlen(data),
                                              Timestamp());
    EXPECT_TRUE(s.isOK());
    wuow.commit();

    // Read the record back.
    RecordData rd;
    ASSERT_TRUE(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd));
    EXPECT_EQ(0, memcmp(data, rd.data(), strlen(data)));
}

TEST_F(StorageEngineTest, InternalRecordStoreReuseOrErrorExistingIdent) {
    const std::string ident = ident::generateNewInternalIdent();
    auto opCtx = cc().makeOperationContext();
    WriteUnitOfWork wuow(opCtx.get());
    auto tempRs = _storageEngine->makeInternalRecordStore(opCtx.get(), ident, KeyFormat::Long);
    ASSERT(tempRs);

    // makeInternalRecordStore colliding with an empty on disk ident is tolerated.
    auto& retryRu = *shard_role_details::getRecoveryUnit(opCtx.get());
    auto reused = _storageEngine->makeInternalRecordStore(opCtx.get(), ident, KeyFormat::Long);
    ASSERT(reused);
    auto cursor = reused->getCursor(opCtx.get(), retryRu);
    EXPECT_FALSE(cursor->next());
    wuow.commit();
}

class StorageEngineReconcileTest : public StorageEngineTest {
protected:
    UUID collectionUUID = UUID::gen();
    UUID buildUUID = UUID::gen();
    std::string resumableIndexFileName = "foo";

    // Makes an empty internal table.
    std::unique_ptr<RecordStore> makeInternalTable(OperationContext* opCtx) {
        std::unique_ptr<RecordStore> ret;
        {
            Lock::GlobalLock lk(opCtx, MODE_IS);
            ret = makeTemporary(opCtx);
        }
        EXPECT_TRUE(identExists(opCtx, ret->getIdent()));
        return ret;
    }

    // Creates a table in the KV engine with a specific ident name, not reflected in the catalog.
    void createTableWithIdent(OperationContext* opCtx, std::string_view ident) {
        auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        Lock::GlobalLock lk(opCtx, MODE_IS);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(_storageEngine->getEngine()->createRecordStore(
            provider, ru, NamespaceString{}, ident, RecordStore::Options{}));
        wuow.commit();
        EXPECT_TRUE(identExists(opCtx, ident));
    }

    std::unique_ptr<RecordStore> makeSpillTable(OperationContext* opCtx) {
        Lock::GlobalLock lk{opCtx, MODE_IS};
        auto spillEngine = opCtx->getServiceContext()->getStorageEngine()->getSpillEngine();
        auto spillTable = spillEngine->makeInternalRecordStore(
            *spillEngine->newRecoveryUnit(), ident::generateNewInternalIdent(), KeyFormat::Long);
        EXPECT_TRUE(spillIdentExists(opCtx, spillTable->getIdent()));
        return spillTable;
    }

    std::string prepareIndexBuild(OperationContext* opCtx, BSONObj& indexSpec) {
        // Creates a collection.
        auto ns = NamespaceString::createNamespaceString_forTest("db.coll1");
        auto collInfo = createCollection(opCtx, ns);
        auto catalogId = collInfo.catalogId;
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
        auto mdbCatalog = _storageEngine->getMDBCatalog();
        auto md = durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog)->metadata;
        auto offset = md->findIndexOffset(indexName);
        indexSpec = md->indexes[offset].spec;
        return mdbCatalog->getIndexIdent(opCtx, catalogId, indexName);
    }

    // Makes an internal table that contains index-resume metadata, where |pretendSideTable| and
    // |pretendSkippedRecordTable| are internal tables used for that resume.
    std::unique_ptr<RecordStore> makeIndexBuildResumeTable(
        OperationContext* opCtx,
        const RecordStore& pretendSideTable,
        const RecordStore& pretendSkippedRecordTable,
        const BSONObj& indexSpec = {}) {
        std::unique_ptr<RecordStore> ret;
        {
            Lock::GlobalLock lk(opCtx, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            ret = _storageEngine->makeInternalRecordStore(
                opCtx, ident::generateNewInternalIdent(kResumableIndexIdentStem), KeyFormat::Long);
            BSONObj resInfo =
                makePretendResumeInfo(pretendSideTable, pretendSkippedRecordTable, indexSpec);
            ASSERT_OK(ret->insertRecord(opCtx,
                                        *shard_role_details::getRecoveryUnit(opCtx),
                                        resInfo.objdata(),
                                        resInfo.objsize(),
                                        Timestamp()));
            wuow.commit();
        }
        EXPECT_TRUE(identExists(opCtx, ret->getIdent()));
        return ret;
    }

    // Returns index-resume metadata which would use the given |pretendSideTable| and
    // |pretendSkippedRecordTable| in the index's build.
    BSONObj makePretendResumeInfo(const RecordStore& pretendSideTable,
                                  const RecordStore& pretendSkippedRecordTable,
                                  const BSONObj& indexSpec) {
        IndexStateInfo indexInfo;
        indexInfo.setSpec(indexSpec);
        indexInfo.setIsMultikey({});
        indexInfo.setMultikeyPaths({});
        indexInfo.setSideWritesTable(pretendSideTable.getIdent());
        indexInfo.setSkippedRecordTrackerTable(pretendSkippedRecordTable.getIdent());
        indexInfo.setStorageIdentifier(resumableIndexFileName);
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

    std::unique_ptr<RecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> skippedRecordRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs, *skippedRecordRs);

    // Reconcile will drop all temporary idents when starting up after an unclean shutdown.
    auto reconcileResult = unittest::assertGet(reconcileAfterUncleanShutdown(opCtx.get()));

    EXPECT_EQ(0UL, reconcileResult.indexBuildsToRestart.size());
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToResume.size());
    EXPECT_FALSE(identExists(opCtx.get(), irrelevantRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), resumableIndexRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), necessaryRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), skippedRecordRs->getIdent()));
}

TEST_F(StorageEngineReconcileTest, ReconcileKeepsFastCountIdentsForUncleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    createTableWithIdent(opCtx.get(), ident::kFastCountMetadataStore);
    createTableWithIdent(opCtx.get(), ident::kFastCountMetadataStoreTimestamps);

    // Replicated fast count idents must survive reconciliation after an unclean shutdown.
    auto reconcileResult = unittest::assertGet(reconcileAfterUncleanShutdown(opCtx.get()));

    EXPECT_TRUE(identExists(opCtx.get(), ident::kFastCountMetadataStore));
    EXPECT_TRUE(identExists(opCtx.get(), ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(StorageEngineReconcileTest, ReconcileKeepsFastCountIdentsForCleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    createTableWithIdent(opCtx.get(), ident::kFastCountMetadataStore);
    createTableWithIdent(opCtx.get(), ident::kFastCountMetadataStoreTimestamps);

    // Replicated fast count idents must survive reconciliation after a clean shutdown.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    EXPECT_TRUE(identExists(opCtx.get(), ident::kFastCountMetadataStore));
    EXPECT_TRUE(identExists(opCtx.get(), ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryKeepsFastCountIdentsForUncleanShutdown) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    createTableWithIdent(opCtx.get(), ident::kFastCountMetadataStore);
    createTableWithIdent(opCtx.get(), ident::kFastCountMetadataStoreTimestamps);

    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kUnclean);

    // Replicated fast count idents must survive full startup recovery after an unclean shutdown.
    EXPECT_TRUE(identExists(opCtx.get(), ident::kFastCountMetadataStore));
    EXPECT_TRUE(identExists(opCtx.get(), ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(StorageEngineReconcileTest, ReconcileOnlyKeepsNecessaryIdentsForCleanShutdown) {
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<RecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> skippedRecordRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs, *skippedRecordRs);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // After clean shutdown, an internal ident should be kept if-and-only-if it is needed to resume
    // an index build.
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToRestart.size());
    EXPECT_EQ(1UL, reconcileResult.indexBuildsToResume.size());
    EXPECT_FALSE(identExists(opCtx.get(), irrelevantRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), resumableIndexRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), necessaryRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), skippedRecordRs->getIdent()));
}

void createTempFile(const boost::filesystem::path& path) {
    std::ofstream file(path.string());
    EXPECT_TRUE(boost::filesystem::exists(path));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryForUncleanShutdown) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<RecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> skippedRecordRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs, *skippedRecordRs);
    auto spillTable = makeSpillTable(opCtx.get());

    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kUnclean);

    // Reconcile will drop all temporary idents when starting up after an unclean shutdown.
    EXPECT_FALSE(identExists(opCtx.get(), irrelevantRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), resumableIndexRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), necessaryRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), skippedRecordRs->getIdent()));
    EXPECT_FALSE(spillIdentExists(opCtx.get(), spillTable->getIdent()));
}

// Abort the two-phase index build since it hangs in vote submission, because we are not running
// a full featured mongodb replica set.
void abortIndexBuild(OperationContext* opCtx, const UUID& buildUUID) {
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    // Pretend initial sync mode, otherwise abort is not allowed as a Secondary.
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx)->setFollowerMode(repl::MemberState::RS_STARTUP2));
    EXPECT_TRUE(IndexBuildsCoordinator::get(opCtx)->abortIndexBuildByBuildUUID(
        opCtx,
        buildUUID,
        IndexBuildAction::kInitialSyncAbort,
        Status{ErrorCodes::IndexBuildAborted, "Shutdown"}));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryResumableIndexForCleanShutdown) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<RecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> skippedRecordRs = makeInternalTable(opCtx.get());
    BSONObj indexSpec;
    auto indexIdent = prepareIndexBuild(opCtx.get(), indexSpec);
    std::unique_ptr<RecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs, *skippedRecordRs, indexSpec);

    // Test cleanup of temporary directory used by resumable index build
    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    auto irrelevantFile = tempDir / "garbage";
    // Create a file for resumable index build
    auto indexFile = tempDir / resumableIndexFileName;
    createTempFile(indexFile);
    createTempFile(irrelevantFile);

    ScopeGuard abortIndexOnExit([this, &opCtx] { abortIndexBuild(opCtx.get(), buildUUID); });
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);

    // tempDir is cleared except for files for resumable builds.
    EXPECT_TRUE(boost::filesystem::exists(tempDir));
    EXPECT_TRUE(boost::filesystem::exists(indexFile));
    EXPECT_FALSE(boost::filesystem::exists(irrelevantFile));

    // After clean shutdown, an internal ident should be kept if-and-only-if it is needed to resume
    // an index build.
    EXPECT_FALSE(identExists(opCtx.get(), irrelevantRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), resumableIndexRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), necessaryRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), skippedRecordRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), indexIdent));
}

TEST_F(StorageEngineReconcileTest, StartupRecoveryResumableIndexFallbackToRestart) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<RecordStore> irrelevantRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> necessaryRs = makeInternalTable(opCtx.get());
    std::unique_ptr<RecordStore> skippedRecordRs = makeInternalTable(opCtx.get());
    std::string indexIdent;
    {
        BSONObj unusedSpec;
        indexIdent = prepareIndexBuild(opCtx.get(), unusedSpec);
    }

    // Use an empty indexSpec which is invalid to resume.
    // The resumable index build will fail and fall back to restart.
    std::unique_ptr<RecordStore> resumableIndexRs =
        makeIndexBuildResumeTable(opCtx.get(), *necessaryRs, *skippedRecordRs);

    // Test cleanup of temporary directory used by resumable index build
    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    auto indexFile = tempDir / resumableIndexFileName;
    createTempFile(indexFile);

    ScopeGuard abortIndexOnExit([this, &opCtx] { abortIndexBuild(opCtx.get(), buildUUID); });
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);

    EXPECT_TRUE(boost::filesystem::exists(tempDir));
    // When resumable index build fails its temp file is removed.
    EXPECT_FALSE(boost::filesystem::exists(indexFile));

    EXPECT_FALSE(identExists(opCtx.get(), irrelevantRs->getIdent()));
    EXPECT_FALSE(identExists(opCtx.get(), resumableIndexRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), necessaryRs->getIdent()));
    EXPECT_TRUE(identExists(opCtx.get(), indexIdent));
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

    ScopeGuard abortIndexOnExit([this, &opCtx] { abortIndexBuild(opCtx.get(), buildUUID); });
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);

    ASSERT(identExists(opCtx.get(), indexIdent));
}

TEST_F(StorageEngineTest, StartupRecoveryBuildMissingIdIndex) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();
    auto ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    createCollection(opCtx.get(), ns);

    auto coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), ns);
    ASSERT(coll);
    // _id index is missing initially.
    EXPECT_FALSE(coll->getIndexCatalog()->findIdIndex(opCtx.get()));
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
    createTempCollection(opCtx.get(), ns);

    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kClean);
    auto coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), ns);
    // The local temp collection is removed.
    EXPECT_FALSE(coll);
}

TEST_F(StorageEngineTest, InternalRecordStoreDoesNotTrackSizeAdjustments) {
    auto opCtx = cc().makeOperationContext();

    const auto insertRecordAndAssertSize = [&](RecordStore* rs, const RecordId& rid) {
        // Verify an internal record store does not track size adjustments.
        const auto data = "data";

        WriteUnitOfWork wuow(opCtx.get());
        StatusWith<RecordId> s = rs->insertRecord(opCtx.get(),
                                                  *shard_role_details::getRecoveryUnit(opCtx.get()),
                                                  rid,
                                                  data,
                                                  strlen(data),
                                                  Timestamp());
        EXPECT_TRUE(s.isOK());
        wuow.commit();

        EXPECT_EQ(rs->numRecords(), 0);
        EXPECT_EQ(rs->dataSize(), 0);
    };

    // Create the internal record store and get its ident.
    std::unique_ptr<RecordStore> rs;
    const std::string ident = [&]() {
        Lock::GlobalLock lk(&*opCtx, MODE_IS);
        rs = makeTemporary(opCtx.get());
        ASSERT(rs.get());

        insertRecordAndAssertSize(rs.get(), RecordId(1));

        return std::string{rs->getIdent()};
    }();
    ASSERT(identExists(opCtx.get(), ident));

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    auto reopened = _storageEngine->getEngine()->getInternalRecordStore(ru, ident, KeyFormat::Long);

    // Verify an internal record store does not track size adjustments after re-opening.
    insertRecordAndAssertSize(reopened.get(), RecordId(2));
}

class StorageEngineTimestampMonitorTest : public StorageEngineTest {
public:
    void setUp() override {
        StorageEngineTest::setUp();
        _storageEngine->startTimestampMonitor(
            {&catalog_helper::kCollectionCatalogCleanupTimestampListener});
    }

    void waitForTimestampMonitorPass() {
        auto timestampMonitor = _storageEngine->getTimestampMonitor();
        using TimestampListener = StorageEngine::TimestampMonitor::TimestampListener;
        auto pf = makePromiseFuture<void>();
        auto listener =
            TimestampListener([promise = &pf.promise](OperationContext* opCtx, auto) mutable {
                promise->emplaceValue();
            });
        timestampMonitor->addListener(&listener);
        pf.future.wait();
        timestampMonitor->removeListener(&listener);
    }
};

TEST_F(StorageEngineTimestampMonitorTest, InternalRecordStoreDroppedByCallerEventually) {
    auto opCtx = cc().makeOperationContext();

    std::string ident;
    std::unique_ptr<RecordStore> tempRs;
    {
        WriteUnitOfWork wuow(opCtx.get());
        tempRs = _storageEngine->makeInternalRecordStore(
            opCtx.get(), _storageEngine->generateNewInternalIdent(), KeyFormat::Long);
        ASSERT(tempRs.get());
        ident = std::string{tempRs->getIdent()};

        ASSERT(identExists(opCtx.get(), ident));
        wuow.commit();
    }

    // Caller drops the record store manually.
    _storageEngine->addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(ident));
    tempRs.reset();

    waitForTimestampMonitorPass();
    ASSERT(!identExists(opCtx.get(), ident));
}

TEST_F(StorageEngineTimestampMonitorTest, InternalRecordStoreNotDroppedWithoutExplicitDrop) {
    auto opCtx = cc().makeOperationContext();

    std::string ident;
    {
        WriteUnitOfWork wuow(opCtx.get());
        auto tempRs = _storageEngine->makeInternalRecordStore(
            opCtx.get(), _storageEngine->generateNewInternalIdent(), KeyFormat::Long);
        ASSERT(tempRs.get());
        ident = std::string{tempRs->getIdent()};

        ASSERT(identExists(opCtx.get(), ident));
        wuow.commit();
    }

    // The ident for the record store should still exist even after a pass of the timestamp monitor,
    // since the caller did not explicitly schedule a drop.
    waitForTimestampMonitorPass();
    ASSERT(identExists(opCtx.get(), ident));
}

TEST_F(StorageEngineTest, ReconcileUnfinishedIndex) {
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_X);

    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    auto collInfo = createCollection(opCtx.get(), ns);


    // Start a single-phase (i.e. no build UUID) index.
    const boost::optional<UUID> buildUUID = boost::none;
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, buildUUID));
        wuow.commit();
    }

    const auto indexIdent =
        _storageEngine->getMDBCatalog()->getIndexIdent(opCtx.get(), collInfo.catalogId, indexName);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // Reconcile should have to dropped the ident to allow the index to be rebuilt.
    ASSERT(!identExists(opCtx.get(), indexIdent));

    // There are no two-phase builds to resume or restart.
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToRestart.size());
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToResume.size());
}

TEST_F(StorageEngineTest, ReconcileTwoPhaseIndexBuilds) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexA("a_1");
    const std::string indexB("b_1");

    auto collInfo = createCollection(opCtx.get(), ns);

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

    const auto indexIdentA =
        _storageEngine->getMDBCatalog()->getIndexIdent(opCtx.get(), collInfo.catalogId, indexA);
    const auto indexIdentB =
        _storageEngine->getMDBCatalog()->getIndexIdent(opCtx.get(), collInfo.catalogId, indexB);

    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));

    // Reconcile should not have dropped the ident to allow the restarted index build to do so
    // transactionally with the start.
    ASSERT(identExists(opCtx.get(), indexIdentA));
    ASSERT(identExists(opCtx.get(), indexIdentB));

    // Only one index build should be indicated as needing to be restarted.
    ASSERT_EQ(1UL, reconcileResult.indexBuildsToRestart.size());
    auto& [toRestartBuildUUID, toRestart] = *reconcileResult.indexBuildsToRestart.begin();
    EXPECT_EQ(buildUUID, toRestartBuildUUID);

    // Both specs should be listed within the same build.
    auto& specsAndIdents = toRestart.indexSpecsAndIdents;
    ASSERT_EQ(2UL, specsAndIdents.size());
    EXPECT_EQ(indexA, std::get<BSONObj>(specsAndIdents[0])["name"].str());
    EXPECT_EQ(indexB, std::get<BSONObj>(specsAndIdents[1])["name"].str());
    EXPECT_EQ(indexIdentA, std::get<std::string>(specsAndIdents[0]));
    EXPECT_EQ(indexIdentB, std::get<std::string>(specsAndIdents[1]));

    // There should be no index builds to resume.
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToResume.size());
}

#ifndef _WIN32  // WiredTiger does not support orphan file recovery on Windows.
TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto collInfo = createCollection(opCtx.get(), collNs);

    // Drop the ident from the storage engine but keep the underlying files.
    _storageEngine->getEngine()->dropIdentForImport(
        *opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), collInfo.ident);
    ASSERT(collectionExists(opCtx.get(), collNs));

    // After the catalog is reloaded, we expect that the ident has been recovered because the
    // KVEngine was started in a repair context.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        catalog::closeCatalog(opCtx.get());
        _storageEngine->loadMDBCatalog(opCtx.get(), StorageEngine::LastShutdownState::kClean);
        catalog::initializeCollectionCatalog(
            opCtx.get(), _storageEngine, catalog::InitMode::kStartup, boost::none);
    }

    ASSERT(identExists(opCtx.get(), collInfo.ident));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get(), callbackMock);
    EXPECT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}
#endif

TEST_F(StorageEngineRepairTest, ReconcileSucceeds) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto collInfo = createCollection(opCtx.get(), collNs);

    ASSERT_OK(dropIdent(*shard_role_details::getRecoveryUnit(opCtx.get()),
                        collInfo.ident,
                        /*identHasSizeInfo=*/true));
    ASSERT(collectionExists(opCtx.get(), collNs));

    // Reconcile would normally return an error if a collection existed with a missing ident in the
    // storage engine. When in a repair context, that should not be the case.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToRestart.size());
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToResume.size());

    ASSERT(!identExists(opCtx.get(), collInfo.ident));
    ASSERT(collectionExists(opCtx.get(), collNs));
    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get(), callbackMock);
    EXPECT_EQ(0U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineRepairTest, LoadCatalogRecoversOrphansInCatalog) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto collInfo = createCollection(opCtx.get(), collNs);
    ASSERT(collectionExists(opCtx.get(), collNs));

    Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
    AutoGetDb db(opCtx.get(), collNs.dbName(), LockMode::MODE_X);
    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(removeEntry(opCtx.get(), collNs.ns_forTest(), _storageEngine->getMDBCatalog()));
        wuow.commit();
    }

    ASSERT(!collectionExists(opCtx.get(), collNs));

    _storageEngine->closeMDBCatalog(opCtx.get());

    // When in a repair context, loadMDBCatalog() recreates catalog entries for orphaned idents.
    _storageEngine->loadMDBCatalog(opCtx.get(), StorageEngine::LastShutdownState::kClean);
    catalog::initializeCollectionCatalog(
        opCtx.get(), _storageEngine, catalog::InitMode::kStartup, boost::none);
    auto identNs = collInfo.ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs =
        NamespaceString::createNamespaceString_forTest("local.orphan." + identNs);

    ASSERT(identExists(opCtx.get(), collInfo.ident));
    ASSERT(collectionExists(opCtx.get(), orphanNs));

    StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(opCtx.get(), callbackMock);
    EXPECT_EQ(1U, StorageRepairObserver::get(getGlobalServiceContext())->getModifications().size());
}

TEST_F(StorageEngineTest, LoadCatalogDropsOrphans) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString collNs = NamespaceString::createNamespaceString_forTest("db.coll1");
    auto collInfo = createCollection(opCtx.get(), collNs);
    ASSERT(collectionExists(opCtx.get(), collNs));

    // Only drop the catalog entry; storage engine still knows about this ident.
    // This simulates an unclean shutdown happening between dropping the catalog entry and
    // the actual drop in storage engine.
    {
        AutoGetDb db(opCtx.get(), collNs.dbName(), LockMode::MODE_X);
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(removeEntry(opCtx.get(), collNs.ns_forTest(), _storageEngine->getMDBCatalog()));
        wuow.commit();
    }
    ASSERT(!collectionExists(opCtx.get(), collNs));

    // When in a normal startup context, loadMDBCatalog() does not recreate catalog entries for
    // orphaned idents.
    {
        Lock::GlobalWrite writeLock(opCtx.get(), Date_t::max(), Lock::InterruptBehavior::kThrow);
        _storageEngine->closeMDBCatalog(opCtx.get());
        _storageEngine->loadMDBCatalog(opCtx.get(), StorageEngine::LastShutdownState::kClean);
        catalog::initializeCollectionCatalog(
            opCtx.get(), _storageEngine, catalog::InitMode::kStartup, boost::none);
    }
    // reconcileCatalogAndIdents() drops orphaned idents.
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    EXPECT_EQ(0UL, reconcileResult.indexBuildsToRestart.size());

    ASSERT(!identExists(opCtx.get(), collInfo.ident));
    auto identNs = collInfo.ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString orphanNs =
        NamespaceString::createNamespaceString_forTest("local.orphan." + identNs);
    ASSERT(!collectionExists(opCtx.get(), orphanNs));
}

TEST_F(StorageEngineTestNotEphemeral, UseAlternateStorageLocation) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString coll1Ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const NamespaceString coll2Ns = NamespaceString::createNamespaceString_forTest("db.coll2");
    createCollection(opCtx.get(), coll1Ns);
    ASSERT(collectionExists(opCtx.get(), coll1Ns));
    EXPECT_FALSE(collectionExists(opCtx.get(), coll2Ns));

    LOGV2(5781102, "Starting up storage engine in alternate location");
    const auto oldPath = storageGlobalParams.dbpath;
    const auto newPath = boost::filesystem::path(oldPath).append(".alternate").string();
    boost::filesystem::create_directory(newPath);
    StorageControl::stopStorageControls(
        opCtx->getServiceContext(),
        {ErrorCodes::InterruptedDueToStorageChange, "The storage engine is being reinitialized."},
        /*forRestart=*/false);
    CollectionCatalog::write(getServiceContext(), [this](CollectionCatalog& catalog) {
        catalog.onCloseCatalog();
        catalog.deregisterAllCollectionsAndViews(getServiceContext());
    });
    auto lastShutdownState = reinitializeStorageEngine(
        opCtx.get(), StorageEngineInitFlags{}, false, false, false, false, [&newPath] {
            storageGlobalParams.dbpath = newPath;
        });
    {
        Lock::GlobalWrite globalLk(opCtx.get());
        catalog::initializeCollectionCatalog(opCtx.get(),
                                             getServiceContext()->getStorageEngine(),
                                             catalog::InitMode::kStorageChange);
    }
    getGlobalServiceContext()->getStorageEngine()->notifyStorageStartupRecoveryComplete();
    LOGV2(5781103, "Started up storage engine in alternate location");
    ASSERT(StorageEngine::LastShutdownState::kClean == lastShutdownState);
    StorageEngineTest::_storageEngine = getServiceContext()->getStorageEngine();
    // Alternate storage location should have no collections.
    EXPECT_FALSE(collectionExists(opCtx.get(), coll1Ns));
    EXPECT_FALSE(collectionExists(opCtx.get(), coll2Ns));

    createCollection(opCtx.get(), coll2Ns);
    EXPECT_FALSE(collectionExists(opCtx.get(), coll1Ns));
    EXPECT_TRUE(collectionExists(opCtx.get(), coll2Ns));

    LOGV2(5781104, "Starting up storage engine in original location");
    StorageControl::stopStorageControls(
        opCtx->getServiceContext(),
        {ErrorCodes::InterruptedDueToStorageChange, "The storage engine is being reinitialized."},
        /*forRestart=*/false);
    CollectionCatalog::write(getServiceContext(), [this](CollectionCatalog& catalog) {
        catalog.onCloseCatalog();
        catalog.deregisterAllCollectionsAndViews(getServiceContext());
    });
    lastShutdownState = reinitializeStorageEngine(
        opCtx.get(), StorageEngineInitFlags{}, false, false, false, false, [&oldPath] {
            storageGlobalParams.dbpath = oldPath;
        });
    {
        Lock::GlobalWrite globalLk(opCtx.get());
        catalog::initializeCollectionCatalog(opCtx.get(),
                                             getServiceContext()->getStorageEngine(),
                                             catalog::InitMode::kStorageChange);
    }
    getGlobalServiceContext()->getStorageEngine()->notifyStorageStartupRecoveryComplete();
    ASSERT(StorageEngine::LastShutdownState::kClean == lastShutdownState);
    StorageEngineTest::_storageEngine = getServiceContext()->getStorageEngine();
    EXPECT_TRUE(collectionExists(opCtx.get(), coll1Ns));
    EXPECT_FALSE(collectionExists(opCtx.get(), coll2Ns));
}

TEST_F(StorageEngineTest, IdentMissingForNonReadyIndex) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();

    Lock::GlobalLock lk(&*opCtx, MODE_X);

    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    auto coll = createCollection(opCtx.get(), ns);

    auto buildUUID = UUID::gen();
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, buildUUID));
        wuow.commit();
    }
    // The index build will never finish due to commit quorum so we need to unconditionally abort it
    ScopeGuard abortIndexOnExit([&] { abortIndexBuild(opCtx.get(), buildUUID); });

    // Drop the index ident, but leave it in the catalog. This can happen if the process is killed
    // while we're restarting an index build (as we drop and recreate the ident).
    const auto indexIdent =
        _storageEngine->getMDBCatalog()->getIndexIdent(opCtx.get(), coll.catalogId, indexName);
    ASSERT_OK(dropIdent(
        *shard_role_details::getRecoveryUnit(opCtx.get()), indexIdent, /*identHasSizeInfo=*/true));
    EXPECT_FALSE(identExists(opCtx.get(), indexIdent));

    // Since the index build never completed, startup repair should treat a missing ident
    // identically to an incomplete index and restart it.
    startup_recovery::repairAndRecoverDatabases(opCtx.get(),
                                                StorageEngine::LastShutdownState::kUnclean);

    // The ident should have been recreated
    ASSERT(identExists(opCtx.get(), indexIdent));

    auto collection =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), ns);
    ASSERT(collection);
    auto indexEntry = collection->getIndexCatalog()->findIndexByName(
        opCtx.get(), indexName, IndexCatalog::InclusionPolicy::kUnfinished);
    ASSERT(indexEntry);
    // Even though the index was rebuilt it's not ready due to that it's waiting for commit quorum
    EXPECT_FALSE(indexEntry->isReady());

    // Creating the IndexAccessMethod initially failed due to the ident not existing, but needs to
    // have been created at some point later or anything which tries to use the recovered index
    // would be broken
    ASSERT(indexEntry->accessMethod());
}

TEST_F(StorageEngineTest, IdentMissingForReadyIndex) {
    repl::StorageInterface::set(getServiceContext(),
                                std::make_unique<repl::StorageInterfaceImpl>());
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    createCollection(opCtx.get(), ns);

    // Create a ready index, and then drop the ident without removing it from the catalog
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(createIndex(opCtx.get(), ns, indexName));
        wuow.commit();
    }
    ASSERT_OK(dropIndexTable(opCtx.get(), ns, indexName));

    // Reinitialize the catalog as otherwise the already-initialized collection will continue to use
    // the now dropped index table
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& catalog) {
        catalog.deregisterAllCollectionsAndViews(opCtx->getServiceContext());
    });
    catalog::initializeCollectionCatalog(
        opCtx.get(), getServiceContext()->getStorageEngine(), catalog::InitMode::kStartup);

    // Startup recovery currently does not handle this invalid state, but throws an appropriate
    // exception rather than segfaulting or otherwise crashing uncleanly
    ASSERT_THROWS_CODE(startup_recovery::repairAndRecoverDatabases(
                           opCtx.get(), StorageEngine::LastShutdownState::kUnclean),
                       DBException,
                       ErrorCodes::NoSuchKey);
}

// Plants a torn catalog record by rewriting the entry's idxIdent sub-document while md.indexes
// is left untouched
void rewriteIdxIdent(OperationContext* opCtx, const RecordId& catalogId, BSONObj idxIdent) {
    auto mdbCatalog = opCtx->getServiceContext()->getStorageEngine()->getMDBCatalog();
    auto entry = durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    WriteUnitOfWork wuow(opCtx);
    durable_catalog::putMetaData(opCtx, catalogId, *entry->metadata, mdbCatalog, idxIdent);
    wuow.commit();
}

TEST_F(StorageEngineTest, ReconcileFailsForReadyIndexAbsentFromIdxIdent) {
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexNameX("x_1");
    const std::string indexNameY("y_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    auto collInfo = createCollection(opCtx.get(), ns);
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(createIndex(opCtx.get(), ns, indexNameX));
        wuow.commit();
    }
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(createIndex(opCtx.get(), ns, indexNameY));
        wuow.commit();
    }

    const auto identY =
        _storageEngine->getMDBCatalog()->getIndexIdent(opCtx.get(), collInfo.catalogId, indexNameY);

    // Drop x_1 from idxIdent while both indexes stay ready in md.indexes
    rewriteIdxIdent(opCtx.get(), collInfo.catalogId, BSON(indexNameY << identY));

    // Reconciliation must fail with a diagnostic naming the collection and index
    auto status = reconcile(opCtx.get()).getStatus();
    ASSERT_EQ(ErrorCodes::DataCorruptionDetected, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), indexNameX);
    ASSERT_STRING_CONTAINS(status.reason(), "db.coll1");
}

TEST_F(StorageEngineTest, ReconcileRestartsTwoPhaseBuildWhenEngineIdentMissing) {
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    auto collInfo = createCollection(opCtx.get(), ns);
    const auto buildUUID = UUID::gen();
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, buildUUID));
        wuow.commit();
    }

    // Point the unfinished index at an ident the engine does not have to simulate when an unclean
    // shutdown follows the untimestamped ident drop of an index build restart
    rewriteIdxIdent(opCtx.get(), collInfo.catalogId, BSON(indexName << "index-doesnotexist"));

    // An unfinished two-phase build with a missing ident must properly reconcile, carrying the
    // catalog's ident onward rather than a regenerated one
    auto reconcileResult = unittest::assertGet(reconcile(opCtx.get()));
    ASSERT_EQ(1UL, reconcileResult.indexBuildsToRestart.size());
    ASSERT_EQ(buildUUID, reconcileResult.indexBuildsToRestart.begin()->first);
    const auto& specsAndIdents =
        reconcileResult.indexBuildsToRestart.begin()->second.indexSpecsAndIdents;
    ASSERT_EQ(1UL, specsAndIdents.size());
    ASSERT_EQ("index-doesnotexist", std::get<1>(specsAndIdents.front()));
}

TEST_F(StorageEngineTest, ReconcileFailsForUnfinishedTwoPhaseBuildWithEmptyIdxIdent) {
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    auto collInfo = createCollection(opCtx.get(), ns);
    const auto buildUUID = UUID::gen();
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, buildUUID));
        wuow.commit();
    }

    // Tear the idxIdent entry away entirely while the unfinished build stays in md.indexes. The
    // restart path cannot construct internal idents from an empty ident string, so reconcile must
    // refuse the record.
    rewriteIdxIdent(opCtx.get(), collInfo.catalogId, BSONObj());

    auto status = reconcile(opCtx.get()).getStatus();
    ASSERT_EQ(ErrorCodes::DataCorruptionDetected, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), indexName);
}

TEST_F(StorageEngineTest, GetAllIdentsThrowsForWrongTypeIdxIdent) {
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("x_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    auto collInfo = createCollection(opCtx.get(), ns);
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(createIndex(opCtx.get(), ns, indexName));
        wuow.commit();
    }

    rewriteIdxIdent(opCtx.get(), collInfo.catalogId, BSON(indexName << 123));

    // getAllIdents rejects the wrong-type value before reconcile's corruption check can see it
    ASSERT_THROWS_CODE(reconcile(opCtx.get()).getStatus().ignore(), DBException, 13111);
}

TEST_F(StorageEngineTest, ReconcileFailsForReadyIndexWithEmptyStringIdent) {
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("x_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    auto collInfo = createCollection(opCtx.get(), ns);
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(createIndex(opCtx.get(), ns, indexName));
        wuow.commit();
    }

    // An empty ident string passes a type-only check but is equally unusable
    rewriteIdxIdent(opCtx.get(), collInfo.catalogId, BSON(indexName << ""));

    auto status = reconcile(opCtx.get()).getStatus();
    ASSERT_EQ(ErrorCodes::DataCorruptionDetected, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), indexName);
}

TEST_F(StorageEngineTest, ReconcileFailsForUnfinishedSinglePhaseIndexWithEmptyIdxIdent) {
    auto opCtx = cc().makeOperationContext();
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db.coll1");
    const std::string indexName("a_1");

    Lock::GlobalLock lk(opCtx.get(), MODE_X);

    auto collInfo = createCollection(opCtx.get(), ns);
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(startIndexBuild(opCtx.get(), ns, indexName, boost::none));
        wuow.commit();
    }

    rewriteIdxIdent(opCtx.get(), collInfo.catalogId, BSONObj());

    // The drop path this index would otherwise take cannot drop an empty ident (buildTableUri
    // invariants on it), so reconcile must refuse the torn record like every other shape.
    auto status = reconcile(opCtx.get()).getStatus();
    ASSERT_EQ(ErrorCodes::DataCorruptionDetected, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), indexName);
}

StorageEngine* reconfigureStorageEngine(OperationContext* opCtx, auto fn) {
    StorageControl::stopStorageControls(
        opCtx->getServiceContext(),
        {ErrorCodes::InterruptedDueToStorageChange, "The storage engine is being reinitialized."},
        /*forRestart=*/false);
    CollectionCatalog::write(opCtx->getServiceContext(), [&](CollectionCatalog& catalog) {
        catalog.deregisterAllCollectionsAndViews(opCtx->getServiceContext());
    });
    reinitializeStorageEngine(opCtx, StorageEngineInitFlags{}, false, false, false, false, [&] {
        boost::filesystem::remove_all(storageGlobalParams.dbpath);
        boost::filesystem::create_directory(storageGlobalParams.dbpath);
        fn();
    });
    return opCtx->getServiceContext()->getStorageEngine();
}

std::pair<std::string, std::string> createCollectionAndIndex(OperationContext* opCtx,
                                                             StorageEngineTest& fixture,
                                                             std::string_view ns) {

    Lock::GlobalLock lk(opCtx, MODE_X);
    auto collNs = NamespaceString::createNamespaceString_forTest(ns);
    auto coll = fixture.createCollection(opCtx, collNs);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_OK(fixture.createIndex(opCtx, collNs, "x"));
    wuow.commit();
    auto indexIdent =
        fixture._storageEngine->getMDBCatalog()->getIndexIdent(opCtx, coll.catalogId, "x");
    return {coll.ident, indexIdent};
}

TEST_F(StorageEngineTest, DirectoryPerDb) {
    auto opCtx = cc().makeOperationContext();

    {
        auto [ident, _] = createCollectionAndIndex(opCtx.get(), *this, "dbname.coll");
        ASSERT_STRING_OMITS(ident, "dbname");
    }

    _storageEngine =
        reconfigureStorageEngine(opCtx.get(), [] { storageGlobalParams.directoryperdb = true; });

    {
        auto [ident, _] = createCollectionAndIndex(opCtx.get(), *this, "dbname.coll");
        ASSERT_STRING_CONTAINS(ident, "dbname/");
    }

    _storageEngine =
        reconfigureStorageEngine(opCtx.get(), [] { storageGlobalParams.directoryperdb = false; });
}

TEST_F(StorageEngineTest, SplitIndexes) {
    auto opCtx = cc().makeOperationContext();

    {
        auto [_, indexIdent] = createCollectionAndIndex(opCtx.get(), *this, "dbname.coll");
        ASSERT_STRING_OMITS(indexIdent, "dbname");
        ASSERT_STRING_CONTAINS(indexIdent, "index-");
    }

    _storageEngine = reconfigureStorageEngine(
        opCtx.get(), [] { wiredTigerGlobalOptions.directoryForIndexes = true; });

    {
        auto [_, indexIdent] = createCollectionAndIndex(opCtx.get(), *this, "dbname.coll");
        ASSERT_STRING_OMITS(indexIdent, "dbname");
        ASSERT_STRING_CONTAINS(indexIdent, "index/");
    }

    _storageEngine = reconfigureStorageEngine(
        opCtx.get(), [] { wiredTigerGlobalOptions.directoryForIndexes = false; });
}

TEST_F(StorageEngineTest, DirectoryPerDBAndSplitIndexes) {
    auto opCtx = cc().makeOperationContext();

    {
        auto [collIdent, indexIdent] = createCollectionAndIndex(opCtx.get(), *this, "dbname.coll");
        ASSERT_STRING_OMITS(collIdent, "dbname");
        ASSERT_STRING_CONTAINS(indexIdent, "index-");
    }

    _storageEngine = reconfigureStorageEngine(opCtx.get(), [] {
        storageGlobalParams.directoryperdb = true;
        wiredTigerGlobalOptions.directoryForIndexes = true;
    });

    {
        auto [collIdent, indexIdent] = createCollectionAndIndex(opCtx.get(), *this, "dbname.coll");
        ASSERT_STRING_CONTAINS(collIdent, "dbname/collection/");
        ASSERT_STRING_CONTAINS(indexIdent, "dbname/index/");
    }

    _storageEngine = reconfigureStorageEngine(opCtx.get(), [] {
        storageGlobalParams.directoryperdb = false;
        wiredTigerGlobalOptions.directoryForIndexes = false;
    });
}

TEST_F(StorageEngineTest, ReinitializeStorageEngineKillsOperations) {
    unittest::Barrier barrier(2);
    bool interrupted = false;
    unittest::JoinThread thread([&, svcCtx = getServiceContext()] {
        ThreadClient client(svcCtx->getService());
        auto opCtx = client->makeOperationContext();
        barrier.countDownAndWait();
        try {
            opCtx->sleepFor(Minutes(10));
        } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>&) {
            interrupted = true;
        }
        opCtx.reset();
        barrier.countDownAndWait();
    });
    barrier.countDownAndWait();

    auto opCtx = cc().makeOperationContext();
    _storageEngine = reconfigureStorageEngine(opCtx.get(), [] {});
    barrier.countDownAndWait();
    ASSERT_TRUE(interrupted);
}

TEST_F(StorageEngineTest, ReinitializeStorageEngineWaitsForOperationsPendingDestruction) {
    unittest::Barrier barrier(2);
    bool interrupted = false;
    unittest::JoinThread thread([&, svcCtx = getServiceContext()] {
        ThreadClient client(svcCtx->getService());
        auto opCtx = client->makeOperationContext();
        svcCtx->markOperationAsPendingDestruction(opCtx.get());
        barrier.countDownAndWait();
        try {
            opCtx->sleepFor(Minutes(10));
        } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>&) {
            interrupted = true;
        }
        // note: intentionally no barrier here. The required synchronization is provided by the wait
        // on the opctx being destroyed.
    });
    barrier.countDownAndWait();

    auto opCtx = cc().makeOperationContext();
    _storageEngine = reconfigureStorageEngine(opCtx.get(), [] {});
    ASSERT_TRUE(interrupted);
}

}  // namespace
}  // namespace mongo
