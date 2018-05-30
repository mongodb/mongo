/**
 *    Copyright 2015 (C) MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace repl {
namespace {

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
OplogEntry makeOplogEntry(OpTypeEnum opType, NamespaceString nss, OptionalCollectionUUID uuid) {
    return OplogEntry(OpTime(Timestamp(1, 1), 1),  // optime
                      1LL,                         // hash
                      opType,                      // opType
                      nss,                         // namespace
                      uuid,                        // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      BSON("_id" << 0),            // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      boost::none,                 // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none);  // post-image optime
}

/**
 * Testing-only SyncTail that returns user-provided "document" for getMissingDoc().
 */
class SyncTailWithLocalDocumentFetcher : public SyncTail, OplogApplier::Observer {
public:
    SyncTailWithLocalDocumentFetcher(const BSONObj& document);
    BSONObj getMissingDoc(OperationContext* opCtx, const OplogEntry& oplogEntry) override;

    // OplogApplier::Observer functions
    void onBatchBegin(const OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<OpTime>&, const OplogApplier::Operations&) final {}
    void onMissingDocumentsFetchedAndInserted(const std::vector<FetchInfo>& docs) final {
        numFetched += docs.size();
    }
    void onOperationConsumed(const BSONObj& op) final {}

    std::size_t numFetched = 0U;

private:
    BSONObj _document;
};

/**
 * Testing-only SyncTail that checks the operation context in fetchAndInsertMissingDocument().
 */
class SyncTailWithOperationContextChecker : public SyncTail {
public:
    SyncTailWithOperationContextChecker();
    bool fetchAndInsertMissingDocument(OperationContext* opCtx,
                                       const OplogEntry& oplogEntry) override;
    bool called = false;
};

SyncTailWithLocalDocumentFetcher::SyncTailWithLocalDocumentFetcher(const BSONObj& document)
    : SyncTail(this,     // observer
               nullptr,  // consistency markers
               nullptr,  // storage interface
               SyncTail::MultiSyncApplyFunc(),
               nullptr,  // writer pool
               SyncTailTest::makeInitialSyncOptions()),
      _document(document) {}

BSONObj SyncTailWithLocalDocumentFetcher::getMissingDoc(OperationContext*, const OplogEntry&) {
    return _document;
}

SyncTailWithOperationContextChecker::SyncTailWithOperationContextChecker()
    : SyncTail(nullptr,  // observer
               nullptr,  // consistency markers
               nullptr,  // storage interface
               SyncTail::MultiSyncApplyFunc(),
               nullptr,  // writer pool
               SyncTailTest::makeInitialSyncOptions()) {}

bool SyncTailWithOperationContextChecker::fetchAndInsertMissingDocument(OperationContext* opCtx,
                                                                        const OplogEntry&) {
    ASSERT_FALSE(opCtx->writesAreReplicated());
    ASSERT_FALSE(opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());
    ASSERT_TRUE(documentValidationDisabled(opCtx));
    called = true;
    return false;
}

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions createOplogCollectionOptions() {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    return options;
}

/**
 * Create test collection.
 * Returns collection.
 */
void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const CollectionOptions& options) {
    writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
        Lock::DBLock dblk(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        auto db = ctx.db();
        ASSERT_TRUE(db);
        mongo::WriteUnitOfWork wuow(opCtx);
        auto coll = db->createCollection(opCtx, nss.ns(), options);
        ASSERT_TRUE(coll);
        wuow.commit();
    });
}


/**
 * Create test collection with UUID.
 */
auto createCollectionWithUuid(OperationContext* opCtx, const NamespaceString& nss) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    createCollection(opCtx, nss, options);
    return options.uuid.get();
}

/**
 * Create test database.
 */
void createDatabase(OperationContext* opCtx, StringData dbName) {
    Lock::GlobalWrite globalLock(opCtx);
    bool justCreated;
    Database* db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName, &justCreated);
    ASSERT_TRUE(db);
    ASSERT_TRUE(justCreated);
}

/**
 * Returns true if collection exists.
 */
bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return AutoGetCollectionForRead(opCtx, nss).getCollection() != nullptr;
}

auto parseFromOplogEntryArray(const BSONObj& obj, int elem) {
    BSONElement tsArray;
    Status status =
        bsonExtractTypedField(obj, OpTime::kTimestampFieldName, BSONType::Array, &tsArray);
    ASSERT_OK(status);

    BSONElement termArray;
    status = bsonExtractTypedField(obj, OpTime::kTermFieldName, BSONType::Array, &termArray);
    ASSERT_OK(status);

    return OpTime(tsArray.Array()[elem].timestamp(), termArray.Array()[elem].Long());
};

TEST_F(SyncTailTest, SyncApplyNoNamespaceBadOp) {
    const BSONObj op = BSON("op"
                            << "x");
    ASSERT_THROWS(
        SyncTail::syncApply(_opCtx.get(), op, OplogApplication::Mode::kInitialSync).ignore(),
        ExceptionFor<ErrorCodes::BadValue>);
}

TEST_F(SyncTailTest, SyncApplyNoNamespaceNoOp) {
    ASSERT_OK(SyncTail::syncApply(_opCtx.get(),
                                  BSON("op"
                                       << "n"),
                                  OplogApplication::Mode::kInitialSync));
}

TEST_F(SyncTailTest, SyncApplyBadOp) {
    const BSONObj op = BSON("op"
                            << "x"
                            << "ns"
                            << "test.t");
    ASSERT_THROWS(
        SyncTail::syncApply(_opCtx.get(), op, OplogApplication::Mode::kInitialSync).ignore(),
        ExceptionFor<ErrorCodes::BadValue>);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentDatabaseMissing) {
    NamespaceString nss("test.t");
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(
        SyncTail::syncApply(_opCtx.get(), op.toBSON(), OplogApplication::Mode::kSecondary).ignore(),
        ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentDatabaseMissing) {
    NamespaceString otherNss("test.othername");
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), false);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, UUID::gen());
    ASSERT_THROWS(
        SyncTail::syncApply(_opCtx.get(), op.toBSON(), OplogApplication::Mode::kSecondary).ignore(),
        ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, UUID::gen());
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), false);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection and lock the database in MODE_X.
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(
        SyncTail::syncApply(_opCtx.get(), op.toBSON(), OplogApplication::Mode::kSecondary).ignore(),
        ExceptionFor<ErrorCodes::NamespaceNotFound>);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection and lock the database in MODE_X.
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), false);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionExists) {
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), true);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionExists) {
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), false);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, uuid);
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), true);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    CollectionOptions options;
    options.uuid = UUID::gen();
    createCollection(_opCtx.get(), nss, options);

    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    _testSyncApplyCrudOperation(ErrorCodes::OK, op.toBSON(), false);
}

TEST_F(SyncTailTest, SyncApplyCommand) {
    NamespaceString nss("test.t");
    auto op = BSON("op"
                   << "c"
                   << "ns"
                   << nss.getCommandNS().ns()
                   << "o"
                   << BSON("create" << nss.coll()));
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            Collection*,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isW());
        ASSERT_TRUE(opCtx->writesAreReplicated());
        ASSERT_FALSE(documentValidationDisabled(opCtx));
        ASSERT_EQUALS(nss, collNss);
        return Status::OK();
    };
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_opCtx.get()));
    ASSERT_OK(SyncTail::syncApply(_opCtx.get(), op, OplogApplication::Mode::kInitialSync));
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(SyncTailTest, SyncApplyCommandThrowsException) {
    const BSONObj op = BSON("op"
                            << "c"
                            << "ns"
                            << 12345
                            << "o"
                            << BSON("create"
                                    << "t"));
    // This test relies on the namespace type check in applyCommand_inlock().
    ASSERT_THROWS(
        SyncTail::syncApply(_opCtx.get(), op, OplogApplication::Mode::kInitialSync).ignore(),
        ExceptionFor<ErrorCodes::InvalidNamespace>);
}

DEATH_TEST_F(SyncTailTest, MultiApplyAbortsWhenNoOperationsAreGiven, "!ops.empty()") {
    auto writerPool = SyncTail::makeWriterPool();
    SyncTail syncTail(nullptr,
                      getConsistencyMarkers(),
                      getStorageInterface(),
                      noopApplyOperationFn,
                      writerPool.get());
    syncTail.multiApply(_opCtx.get(), {}).getStatus().ignore();
}

bool _testOplogEntryIsForCappedCollection(OperationContext* opCtx,
                                          ReplicationConsistencyMarkers* const consistencyMarkers,
                                          StorageInterface* const storageInterface,
                                          const NamespaceString& nss,
                                          const CollectionOptions& options) {
    auto writerPool = SyncTail::makeWriterPool();
    MultiApplier::Operations operationsApplied;
    auto applyOperationFn = [&operationsApplied](OperationContext* opCtx,
                                                 MultiApplier::OperationPtrs* operationsToApply,
                                                 SyncTail* st,
                                                 WorkerMultikeyPathInfo*) -> Status {
        for (auto&& opPtr : *operationsToApply) {
            operationsApplied.push_back(*opPtr);
        }
        return Status::OK();
    };
    createCollection(opCtx, nss, options);

    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("a" << 1));
    ASSERT_FALSE(op.isForCappedCollection);

    SyncTail syncTail(
        nullptr, consistencyMarkers, storageInterface, applyOperationFn, writerPool.get());
    auto lastOpTime = unittest::assertGet(syncTail.multiApply(opCtx, {op}));
    ASSERT_EQUALS(op.getOpTime(), lastOpTime);

    ASSERT_EQUALS(1U, operationsApplied.size());
    const auto& opApplied = operationsApplied.front();
    ASSERT_EQUALS(op, opApplied);
    // "isForCappedCollection" is not parsed from raw oplog entry document.
    return opApplied.isForCappedCollection;
}

TEST_F(
    SyncTailTest,
    MultiApplyDoesNotSetOplogEntryIsForCappedCollectionWhenProcessingNonCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_FALSE(_testOplogEntryIsForCappedCollection(
        _opCtx.get(), getConsistencyMarkers(), getStorageInterface(), nss, CollectionOptions()));
}

TEST_F(SyncTailTest,
       MultiApplySetsOplogEntryIsForCappedCollectionWhenProcessingCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                     getConsistencyMarkers(),
                                                     getStorageInterface(),
                                                     nss,
                                                     createOplogCollectionOptions()));
}

TEST_F(SyncTailTest, MultiApplyAssignsOperationsToWriterThreadsBasedOnNamespaceHash) {
    // This test relies on implementation details of how multiApply uses hashing to distribute ops
    // to threads. It is possible for this test to fail, even if the implementation of multiApply is
    // correct. If it fails, consider adjusting the namespace names (to adjust the hash values) or
    // the number of threads in the pool.
    NamespaceString nss1("test.t0");
    NamespaceString nss2("test.t1");
    auto writerPool = SyncTail::makeWriterPool(2);

    stdx::mutex mutex;
    std::vector<MultiApplier::Operations> operationsApplied;
    auto applyOperationFn =
        [&mutex, &operationsApplied](OperationContext* opCtx,
                                     MultiApplier::OperationPtrs* operationsForWriterThreadToApply,
                                     SyncTail* st,
                                     WorkerMultikeyPathInfo*) -> Status {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationsApplied.emplace_back();
        for (auto&& opPtr : *operationsForWriterThreadToApply) {
            operationsApplied.back().push_back(*opPtr);
        }
        return Status::OK();
    };

    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss1, BSON("x" << 1));
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss2, BSON("x" << 2));

    SyncTail syncTail(nullptr,
                      getConsistencyMarkers(),
                      getStorageInterface(),
                      applyOperationFn,
                      writerPool.get());
    auto lastOpTime = unittest::assertGet(syncTail.multiApply(_opCtx.get(), {op1, op2}));
    ASSERT_EQUALS(op2.getOpTime(), lastOpTime);

    // Each writer thread should be given exactly one operation to apply.
    std::vector<OpTime> seen;
    {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        ASSERT_EQUALS(operationsApplied.size(), 2U);
        for (auto&& operationsAppliedByThread : operationsApplied) {
            ASSERT_EQUALS(1U, operationsAppliedByThread.size());
            const auto& oplogEntry = operationsAppliedByThread.front();
            ASSERT_TRUE(std::find(seen.cbegin(), seen.cend(), oplogEntry.getOpTime()) ==
                        seen.cend());
            ASSERT_TRUE(oplogEntry == op1 || oplogEntry == op2);
            seen.push_back(oplogEntry.getOpTime());
        }
    }

    // Check ops in oplog.
    // Obtain the last 2 entries in the oplog using a reverse collection scan.
    stdx::lock_guard<stdx::mutex> lock(mutex);
    auto storage = getStorageInterface();
    auto operationsWrittenToOplog =
        unittest::assertGet(storage->findDocuments(_opCtx.get(),
                                                   NamespaceString::kRsOplogNamespace,
                                                   {},
                                                   StorageInterface::ScanDirection::kBackward,
                                                   {},
                                                   BoundInclusion::kIncludeStartKeyOnly,
                                                   2U));
    ASSERT_EQUALS(2U, operationsWrittenToOplog.size());

    auto lastEntry = unittest::assertGet(OplogEntry::parse(operationsWrittenToOplog[0]));
    auto secondToLastEntry = unittest::assertGet(OplogEntry::parse(operationsWrittenToOplog[1]));
    ASSERT_EQUALS(op1, secondToLastEntry);
    ASSERT_EQUALS(op2, lastEntry);
}

TEST_F(SyncTailTest, MultiSyncApplyUsesSyncApplyToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);

    MultiApplier::OperationPtrs ops = {&op};
    WorkerMultikeyPathInfo pathInfo;
    SyncTail syncTail(nullptr, nullptr, nullptr, {}, nullptr);
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
    // Collection should be created after SyncTail::syncApply() processes operation.
    ASSERT_TRUE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
}

void testWorkerMultikeyPaths(OperationContext* opCtx,
                             const OplogEntry& op,
                             unsigned long numPaths) {
    SyncTail syncTail(nullptr, nullptr, nullptr, {}, nullptr);
    WorkerMultikeyPathInfo pathInfo;
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_OK(multiSyncApply(opCtx, &ops, &syncTail, &pathInfo));
    ASSERT_EQ(pathInfo.size(), numPaths);
}

TEST_F(SyncTailTest, MultiSyncApplyAddsWorkerMultikeyPathInfoOnInsert) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());

    {
        auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("a" << 1);
        auto op =
            makeCreateIndexOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, "a_1", keyPattern);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto doc = BSON("_id" << 1 << "a" << BSON_ARRAY(4 << 5));
        auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(3), 0), 1LL}, nss, doc);
        testWorkerMultikeyPaths(_opCtx.get(), op, 1UL);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyAddsMultipleWorkerMultikeyPathInfo) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());

    {
        auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("a" << 1);
        auto op =
            makeCreateIndexOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, "a_1", keyPattern);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("b" << 1);
        auto op =
            makeCreateIndexOplogEntry({Timestamp(Seconds(3), 0), 1LL}, nss, "b_1", keyPattern);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto docA = BSON("_id" << 1 << "a" << BSON_ARRAY(4 << 5));
        auto opA = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, docA);
        auto docB = BSON("_id" << 2 << "b" << BSON_ARRAY(6 << 7));
        auto opB = makeInsertDocumentOplogEntry({Timestamp(Seconds(5), 0), 1LL}, nss, docB);
        SyncTail syncTail(nullptr, nullptr, nullptr, {}, nullptr);
        WorkerMultikeyPathInfo pathInfo;
        MultiApplier::OperationPtrs ops = {&opA, &opB};
        ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
        ASSERT_EQ(pathInfo.size(), 2UL);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyDoesNotAddWorkerMultikeyPathInfoOnCreateIndex) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());

    {
        auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto doc = BSON("_id" << 1 << "a" << BSON_ARRAY(4 << 5));
        auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("a" << 1);
        auto op =
            makeCreateIndexOplogEntry({Timestamp(Seconds(3), 0), 1LL}, nss, "a_1", keyPattern);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto doc = BSON("_id" << 2 << "a" << BSON_ARRAY(6 << 7));
        auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyFailsWhenCollectionCreationTriesToMakeUUID) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_SECONDARY));
    NamespaceString nss("foo." + _agent.getSuiteName() + "_" + _agent.getTestName());

    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    SyncTail syncTail(nullptr, nullptr, nullptr, {}, nullptr);
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  multiSyncApply(_opCtx.get(), &ops, &syncTail, nullptr));
}

TEST_F(SyncTailTest, MultiSyncApplyDisablesDocumentValidationWhileApplyingOperations) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            onInsertsCalled = true;
            ASSERT_FALSE(opCtx->writesAreReplicated());
            ASSERT_FALSE(opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());
            ASSERT_TRUE(documentValidationDisabled(opCtx));
            return Status::OK();
        };
    createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0));
    ASSERT_OK(runOpSteadyState(op));
    ASSERT(onInsertsCalled);
}

TEST_F(SyncTailTest, MultiSyncApplyPassesThroughSyncApplyErrorAfterFailingToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    // Delete operation without _id in 'o' field.
    auto op = makeDeleteDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, {});
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, runOpSteadyState(op));
}

TEST_F(SyncTailTest, MultiSyncApplyPassesThroughSyncApplyException) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            onInsertsCalled = true;
            uasserted(ErrorCodes::OperationFailed, "");
            MONGO_UNREACHABLE;
        };
    createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, runOpSteadyState(op));
    ASSERT(onInsertsCalled);
}

TEST_F(SyncTailTest, MultiSyncApplySortsOperationsStablyByNamespaceBeforeApplying) {
    NamespaceString nss1("test.t1");
    NamespaceString nss2("test.t2");
    NamespaceString nss3("test.t3");

    const Seconds s(1);
    unsigned int i = 1;
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss1, BSON("_id" << 1));
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss1, BSON("_id" << 2));
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss2, BSON("_id" << 3));
    auto op4 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss3, BSON("_id" << 4));

    std::vector<NamespaceString> nssInserted;
    std::vector<BSONObj> docsInserted;
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            onInsertsCalled = true;
            for (const auto& doc : docs) {
                nssInserted.push_back(nss);
                docsInserted.push_back(doc);
            }
        };

    createCollectionWithUuid(_opCtx.get(), nss1);
    createCollectionWithUuid(_opCtx.get(), nss2);
    createCollectionWithUuid(_opCtx.get(), nss3);

    ASSERT_OK(runOpsSteadyState({op4, op1, op3, op2}));

    ASSERT_EQUALS(4U, nssInserted.size());
    ASSERT_EQUALS(nss1, nssInserted[0]);
    ASSERT_EQUALS(nss1, nssInserted[1]);
    ASSERT_EQUALS(nss2, nssInserted[2]);
    ASSERT_EQUALS(nss3, nssInserted[3]);

    ASSERT_EQUALS(4U, docsInserted.size());
    ASSERT_BSONOBJ_EQ(op1.getObject(), docsInserted[0]);
    ASSERT_BSONOBJ_EQ(op2.getObject(), docsInserted[1]);
    ASSERT_BSONOBJ_EQ(op3.getObject(), docsInserted[2]);
    ASSERT_BSONOBJ_EQ(op4.getObject(), docsInserted[3]);

    ASSERT(onInsertsCalled);
}

TEST_F(SyncTailTest, MultiSyncApplyGroupsInsertOperationByNamespaceBeforeApplying) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };
    NamespaceString nss1("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    NamespaceString nss2("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_2");
    auto createOp1 = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss1);
    auto createOp2 = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss2);
    auto insertOp1a = makeOp(nss1);
    auto insertOp1b = makeOp(nss1);
    auto insertOp2a = makeOp(nss2);
    auto insertOp2b = makeOp(nss2);

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    MultiApplier::Operations ops = {
        createOp1, createOp2, insertOp1a, insertOp2a, insertOp1b, insertOp2b};
    ASSERT_OK(runOpsSteadyState(ops));

    ASSERT_EQUALS(2U, docsInserted.size());

    // Check grouped insert operations in namespace "nss1".
    const auto& group1 = docsInserted[0];
    ASSERT_EQUALS(2U, group1.size());
    ASSERT_BSONOBJ_EQ(insertOp1a.getObject(), group1[0]);
    ASSERT_BSONOBJ_EQ(insertOp1b.getObject(), group1[1]);

    // Check grouped insert operations in namespace "nss2".
    const auto& group2 = docsInserted[1];
    ASSERT_EQUALS(2U, group2.size());
    ASSERT_BSONOBJ_EQ(insertOp2a.getObject(), group2[0]);
    ASSERT_BSONOBJ_EQ(insertOp2b.getObject(), group2[1]);
}

TEST_F(SyncTailTest, MultiSyncApplyLimitsBatchCountWhenGroupingInsertOperation) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Generate operations to apply:
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    std::size_t limit = 64;
    MultiApplier::Operations insertOps;
    for (std::size_t i = 0; i < limit + 1; ++i) {
        insertOps.push_back(makeOp(nss));
    }
    MultiApplier::Operations operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // multiSyncApply should combine operations as follows:
    // {create}, {grouped_insert}, {insert_(limit+1)}
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(2U, docsInserted.size());

    const auto& groupedInsertDocuments = docsInserted[0];
    ASSERT_EQUALS(limit, groupedInsertDocuments.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& insertOp = insertOps[i];
        ASSERT_BSONOBJ_EQ(insertOp.getObject(), groupedInsertDocuments[i]);
    }

    // (limit + 1)-th insert operations should not be included in group of first (limit) inserts.
    const auto& singleInsertDocumentGroup = docsInserted[1];
    ASSERT_EQUALS(1U, singleInsertDocumentGroup.size());
    ASSERT_BSONOBJ_EQ(insertOps.back().getObject(), singleInsertDocumentGroup[0]);
}

// Create an 'insert' oplog operation of an approximate size in bytes. The '_id' of the oplog entry
// and its optime in seconds are given by the 'id' argument.
OplogEntry makeSizedInsertOp(const NamespaceString& nss, int size, int id) {
    return makeInsertDocumentOplogEntry({Timestamp(Seconds(id), 0), 1LL},
                                        nss,
                                        BSON("_id" << id << "data" << std::string(size, '*')));
};

TEST_F(SyncTailTest, MultiSyncApplyLimitsBatchSizeWhenGroupingInsertOperations) {
    int seconds = 1;
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Create a sequence of insert ops that are too large to fit in one group.
    int maxBatchSize = insertVectorMaxBytes;
    int opsPerBatch = 3;
    int opSize = maxBatchSize / opsPerBatch - 500;  // Leave some room for other oplog fields.

    // Create the insert ops.
    MultiApplier::Operations insertOps;
    int numOps = 4;
    for (int i = 0; i < numOps; i++) {
        insertOps.push_back(makeSizedInsertOp(nss, opSize, seconds++));
    }

    MultiApplier::Operations operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    // Apply the ops.
    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // Applied ops should be as follows:
    // [ {create}, INSERT_GROUP{insert 1, insert 2, insert 3}, {insert 4} ]
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(2U, docsInserted.size());

    // Make sure the insert group was created correctly.
    const auto& groupedInsertOpArray = docsInserted[0];
    ASSERT_EQUALS(std::size_t(opsPerBatch), groupedInsertOpArray.size());
    for (int i = 0; i < opsPerBatch; ++i) {
        ASSERT_BSONOBJ_EQ(insertOps[i].getObject(), groupedInsertOpArray[i]);
    }

    // Check that the last op was applied individually.
    const auto& singleInsertDocumentGroup = docsInserted[1];
    ASSERT_EQUALS(1U, singleInsertDocumentGroup.size());
    ASSERT_BSONOBJ_EQ(insertOps[3].getObject(), singleInsertDocumentGroup[0]);
}

TEST_F(SyncTailTest, MultiSyncApplyAppliesOpIndividuallyWhenOpIndividuallyExceedsBatchSize) {
    int seconds = 1;
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    int maxBatchSize = insertVectorMaxBytes;
    // Create an insert op that exceeds the maximum batch size by itself.
    auto insertOpLarge = makeSizedInsertOp(nss, maxBatchSize, seconds++);
    auto insertOpSmall = makeSizedInsertOp(nss, 100, seconds++);

    MultiApplier::Operations operationsToApply = {createOp, insertOpLarge, insertOpSmall};

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    // Apply the ops.
    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // Applied ops should be as follows:
    // [ {create}, {large insert} {small insert} ]
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(2U, docsInserted.size());

    ASSERT_EQUALS(1U, docsInserted[0].size());
    ASSERT_BSONOBJ_EQ(insertOpLarge.getObject(), docsInserted[0][0]);

    ASSERT_EQUALS(1U, docsInserted[1].size());
    ASSERT_BSONOBJ_EQ(insertOpSmall.getObject(), docsInserted[1][0]);
}

TEST_F(SyncTailTest, MultiSyncApplyAppliesInsertOpsIndividuallyWhenUnableToCreateGroupByNamespace) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };

    auto testNs = "test." + _agent.getSuiteName() + "_" + _agent.getTestName();

    // Create a sequence of 3 'insert' ops that can't be grouped because they are from different
    // namespaces.
    MultiApplier::Operations operationsToApply = {makeOp(NamespaceString(testNs + "_1")),
                                                  makeOp(NamespaceString(testNs + "_2")),
                                                  makeOp(NamespaceString(testNs + "_3"))};

    for (const auto& oplogEntry : operationsToApply) {
        createCollectionWithUuid(_opCtx.get(), oplogEntry.getNamespace());
    }

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    // Apply the ops.
    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // Applied ops should be as follows i.e. no insert grouping:
    // [{insert 1}, {insert 2}, {insert 3}]
    ASSERT_EQ(operationsToApply.size(), docsInserted.size());
    for (std::size_t i = 0; i < operationsToApply.size(); i++) {
        const auto& group = docsInserted[i];
        ASSERT_EQUALS(1U, group.size()) << i;
        ASSERT_BSONOBJ_EQ(operationsToApply[i].getObject(), group[0]);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyFallsBackOnApplyingInsertsIndividuallyWhenGroupedInsertFails) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Generate operations to apply:
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    std::size_t limit = 64;
    MultiApplier::Operations insertOps;
    for (std::size_t i = 0; i < limit + 1; ++i) {
        insertOps.push_back(makeOp(nss));
    }
    MultiApplier::Operations operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    std::size_t numFailedGroupedInserts = 0;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            // Reject grouped insert operations.
            if (docs.size() > 1U) {
                numFailedGroupedInserts++;
                uasserted(ErrorCodes::OperationFailed, "grouped inserts not supported");
            }
            docsInserted.push_back(docs);
        };

    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // On failing to apply the grouped insert operation, multiSyncApply should apply the operations
    // as given in "operationsToApply":
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(limit + 1, docsInserted.size());

    for (std::size_t i = 0; i < limit + 1; ++i) {
        const auto& insertOp = insertOps[i];
        const auto& group = docsInserted[i];
        ASSERT_EQUALS(1U, group.size()) << i;
        ASSERT_BSONOBJ_EQ(insertOp.getObject(), group[0]);
    }

    // Ensure that multiSyncApply does not attempt to group remaining operations in first failed
    // grouped insert operation.
    ASSERT_EQUALS(1U, numFailedGroupedInserts);
}

TEST_F(SyncTailTest, MultiSyncApplyIgnoresUpdateOperationIfDocumentIsMissingFromSyncSource) {
    BSONObj emptyDoc;
    SyncTailWithLocalDocumentFetcher syncTail(emptyDoc);
    NamespaceString nss("test.t");
    {
        Lock::GlobalWrite globalLock(_opCtx.get());
        bool justCreated = false;
        Database* db =
            DatabaseHolder::getDatabaseHolder().openDb(_opCtx.get(), nss.db(), &justCreated);
        ASSERT_TRUE(db);
        ASSERT_TRUE(justCreated);
    }
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    MultiApplier::OperationPtrs ops = {&op};
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));

    // Since the missing document is not found on the sync source, the collection referenced by
    // the failed operation should not be automatically created.
    ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());

    // Fetch count should remain zero if we failed to copy the missing document.
    ASSERT_EQUALS(syncTail.numFetched, 0U);
}

TEST_F(SyncTailTest, MultiSyncApplySkipsDocumentOnNamespaceNotFoundDuringInitialSync) {
    BSONObj emptyDoc;
    SyncTailWithLocalDocumentFetcher syncTail(emptyDoc);
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    NamespaceString badNss("local." + _agent.getSuiteName() + "_" + _agent.getTestName() + "bad");
    auto doc1 = BSON("_id" << 1);
    auto doc2 = BSON("_id" << 2);
    auto doc3 = BSON("_id" << 3);
    auto op0 = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(Seconds(3), 0), 1LL}, badNss, doc2);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    MultiApplier::OperationPtrs ops = {&op0, &op1, &op2, &op3};
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
    ASSERT_EQUALS(syncTail.numFetched, 0U);

    OplogInterfaceLocal collectionReader(_opCtx.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_BSONOBJ_EQ(doc3, unittest::assertGet(iter->next()).first);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(SyncTailTest, MultiSyncApplySkipsIndexCreationOnNamespaceNotFoundDuringInitialSync) {
    BSONObj emptyDoc;
    SyncTailWithLocalDocumentFetcher syncTail(emptyDoc);
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    NamespaceString badNss("local." + _agent.getSuiteName() + "_" + _agent.getTestName() + "bad");
    auto doc1 = BSON("_id" << 1);
    auto keyPattern = BSON("a" << 1);
    auto doc3 = BSON("_id" << 3);
    auto op0 = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 =
        makeCreateIndexOplogEntry({Timestamp(Seconds(3), 0), 1LL}, badNss, "a_1", keyPattern);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    MultiApplier::OperationPtrs ops = {&op0, &op1, &op2, &op3};
    AtomicUInt32 fetchCount(0);
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
    ASSERT_EQUALS(syncTail.numFetched, 0U);

    OplogInterfaceLocal collectionReader(_opCtx.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_BSONOBJ_EQ(doc3, unittest::assertGet(iter->next()).first);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());

    // 'badNss' collection should not be implicitly created while attempting to create an index.
    ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx.get(), badNss).getCollection());
}

TEST_F(SyncTailTest, MultiSyncApplyFetchesMissingDocumentIfDocumentIsAvailableFromSyncSource) {
    SyncTailWithLocalDocumentFetcher syncTail(BSON("_id" << 0 << "x" << 1));
    NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto updatedDocument = BSON("_id" << 0 << "x" << 1);
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), updatedDocument);
    MultiApplier::OperationPtrs ops = {&op};
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
    ASSERT_EQUALS(syncTail.numFetched, 1U);

    // The collection referenced by "ns" in the failed operation is automatically created to hold
    // the missing document fetched from the sync source. We verify the contents of the collection
    // with the OplogInterfaceLocal class.
    OplogInterfaceLocal collectionReader(_opCtx.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_BSONOBJ_EQ(updatedDocument, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(IdempotencyTest, Geo2dsphereIndexFailedOnUpdate) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection()));
    auto insertOp = insert(fromjson("{_id: 1, loc: 'hi'}"));
    auto updateOp = update(1, fromjson("{$set: {loc: [1, 2]}}"));
    auto indexOp = buildIndex(fromjson("{loc: '2dsphere'}"), BSON("2dsphereIndexVersion" << 3));

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 16755);
}

TEST_F(IdempotencyTest, Geo2dsphereIndexFailedOnIndexing) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection()));
    auto indexOp = buildIndex(fromjson("{loc: '2dsphere'}"), BSON("2dsphereIndexVersion" << 3));
    auto dropIndexOp = dropIndex("loc_index");
    auto insertOp = insert(fromjson("{_id: 1, loc: 'hi'}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 16755);
}

TEST_F(IdempotencyTest, Geo2dIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection()));
    auto insertOp = insert(fromjson("{_id: 1, loc: [1]}"));
    auto updateOp = update(1, fromjson("{$set: {loc: [1, 2]}}"));
    auto indexOp = buildIndex(fromjson("{loc: '2d'}"));

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 13068);
}

TEST_F(IdempotencyTest, UniqueKeyIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection()));
    auto insertOp = insert(fromjson("{_id: 1, x: 5}"));
    auto updateOp = update(1, fromjson("{$set: {x: 6}}"));
    auto insertOp2 = insert(fromjson("{_id: 2, x: 5}"));
    auto indexOp = buildIndex(fromjson("{x: 1}"), fromjson("{unique: true}"));

    auto ops = {insertOp, updateOp, insertOp2, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);
}

TEST_F(IdempotencyTest, ParallelArrayError) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1}"))));

    auto updateOp1 = update(1, fromjson("{$set: {x: [1, 2]}}"));
    auto updateOp2 = update(1, fromjson("{$set: {x: 1}}"));
    auto updateOp3 = update(1, fromjson("{$set: {y: [3, 4]}}"));
    auto indexOp = buildIndex(fromjson("{x: 1, y: 1}"));

    auto ops = {updateOp1, updateOp2, updateOp3, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::CannotIndexParallelArrays);
}

TEST_F(IdempotencyTest, IndexKeyTooLongError) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1}"))));

    // Key size limit is 1024 for ephemeral storage engine, so two 800 byte fields cannot
    // co-exist.
    std::string longStr(800, 'a');
    auto updateOp1 = update(1, BSON("$set" << BSON("x" << longStr)));
    auto updateOp2 = update(1, fromjson("{$set: {x: 1}}"));
    auto updateOp3 = update(1, BSON("$set" << BSON("y" << longStr)));
    auto indexOp = buildIndex(fromjson("{x: 1, y: 1}"));

    auto ops = {updateOp1, updateOp2, updateOp3, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::KeyTooLong);
}

TEST_F(IdempotencyTest, IndexWithDifferentOptions) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1, x: 'hi'}"))));

    auto indexOp1 = buildIndex(fromjson("{x: 'text'}"), fromjson("{default_language: 'spanish'}"));
    auto dropIndexOp = dropIndex("x_index");
    auto indexOp2 = buildIndex(fromjson("{x: 'text'}"), fromjson("{default_language: 'english'}"));

    auto ops = {indexOp1, dropIndexOp, indexOp2};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::IndexOptionsConflict);
}

TEST_F(IdempotencyTest, TextIndexDocumentHasNonStringLanguageField) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 1}"));
    auto updateOp = update(1, fromjson("{$unset: {language: 1}}"));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj());

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, InsertDocumentWithNonStringLanguageFieldWhenTextIndexExists) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj());
    auto dropIndexOp = dropIndex("x_index");
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 1}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, TextIndexDocumentHasNonStringLanguageOverrideField) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', y: 1}"));
    auto updateOp = update(1, fromjson("{$unset: {y: 1}}"));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), fromjson("{language_override: 'y'}"));

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, InsertDocumentWithNonStringLanguageOverrideFieldWhenTextIndexExists) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), fromjson("{language_override: 'y'}"));
    auto dropIndexOp = dropIndex("x_index");
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', y: 1}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, TextIndexDocumentHasUnknownLanguage) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 'bad'}"));
    auto updateOp = update(1, fromjson("{$unset: {language: 1}}"));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj());

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17262);
}

TEST_F(IdempotencyTest, CreateCollectionWithValidation) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    const BSONObj uuidObj = UUID::gen().toBSON();

    auto runOpsAndValidate = [this, uuidObj]() {
        auto options1 = fromjson("{'validator' : {'phone' : {'$type' : 'string' } } }");
        options1 = options1.addField(uuidObj.firstElement());
        auto createColl1 = makeCreateCollectionOplogEntry(nextOpTime(), nss, options1);
        auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));

        auto options2 = fromjson("{'validator' : {'phone' : {'$type' : 'number' } } }");
        options2 = options2.addField(uuidObj.firstElement());
        auto createColl2 = makeCreateCollectionOplogEntry(nextOpTime(), nss, options2);

        auto ops = {createColl1, dropColl, createColl2};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate();

        return state;
    };

    auto state1 = runOpsAndValidate();
    auto state2 = runOpsAndValidate();
    ASSERT_EQUALS(state1, state2);
}

TEST_F(IdempotencyTest, CreateCollectionWithCollation) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection()));
    CollectionUUID uuid = UUID::gen();

    auto runOpsAndValidate = [this, uuid]() {
        auto insertOp1 = insert(fromjson("{ _id: 'foo' }"));
        auto insertOp2 = insert(fromjson("{ _id: 'Foo', x: 1 }"));
        auto updateOp = update("foo", BSON("$set" << BSON("x" << 2)));
        auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));
        auto options = BSON("collation" << BSON("locale"
                                                << "en"
                                                << "caseLevel"
                                                << false
                                                << "caseFirst"
                                                << "off"
                                                << "strength"
                                                << 1
                                                << "numericOrdering"
                                                << false
                                                << "alternate"
                                                << "non-ignorable"
                                                << "maxVariable"
                                                << "punct"
                                                << "normalization"
                                                << false
                                                << "backwards"
                                                << false
                                                << "version"
                                                << "57.1")
                                        << "uuid"
                                        << uuid);
        auto createColl = makeCreateCollectionOplogEntry(nextOpTime(), nss, options);

        auto ops = {insertOp1, insertOp2, updateOp, dropColl, createColl};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate();

        return state;
    };

    auto state1 = runOpsAndValidate();
    auto state2 = runOpsAndValidate();
    ASSERT_EQUALS(state1, state2);
}

TEST_F(IdempotencyTest, CreateCollectionWithIdIndex) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    CollectionUUID uuid = UUID::gen();

    auto options1 = BSON("idIndex" << BSON("key" << fromjson("{_id: 1}") << "name"
                                                 << "_id_"
                                                 << "v"
                                                 << 2
                                                 << "ns"
                                                 << nss.ns())
                                   << "uuid"
                                   << uuid);
    auto createColl1 = makeCreateCollectionOplogEntry(nextOpTime(), nss, options1);
    ASSERT_OK(runOpInitialSync(createColl1));

    auto runOpsAndValidate = [this, uuid]() {
        auto insertOp = insert(BSON("_id" << Decimal128(1)));
        auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));
        auto createColl2 = createCollection(uuid);

        auto ops = {insertOp, dropColl, createColl2};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate();

        return state;
    };

    auto state1 = runOpsAndValidate();
    auto state2 = runOpsAndValidate();
    ASSERT_EQUALS(state1, state2);
}

TEST_F(IdempotencyTest, CreateCollectionWithView) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    CollectionOptions options;
    options.uuid = UUID::gen();

    // Create data collection
    ASSERT_OK(runOpInitialSync(createCollection()));
    // Create "system.views" collection
    auto viewNss = NamespaceString(nss.db(), "system.views");
    ASSERT_OK(
        runOpInitialSync(makeCreateCollectionOplogEntry(nextOpTime(), viewNss, options.toBSON())));

    auto viewDoc =
        BSON("_id" << NamespaceString(nss.db(), "view").ns() << "viewOn" << nss.coll() << "pipeline"
                   << fromjson("[ { '$project' : { 'x' : 1 } } ]"));
    auto insertViewOp = makeInsertDocumentOplogEntry(nextOpTime(), viewNss, viewDoc);
    auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));

    auto ops = {insertViewOp, dropColl};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, CollModNamespaceNotFound) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    ASSERT_OK(
        runOpInitialSync(buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600))));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto collModOp = makeCommandOplogEntry(nextOpTime(), nss, collModCmd);
    auto dropCollOp = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));

    auto ops = {collModOp, dropCollOp};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, CollModIndexNotFound) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection()));
    ASSERT_OK(
        runOpInitialSync(buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600))));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto collModOp = makeCommandOplogEntry(nextOpTime(), nss, collModCmd);
    auto dropIndexOp = dropIndex("createdAt_index");

    auto ops = {collModOp, dropIndexOp};
    testOpsAreIdempotent(ops);
}

TEST_F(SyncTailTest, FailOnDropFCVCollection) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    auto cmd = BSON("drop" << fcvNS.coll());
    auto op = makeCommandOplogEntry(nextOpTime(), fcvNS, cmd);
    ASSERT_EQUALS(runOpInitialSync(op), ErrorCodes::OplogOperationUnsupported);
}

TEST_F(SyncTailTest, FailOnInsertFCVDocument) {
    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    ::mongo::repl::createCollection(_opCtx.get(), fcvNS, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeInsertDocumentOplogEntry(
        nextOpTime(), fcvNS, BSON("_id" << FeatureCompatibilityVersionParser::kParameterName));
    ASSERT_EQUALS(runOpInitialSync(op), ErrorCodes::OplogOperationUnsupported);
}

TEST_F(IdempotencyTest, InsertToFCVCollectionBesidesFCVDocumentSucceeds) {
    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    ::mongo::repl::createCollection(_opCtx.get(), fcvNS, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeInsertDocumentOplogEntry(nextOpTime(),
                                           fcvNS,
                                           BSON("_id"
                                                << "other"));
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(IdempotencyTest, DropDatabaseSucceeds) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(SyncTailTest, DropDatabaseSucceedsInRecovering) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpSteadyState(op));
}

class SyncTailTxnTableTest : public SyncTailTest {
public:
    void setUp() override {
        SyncTailTest::setUp();

        SessionCatalog::get(_opCtx->getServiceContext())->onStepUp(_opCtx.get());

        DBDirectClient client(_opCtx.get());
        BSONObj result;
        ASSERT(client.runCommand(kNs.db().toString(), BSON("create" << kNs.coll()), result));
    }
    void tearDown() override {
        SessionCatalog::get(_opCtx->getServiceContext())->reset_forTest();
        SyncTailTest::tearDown();
    }

    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    repl::OplogEntry makeOplogEntry(const NamespaceString& ns,
                                    repl::OpTime opTime,
                                    repl::OpTypeEnum opType,
                                    BSONObj object,
                                    boost::optional<BSONObj> object2,
                                    const OperationSessionInfo& sessionInfo,
                                    Date_t wallClockTime) {
        return repl::OplogEntry(opTime,         // optime
                                0,              // hash
                                opType,         // opType
                                ns,             // namespace
                                boost::none,    // uuid
                                boost::none,    // fromMigrate
                                0,              // version
                                object,         // o
                                object2,        // o2
                                sessionInfo,    // sessionInfo
                                boost::none,    // false
                                wallClockTime,  // wall clock time
                                boost::none,    // statement id
                                boost::none,    // optime of previous write within same transaction
                                boost::none,    // pre-image optime
                                boost::none);   // post-image optime
    }

    void checkTxnTable(const OperationSessionInfo& sessionInfo,
                       const repl::OpTime& expectedOpTime,
                       Date_t expectedWallClock) {
        invariant(sessionInfo.getSessionId());
        invariant(sessionInfo.getTxnNumber());

        DBDirectClient client(_opCtx.get());
        auto result = client.findOne(
            NamespaceString::kSessionTransactionsTableNamespace.ns(),
            {BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())});
        ASSERT_FALSE(result.isEmpty());

        auto txnRecord =
            SessionTxnRecord::parse(IDLParserErrorContext("parse txn record for test"), result);

        ASSERT_EQ(*sessionInfo.getTxnNumber(), txnRecord.getTxnNum());
        ASSERT_EQ(expectedOpTime, txnRecord.getLastWriteOpTime());
        ASSERT_EQ(expectedWallClock, txnRecord.getLastWriteDate());
    }

    static const NamespaceString& nss() {
        return kNs;
    }

private:
    static const NamespaceString kNs;
};

const NamespaceString SyncTailTxnTableTest::kNs("test.foo");

TEST_F(SyncTailTxnTableTest, SimpleWriteWithTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    const auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    auto writerPool = SyncTail::makeWriterPool();
    SyncTail syncTail(
        nullptr, getConsistencyMarkers(), getStorageInterface(), multiSyncApply, writerPool.get());
    ASSERT_OK(syncTail.multiApply(_opCtx.get(), {insertOp}));

    checkTxnTable(sessionInfo, {Timestamp(1, 0), 1}, date);
}

TEST_F(SyncTailTxnTableTest, WriteWithTxnMixedWithDirectWriteToTxnTable) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    const auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    auto deleteOp = makeOplogEntry(NamespaceString::kSessionTransactionsTableNamespace,
                                   {Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kDelete,
                                   BSON("_id" << sessionInfo.getSessionId()->toBSON()),
                                   boost::none,
                                   {},
                                   Date_t::now());

    auto writerPool = SyncTail::makeWriterPool();
    SyncTail syncTail(
        nullptr, getConsistencyMarkers(), getStorageInterface(), multiSyncApply, writerPool.get());
    ASSERT_OK(syncTail.multiApply(_opCtx.get(), {insertOp, deleteOp}));

    DBDirectClient client(_opCtx.get());
    auto result = client.findOne(
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        {BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())});
    ASSERT_TRUE(result.isEmpty());
}

TEST_F(SyncTailTxnTableTest, InterleavedWriteWithTxnMixedWithDirectDeleteToTxnTable) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    auto deleteOp = makeOplogEntry(NamespaceString::kSessionTransactionsTableNamespace,
                                   {Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kDelete,
                                   BSON("_id" << sessionInfo.getSessionId()->toBSON()),
                                   boost::none,
                                   {},
                                   Date_t::now());

    date = Date_t::now();
    sessionInfo.setTxnNumber(7);
    auto insertOp2 = makeOplogEntry(nss(),
                                    {Timestamp(3, 0), 2},
                                    repl::OpTypeEnum::kInsert,
                                    BSON("_id" << 6),
                                    boost::none,
                                    sessionInfo,
                                    date);

    auto writerPool = SyncTail::makeWriterPool();
    SyncTail syncTail(
        nullptr, getConsistencyMarkers(), getStorageInterface(), multiSyncApply, writerPool.get());
    ASSERT_OK(syncTail.multiApply(_opCtx.get(), {insertOp, deleteOp, insertOp2}));

    checkTxnTable(sessionInfo, {Timestamp(3, 0), 2}, date);
}

TEST_F(SyncTailTxnTableTest, InterleavedWriteWithTxnMixedWithDirectUpdateToTxnTable) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    repl::OpTime newWriteOpTime(Timestamp(2, 0), 1);
    auto updateOp = makeOplogEntry(NamespaceString::kSessionTransactionsTableNamespace,
                                   {Timestamp(4, 0), 1},
                                   repl::OpTypeEnum::kUpdate,
                                   BSON("$set" << BSON("lastWriteOpTime" << newWriteOpTime)),
                                   BSON("_id" << sessionInfo.getSessionId()->toBSON()),
                                   {},
                                   Date_t::now());

    auto writerPool = SyncTail::makeWriterPool();
    SyncTail syncTail(
        nullptr, getConsistencyMarkers(), getStorageInterface(), multiSyncApply, writerPool.get());
    ASSERT_OK(syncTail.multiApply(_opCtx.get(), {insertOp, updateOp}));

    checkTxnTable(sessionInfo, newWriteOpTime, date);
}

TEST_F(SyncTailTxnTableTest, MultiApplyUpdatesTheTransactionTable) {
    NamespaceString ns0("test.0");
    NamespaceString ns1("test.1");
    NamespaceString ns2("test.2");
    NamespaceString ns3("test.3");

    DBDirectClient client(_opCtx.get());
    BSONObj result;
    ASSERT(client.runCommand(ns0.db().toString(), BSON("create" << ns0.coll()), result));
    ASSERT(client.runCommand(ns1.db().toString(), BSON("create" << ns1.coll()), result));
    ASSERT(client.runCommand(ns2.db().toString(), BSON("create" << ns2.coll()), result));
    ASSERT(client.runCommand(ns3.db().toString(), BSON("create" << ns3.coll()), result));

    // Entries with a session id and a txnNumber update the transaction table.
    auto lsidSingle = makeLogicalSessionIdForTest();
    auto opSingle = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(1), 0), 1LL}, ns0, BSON("_id" << 0), lsidSingle, 5LL, 0);

    // For entries with the same session, the entry with a larger txnNumber is saved.
    auto lsidDiffTxn = makeLogicalSessionIdForTest();
    auto opDiffTxnSmaller = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(2), 0), 1LL}, ns1, BSON("_id" << 0), lsidDiffTxn, 10LL, 1);
    auto opDiffTxnLarger = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(3), 0), 1LL}, ns1, BSON("_id" << 1), lsidDiffTxn, 20LL, 1);

    // For entries with the same session and txnNumber, the later optime is saved.
    auto lsidSameTxn = makeLogicalSessionIdForTest();
    auto opSameTxnLater = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(6), 0), 1LL}, ns2, BSON("_id" << 0), lsidSameTxn, 30LL, 0);
    auto opSameTxnSooner = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(5), 0), 1LL}, ns2, BSON("_id" << 1), lsidSameTxn, 30LL, 1);

    // Entries with a session id but no txnNumber do not lead to updates.
    auto lsidNoTxn = makeLogicalSessionIdForTest();
    OperationSessionInfo info;
    info.setSessionId(lsidNoTxn);
    auto opNoTxn = makeInsertDocumentOplogEntryWithSessionInfo(
        {Timestamp(Seconds(7), 0), 1LL}, ns3, BSON("_id" << 0), info);

    auto writerPool = SyncTail::makeWriterPool();
    SyncTail syncTail(
        nullptr, getConsistencyMarkers(), getStorageInterface(), multiSyncApply, writerPool.get());
    ASSERT_OK(syncTail.multiApply(
        _opCtx.get(),
        {opSingle, opDiffTxnSmaller, opDiffTxnLarger, opSameTxnSooner, opSameTxnLater, opNoTxn}));

    // The txnNum and optime of the only write were saved.
    auto resultSingleDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidSingle.toBSON()));
    ASSERT_TRUE(!resultSingleDoc.isEmpty());

    auto resultSingle =
        SessionTxnRecord::parse(IDLParserErrorContext("resultSingleDoc test"), resultSingleDoc);

    ASSERT_EQ(resultSingle.getTxnNum(), 5LL);
    ASSERT_EQ(resultSingle.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(1), 0), 1));

    // The txnNum and optime of the write with the larger txnNum were saved.
    auto resultDiffTxnDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidDiffTxn.toBSON()));
    ASSERT_TRUE(!resultDiffTxnDoc.isEmpty());

    auto resultDiffTxn =
        SessionTxnRecord::parse(IDLParserErrorContext("resultDiffTxnDoc test"), resultDiffTxnDoc);

    ASSERT_EQ(resultDiffTxn.getTxnNum(), 20LL);
    ASSERT_EQ(resultDiffTxn.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(3), 0), 1));

    // The txnNum and optime of the write with the later optime were saved.
    auto resultSameTxnDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidSameTxn.toBSON()));
    ASSERT_TRUE(!resultSameTxnDoc.isEmpty());

    auto resultSameTxn =
        SessionTxnRecord::parse(IDLParserErrorContext("resultSameTxnDoc test"), resultSameTxnDoc);

    ASSERT_EQ(resultSameTxn.getTxnNum(), 30LL);
    ASSERT_EQ(resultSameTxn.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(6), 0), 1));

    // There is no entry for the write with no txnNumber.
    auto resultNoTxn =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidNoTxn.toBSON()));
    ASSERT_TRUE(resultNoTxn.isEmpty());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
