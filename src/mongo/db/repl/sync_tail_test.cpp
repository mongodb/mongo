/**
 *    Copyright 2015 MongoDB Inc.
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
#include <vector>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/string_map.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class SyncTailTest : public ServiceContextMongoDTest {
protected:
    void _testSyncApplyInsertDocument(LockMode expectedMode);
    ServiceContext::UniqueOperationContext _txn;
    unsigned int _opsApplied;
    SyncTail::ApplyOperationInLockFn _applyOp;
    SyncTail::ApplyCommandInLockFn _applyCmd;
    SyncTail::IncrementOpsAppliedStatsFn _incOps;
    StorageInterfaceMock* _storageInterface = nullptr;

private:
    void setUp() override;
    void tearDown() override;
};

/**
 * Testing-only SyncTail that returns user-provided "document" for getMissingDoc().
 */
class SyncTailWithLocalDocumentFetcher : public SyncTail {
public:
    SyncTailWithLocalDocumentFetcher(const BSONObj& document);
    BSONObj getMissingDoc(OperationContext* txn, Database* db, const BSONObj& o) override;

private:
    BSONObj _document;
};

/**
 * Testing-only SyncTail that checks the operation context in shouldRetry().
 */
class SyncTailWithOperationContextChecker : public SyncTail {
public:
    SyncTailWithOperationContextChecker();
    bool shouldRetry(OperationContext* txn, const BSONObj& o) override;
};

void SyncTailTest::setUp() {
    ServiceContextMongoDTest::setUp();
    ReplSettings replSettings;
    replSettings.setOplogSizeBytes(5 * 1024 * 1024);

    auto serviceContext = mongo::getGlobalServiceContext();
    ReplicationCoordinator::set(serviceContext,
                                stdx::make_unique<ReplicationCoordinatorMock>(replSettings));
    auto storageInterface = stdx::make_unique<StorageInterfaceMock>();
    _storageInterface = storageInterface.get();
    StorageInterface::set(serviceContext, std::move(storageInterface));

    Client::initThreadIfNotAlready();
    _txn = cc().makeOperationContext();
    _opsApplied = 0;
    _applyOp = [](OperationContext* txn,
                  Database* db,
                  const BSONObj& op,
                  bool convertUpdateToUpsert,
                  stdx::function<void()>) { return Status::OK(); };
    _applyCmd = [](OperationContext* txn, const BSONObj& op) { return Status::OK(); };
    _incOps = [this]() { _opsApplied++; };
}

void SyncTailTest::tearDown() {
    if (_txn) {
        Lock::GlobalWrite globalLock(_txn->lockState());
        BSONObjBuilder unused;
        invariant(mongo::dbHolder().closeAll(_txn.get(), unused, false));
    }
    _txn.reset();
    _storageInterface = nullptr;
}

SyncTailWithLocalDocumentFetcher::SyncTailWithLocalDocumentFetcher(const BSONObj& document)
    : SyncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr), _document(document) {}

BSONObj SyncTailWithLocalDocumentFetcher::getMissingDoc(OperationContext*,
                                                        Database*,
                                                        const BSONObj&) {
    return _document;
}

SyncTailWithOperationContextChecker::SyncTailWithOperationContextChecker()
    : SyncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr) {}

bool SyncTailWithOperationContextChecker::shouldRetry(OperationContext* txn, const BSONObj&) {
    ASSERT_FALSE(txn->writesAreReplicated());
    ASSERT_TRUE(txn->lockState()->isBatchWriter());
    ASSERT_TRUE(documentValidationDisabled(txn));
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
void createCollection(OperationContext* txn,
                      const NamespaceString& nss,
                      const CollectionOptions& options) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), nss.db(), MODE_X);
        OldClientContext ctx(txn, nss.ns());
        auto db = ctx.db();
        ASSERT_TRUE(db);
        mongo::WriteUnitOfWork wuow(txn);
        auto coll = db->createCollection(txn, nss.ns(), options);
        ASSERT_TRUE(coll);
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", nss.ns());
}

/**
 * Creates a create collection oplog entry with given optime.
 */
OplogEntry makeCreateCollectionOplogEntry(OpTime opTime,
                                          const NamespaceString& nss = NamespaceString("test.t")) {
    BSONObjBuilder bob;
    bob.appendElements(opTime.toBSON());
    bob.append("h", 1LL);
    bob.append("op", "c");
    bob.append("ns", nss.ns());
    return OplogEntry(bob.obj());
}

/**
 * Creates an insert oplog entry with given optime and namespace.
 */
OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert) {
    BSONObjBuilder bob;
    bob.appendElements(opTime.toBSON());
    bob.append("h", 1LL);
    bob.append("op", "i");
    bob.append("ns", nss.ns());
    bob.append("o", documentToInsert);
    return OplogEntry(bob.obj());
}

/**
 * Creates an update oplog entry with given optime and namespace.
 */
OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument) {
    BSONObjBuilder bob;
    bob.appendElements(opTime.toBSON());
    bob.append("h", 1LL);
    bob.append("op", "u");
    bob.append("ns", nss.ns());
    bob.append("o2", documentToUpdate);
    bob.append("o", updatedDocument);
    return OplogEntry(bob.obj());
}

TEST_F(SyncTailTest, SyncApplyNoNamespaceBadOp) {
    const BSONObj op = BSON("op"
                            << "x");
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, _applyOp, _applyCmd, _incOps));
    ASSERT_EQUALS(0U, _opsApplied);
}

TEST_F(SyncTailTest, SyncApplyNoNamespaceNoOp) {
    ASSERT_OK(SyncTail::syncApply(_txn.get(),
                                  BSON("op"
                                       << "n"),
                                  false));
    ASSERT_EQUALS(0U, _opsApplied);
}

TEST_F(SyncTailTest, SyncApplyBadOp) {
    const BSONObj op = BSON("op"
                            << "x"
                            << "ns"
                            << "test.t");
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  SyncTail::syncApply(_txn.get(), op, false, _applyOp, _applyCmd, _incOps).code());
    ASSERT_EQUALS(0U, _opsApplied);
}

TEST_F(SyncTailTest, SyncApplyNoOp) {
    const BSONObj op = BSON("op"
                            << "n"
                            << "ns"
                            << "test.t");
    bool applyOpCalled = false;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* txn,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool convertUpdateToUpsert,
                                                   stdx::function<void()>) {
        applyOpCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isDbLockedForMode("test", MODE_X));
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(db);
        ASSERT_EQUALS(op, theOperation);
        ASSERT_FALSE(convertUpdateToUpsert);
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd = [&](OperationContext* txn,
                                                  const BSONObj& theOperation) {
        FAIL("applyCommand unexpectedly invoked.");
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, applyCmd, _incOps));
    ASSERT_TRUE(applyOpCalled);
}

TEST_F(SyncTailTest, SyncApplyNoOpApplyOpThrowsException) {
    const BSONObj op = BSON("op"
                            << "n"
                            << "ns"
                            << "test.t");
    int applyOpCalled = 0;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* txn,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool convertUpdateToUpsert,
                                                   stdx::function<void()>) {
        applyOpCalled++;
        if (applyOpCalled < 5) {
            throw WriteConflictException();
        }
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd = [&](OperationContext* txn,
                                                  const BSONObj& theOperation) {
        FAIL("applyCommand unexpectedly invoked.");
        return Status::OK();
    };
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, applyCmd, _incOps));
    ASSERT_EQUALS(5, applyOpCalled);
}

void SyncTailTest::_testSyncApplyInsertDocument(LockMode expectedMode) {
    const BSONObj op = BSON("op"
                            << "i"
                            << "ns"
                            << "test.t");
    bool applyOpCalled = false;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* txn,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool convertUpdateToUpsert,
                                                   stdx::function<void()>) {
        applyOpCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isDbLockedForMode("test", expectedMode));
        ASSERT_TRUE(txn->lockState()->isCollectionLockedForMode("test.t", expectedMode));
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(db);
        ASSERT_EQUALS(op, theOperation);
        ASSERT_TRUE(convertUpdateToUpsert);
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd = [&](OperationContext* txn,
                                                  const BSONObj& theOperation) {
        FAIL("applyCommand unexpectedly invoked.");
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, true, applyOp, applyCmd, _incOps));
    ASSERT_TRUE(applyOpCalled);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentDatabaseMissing) {
    _testSyncApplyInsertDocument(MODE_X);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionMissing) {
    {
        Lock::GlobalWrite globalLock(_txn->lockState());
        bool justCreated = false;
        Database* db = dbHolder().openDb(_txn.get(), "test", &justCreated);
        ASSERT_TRUE(db);
        ASSERT_TRUE(justCreated);
    }
    _testSyncApplyInsertDocument(MODE_X);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionExists) {
    {
        Lock::GlobalWrite globalLock(_txn->lockState());
        bool justCreated = false;
        Database* db = dbHolder().openDb(_txn.get(), "test", &justCreated);
        ASSERT_TRUE(db);
        ASSERT_TRUE(justCreated);
        Collection* collection = db->createCollection(_txn.get(), "test.t");
        ASSERT_TRUE(collection);
    }
    _testSyncApplyInsertDocument(MODE_IX);
}

TEST_F(SyncTailTest, SyncApplyIndexBuild) {
    const BSONObj op = BSON("op"
                            << "i"
                            << "ns"
                            << "test.system.indexes");
    bool applyOpCalled = false;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* txn,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool convertUpdateToUpsert,
                                                   stdx::function<void()>) {
        applyOpCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isDbLockedForMode("test", MODE_X));
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(db);
        ASSERT_EQUALS(op, theOperation);
        ASSERT_FALSE(convertUpdateToUpsert);
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd = [&](OperationContext* txn,
                                                  const BSONObj& theOperation) {
        FAIL("applyCommand unexpectedly invoked.");
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, applyCmd, _incOps));
    ASSERT_TRUE(applyOpCalled);
}

TEST_F(SyncTailTest, SyncApplyCommand) {
    const BSONObj op = BSON("op"
                            << "c"
                            << "ns"
                            << "test.t");
    bool applyCmdCalled = false;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* txn,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool convertUpdateToUpsert,
                                                   stdx::function<void()>) {
        FAIL("applyOperation unexpectedly invoked.");
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd = [&](OperationContext* txn,
                                                  const BSONObj& theOperation) {
        applyCmdCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isW());
        ASSERT_TRUE(txn->writesAreReplicated());
        ASSERT_FALSE(documentValidationDisabled(txn));
        ASSERT_EQUALS(op, theOperation);
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, applyCmd, _incOps));
    ASSERT_TRUE(applyCmdCalled);
    ASSERT_EQUALS(1U, _opsApplied);
}

TEST_F(SyncTailTest, SyncApplyCommandThrowsException) {
    const BSONObj op = BSON("op"
                            << "c"
                            << "ns"
                            << "test.t");
    int applyCmdCalled = 0;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* txn,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool convertUpdateToUpsert,
                                                   stdx::function<void()>) {
        FAIL("applyOperation unexpectedly invoked.");
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd = [&](OperationContext* txn,
                                                  const BSONObj& theOperation) {
        applyCmdCalled++;
        if (applyCmdCalled < 5) {
            throw WriteConflictException();
        }
        return Status::OK();
    };
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, applyCmd, _incOps));
    ASSERT_EQUALS(5, applyCmdCalled);
    ASSERT_EQUALS(1U, _opsApplied);
}

TEST_F(SyncTailTest, MultiApplyReturnsBadValueOnNullOperationContext) {
    auto writerPool = SyncTail::makeWriterPool();
    auto applyOperationFn = [](const MultiApplier::Operations&) {};
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = multiApply(nullptr, writerPool.get(), {op}, applyOperationFn).getStatus();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "invalid operation context");
}

TEST_F(SyncTailTest, MultiApplyReturnsBadValueOnNullWriterPool) {
    auto applyOperationFn = [](const MultiApplier::Operations&) {};
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = multiApply(_txn.get(), nullptr, {op}, applyOperationFn).getStatus();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "invalid worker pool");
}

TEST_F(SyncTailTest, MultiApplyReturnsEmptyArrayOperationWhenNoOperationsAreGiven) {
    auto writerPool = SyncTail::makeWriterPool();
    auto applyOperationFn = [](const MultiApplier::Operations&) {};
    auto status = multiApply(_txn.get(), writerPool.get(), {}, applyOperationFn).getStatus();
    ASSERT_EQUALS(ErrorCodes::EmptyArrayOperation, status);
    ASSERT_STRING_CONTAINS(status.reason(), "no operations provided to multiApply");
}

TEST_F(SyncTailTest, MultiApplyReturnsBadValueOnNullApplyOperation) {
    auto writerPool = SyncTail::makeWriterPool();
    MultiApplier::ApplyOperationFn nullApplyOperationFn;
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = multiApply(_txn.get(), writerPool.get(), {op}, nullApplyOperationFn).getStatus();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "invalid apply operation function");
}

bool _testOplogEntryIsForCappedCollection(OperationContext* txn,
                                          const NamespaceString& nss,
                                          const CollectionOptions& options) {
    auto writerPool = SyncTail::makeWriterPool();
    MultiApplier::Operations operationsApplied;
    auto applyOperationFn =
        [&operationsApplied](const MultiApplier::Operations& operationsToApply) {
            operationsApplied = operationsToApply;
        };
    createCollection(txn, nss, options);

    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("a" << 1));
    ASSERT_FALSE(op.isForCappedCollection);

    auto lastOpTime =
        unittest::assertGet(multiApply(txn, writerPool.get(), {op}, applyOperationFn));
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
    ASSERT_FALSE(_testOplogEntryIsForCappedCollection(_txn.get(), nss, CollectionOptions()));
}

TEST_F(SyncTailTest,
       MultiApplySetsOplogEntryIsForCappedCollectionWhenProcessingCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(
        _testOplogEntryIsForCappedCollection(_txn.get(), nss, createOplogCollectionOptions()));
}

TEST_F(SyncTailTest, MultiApplyAssignsOperationsToWriterThreadsBasedOnNamespaceHash) {
    NamespaceString nss1("test.t0");
    NamespaceString nss2("test.t1");
    OldThreadPool writerPool(2);

    // Ensure that namespaces are hashed to different threads in pool.
    ASSERT_EQUALS(0U, StringMapTraits::hash(nss1.ns()) % writerPool.getNumThreads());
    ASSERT_EQUALS(1U, StringMapTraits::hash(nss2.ns()) % writerPool.getNumThreads());

    stdx::mutex mutex;
    std::vector<MultiApplier::Operations> operationsApplied;
    auto applyOperationFn = [&mutex, &operationsApplied](
        const MultiApplier::Operations& operationsForWriterThreadToApply) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationsApplied.push_back(operationsForWriterThreadToApply);
    };

    auto op1 = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss1);
    auto op2 = makeCreateCollectionOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss2);

    auto lastOpTime =
        unittest::assertGet(multiApply(_txn.get(), &writerPool, {op1, op2}, applyOperationFn));
    ASSERT_EQUALS(op2.getOpTime(), lastOpTime);

    // Each writer thread should be given exactly one operation to apply.
    std::vector<OpTime> seen;
    {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        ASSERT_EQUALS(writerPool.getNumThreads(), operationsApplied.size());
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
    auto operationsWritternToOplog = _storageInterface->getOperationsWrittenToOplog();
    ASSERT_EQUALS(2U, operationsWritternToOplog.size());
    ASSERT_EQUALS(op1, operationsWritternToOplog[0]);
    ASSERT_EQUALS(op2, operationsWritternToOplog[1]);
}

TEST_F(SyncTailTest, MultiInitialSyncApplyDisablesDocumentValidationWhileApplyingOperations) {
    SyncTailWithOperationContextChecker syncTail;
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_FALSE(_txn->lockState()->isBatchWriter());
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), {op}, &syncTail));
}

TEST_F(SyncTailTest,
       MultiInitialSyncApplyDoesNotRetryFailedUpdateIfDocumentIsMissingFromSyncSource) {
    BSONObj emptyDoc;
    SyncTailWithLocalDocumentFetcher syncTail(emptyDoc);
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), {op}, &syncTail));

    // Since the missing document is not found on the sync source, the collection referenced by
    // the failed operation should not be automatically created.
    ASSERT_FALSE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

TEST_F(SyncTailTest, MultiInitialSyncApplyRetriesFailedUpdateIfDocumentIsAvailableFromSyncSource) {
    SyncTailWithLocalDocumentFetcher syncTail(BSON("_id" << 0 << "x" << 1));
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto updatedDocument = BSON("_id" << 0 << "x" << 2);
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), updatedDocument);
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), {op}, &syncTail));

    // The collection referenced by "ns" in the failed operation is automatically created to hold
    // the missing document fetched from the sync source. We verify the contents of the collection
    // with the OplogInterfaceLocal class.
    OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_EQUALS(updatedDocument, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
}

TEST_F(SyncTailTest, MultiInitialSyncApplyPassesThroughSyncApplyErrorAfterFailingToRetryBadOp) {
    SyncTailWithLocalDocumentFetcher syncTail(BSON("_id" << 0 << "x" << 1));
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto updatedDocument = BSON("_id" << 0 << "x" << 2);
    OplogEntry op(BSON("op"
                       << "x"
                       << "ns"
                       << nss.ns()));
    ASSERT_EQUALS(ErrorCodes::BadValue, multiInitialSyncApply_noAbort(_txn.get(), {op}, &syncTail));
}

TEST_F(SyncTailTest, MultiInitialSyncApplyPassesThroughShouldSyncTailRetryError) {
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto updatedDocument = BSON("_id" << 0 << "x" << 2);
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    ASSERT_THROWS_CODE(
        syncTail.shouldRetry(_txn.get(), op.raw), mongo::UserException, ErrorCodes::FailedToParse);
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  multiInitialSyncApply_noAbort(_txn.get(), {op}, &syncTail));
}

}  // namespace
