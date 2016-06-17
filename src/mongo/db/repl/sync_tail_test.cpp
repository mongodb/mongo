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
#include <utility>
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
#include "mongo/db/repl/oplog.h"
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

    // Implements the MultiApplier::ApplyOperationFn interface and does nothing.
    static void noopApplyOperationFn(MultiApplier::OperationPtrs*) {}

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
    storageInterface->insertDocumentsFn = [](OperationContext*,
                                             const NamespaceString&,
                                             const std::vector<BSONObj>&) { return Status::OK(); };
    StorageInterface::set(serviceContext, std::move(storageInterface));

    Client::initThreadIfNotAlready();
    _txn = cc().makeOperationContext();
    _txn->lockState()->setIsBatchWriter(false);
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
    bob.append("ns", nss.getCommandNS());
    bob.append("o", BSON("create" << nss.coll()));
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
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = multiApply(nullptr, writerPool.get(), {op}, noopApplyOperationFn).getStatus();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "invalid operation context");
}

TEST_F(SyncTailTest, MultiApplyReturnsBadValueOnNullWriterPool) {
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = multiApply(_txn.get(), nullptr, {op}, noopApplyOperationFn).getStatus();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "invalid worker pool");
}

TEST_F(SyncTailTest, MultiApplyReturnsEmptyArrayOperationWhenNoOperationsAreGiven) {
    auto writerPool = SyncTail::makeWriterPool();
    auto status = multiApply(_txn.get(), writerPool.get(), {}, noopApplyOperationFn).getStatus();
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
    auto applyOperationFn = [&operationsApplied](MultiApplier::OperationPtrs* operationsToApply) {
        for (auto&& opPtr : *operationsToApply) {
            operationsApplied.push_back(*opPtr);
        }
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
    // This test relies on implementation details of how multiApply uses hashing to distribute ops
    // to threads. It is possible for this test to fail, even if the implementation of multiApply is
    // correct. If it fails, consider adjusting the namespace names (to adjust the hash values) or
    // the number of threads in the pool.
    NamespaceString nss1("test.t0");
    NamespaceString nss2("test.t1");
    OldThreadPool writerPool(3);

    stdx::mutex mutex;
    std::vector<MultiApplier::Operations> operationsApplied;
    auto applyOperationFn = [&mutex, &operationsApplied](
        MultiApplier::OperationPtrs* operationsForWriterThreadToApply) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationsApplied.emplace_back();
        for (auto&& opPtr : *operationsForWriterThreadToApply) {
            operationsApplied.back().push_back(*opPtr);
        }
    };

    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss1, BSON("x" << 1));
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss2, BSON("x" << 2));

    NamespaceString nssForInsert;
    std::vector<BSONObj> operationsWrittenToOplog;
    _storageInterface->insertDocumentsFn = [&mutex, &nssForInsert, &operationsWrittenToOplog](
        OperationContext* txn, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        nssForInsert = nss;
        operationsWrittenToOplog = docs;
        return Status::OK();
    };

    auto lastOpTime =
        unittest::assertGet(multiApply(_txn.get(), &writerPool, {op1, op2}, applyOperationFn));
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
    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_EQUALS(2U, operationsWrittenToOplog.size());
    ASSERT_EQUALS(NamespaceString(rsOplogName), nssForInsert);
    ASSERT_EQUALS(op1.raw, operationsWrittenToOplog[0]);
    ASSERT_EQUALS(op2.raw, operationsWrittenToOplog[1]);
}

TEST_F(SyncTailTest, MultiSyncApplyUsesSyncApplyToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_FALSE(_txn->lockState()->isBatchWriter());
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    _txn.reset();

    MultiApplier::OperationPtrs ops = {&op};
    multiSyncApply(&ops, nullptr);
    // Collection should be created after SyncTail::syncApply() processes operation.
    _txn = cc().makeOperationContext();
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

TEST_F(SyncTailTest, MultiSyncApplyDisablesDocumentValidationWhileApplyingOperations) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_FALSE(_txn->lockState()->isBatchWriter());
    auto syncApply = [](OperationContext* txn, const BSONObj&, bool convertUpdatesToUpserts) {
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(txn->lockState()->isBatchWriter());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(convertUpdatesToUpserts);
        return Status::OK();
    };
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_OK(multiSyncApply_noAbort(_txn.get(), &ops, syncApply));
}

TEST_F(SyncTailTest, MultiSyncApplyPassesThroughSyncApplyErrorAfterFailingToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    OplogEntry op(BSON("op"
                       << "x"
                       << "ns"
                       << nss.ns()));
    auto syncApply = [](OperationContext*, const BSONObj&, bool) -> Status {
        return {ErrorCodes::OperationFailed, ""};
    };
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_EQUALS(ErrorCodes::OperationFailed, multiSyncApply_noAbort(_txn.get(), &ops, syncApply));
}

TEST_F(SyncTailTest, MultiSyncApplyPassesThroughSyncApplyException) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    OplogEntry op(BSON("op"
                       << "x"
                       << "ns"
                       << nss.ns()));
    auto syncApply = [](OperationContext*, const BSONObj&, bool) -> Status {
        uasserted(ErrorCodes::OperationFailed, "");
        MONGO_UNREACHABLE;
    };
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_EQUALS(ErrorCodes::OperationFailed, multiSyncApply_noAbort(_txn.get(), &ops, syncApply));
}

TEST_F(SyncTailTest, MultiSyncApplySortsOperationsStablyByNamespaceBeforeApplying) {
    int x = 0;
    auto makeOp = [&x](const char* ns) -> OplogEntry {
        return OplogEntry(BSON("op"
                               << "x"
                               << "ns"
                               << ns
                               << "x"
                               << x++));
    };
    auto op1 = makeOp("test.t1");
    auto op2 = makeOp("test.t1");
    auto op3 = makeOp("test.t2");
    auto op4 = makeOp("test.t3");
    MultiApplier::Operations operationsApplied;
    auto syncApply = [&operationsApplied](OperationContext*, const BSONObj& op, bool) {
        operationsApplied.push_back(OplogEntry(op));
        return Status::OK();
    };
    MultiApplier::OperationPtrs ops = {&op4, &op1, &op3, &op2};
    ASSERT_OK(multiSyncApply_noAbort(_txn.get(), &ops, syncApply));
    ASSERT_EQUALS(4U, operationsApplied.size());
    ASSERT_EQUALS(op1, operationsApplied[0]);
    ASSERT_EQUALS(op2, operationsApplied[1]);
    ASSERT_EQUALS(op3, operationsApplied[2]);
    ASSERT_EQUALS(op4, operationsApplied[3]);
}

TEST_F(SyncTailTest, MultiSyncApplyGroupsInsertOperationByNamespaceBeforeApplying) {
    int seconds = 0;
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
    MultiApplier::Operations operationsApplied;
    auto syncApply = [&operationsApplied](OperationContext*, const BSONObj& op, bool) {
        operationsApplied.push_back(OplogEntry(op));
        return Status::OK();
    };

    MultiApplier::OperationPtrs ops = {
        &createOp1, &createOp2, &insertOp1a, &insertOp2a, &insertOp1b, &insertOp2b};
    ASSERT_OK(multiSyncApply_noAbort(_txn.get(), &ops, syncApply));

    ASSERT_EQUALS(4U, operationsApplied.size());
    ASSERT_EQUALS(createOp1, operationsApplied[0]);
    ASSERT_EQUALS(createOp2, operationsApplied[1]);

    // Check grouped insert operations in namespace "nss1".
    ASSERT_EQUALS(insertOp1a.getOpTime(), operationsApplied[2].getOpTime());
    ASSERT_EQUALS(insertOp1a.ns, operationsApplied[2].ns);
    ASSERT_EQUALS(BSONType::Array, operationsApplied[2].o.type());
    auto group1 = operationsApplied[2].o.Array();
    ASSERT_EQUALS(2U, group1.size());
    ASSERT_EQUALS(insertOp1a.o.Obj(), group1[0].Obj());
    ASSERT_EQUALS(insertOp1b.o.Obj(), group1[1].Obj());

    // Check grouped insert operations in namespace "nss2".
    ASSERT_EQUALS(insertOp2a.getOpTime(), operationsApplied[3].getOpTime());
    ASSERT_EQUALS(insertOp2a.ns, operationsApplied[3].ns);
    ASSERT_EQUALS(BSONType::Array, operationsApplied[3].o.type());
    auto group2 = operationsApplied[3].o.Array();
    ASSERT_EQUALS(2U, group2.size());
    ASSERT_EQUALS(insertOp2a.o.Obj(), group2[0].Obj());
    ASSERT_EQUALS(insertOp2b.o.Obj(), group2[1].Obj());
}

TEST_F(SyncTailTest, MultiSyncApplyUsesLimitWhenGroupingInsertOperation) {
    int seconds = 0;
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
    MultiApplier::Operations operationsApplied;
    auto syncApply = [&operationsApplied](OperationContext*, const BSONObj& op, bool) {
        operationsApplied.push_back(OplogEntry(op));
        return Status::OK();
    };

    MultiApplier::OperationPtrs ops;
    for (auto&& op : operationsToApply) {
        ops.push_back(&op);
    }
    ASSERT_OK(multiSyncApply_noAbort(_txn.get(), &ops, syncApply));

    // multiSyncApply should combine operations as follows:
    // {create}, {grouped_insert}, {insert_(limit+1)}
    ASSERT_EQUALS(3U, operationsApplied.size());
    ASSERT_EQUALS(createOp, operationsApplied[0]);

    const auto& groupedInsertOp = operationsApplied[1];
    ASSERT_EQUALS(insertOps.front().getOpTime(), groupedInsertOp.getOpTime());
    ASSERT_EQUALS(insertOps.front().ns, groupedInsertOp.ns);
    ASSERT_EQUALS(BSONType::Array, groupedInsertOp.o.type());
    auto groupedInsertDocuments = groupedInsertOp.o.Array();
    ASSERT_EQUALS(limit, groupedInsertDocuments.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& insertOp = insertOps[i];
        ASSERT_EQUALS(insertOp.o.Obj(), groupedInsertDocuments[i].Obj());
    }

    // (limit + 1)-th insert operations should not be included in group of first (limit) inserts.
    ASSERT_EQUALS(insertOps.back(), operationsApplied[2]);
}

TEST_F(SyncTailTest, MultiSyncApplyFallsBackOnApplyingInsertsIndividuallyWhenGroupedInsertFails) {
    int seconds = 0;
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

    std::size_t numFailedGroupedInserts = 0;
    MultiApplier::Operations operationsApplied;
    auto syncApply = [&numFailedGroupedInserts,
                      &operationsApplied](OperationContext*, const BSONObj& op, bool) -> Status {
        // Reject grouped insert operations.
        if (op["o"].type() == BSONType::Array) {
            numFailedGroupedInserts++;
            return {ErrorCodes::OperationFailed, "grouped inserts not supported"};
        }
        operationsApplied.push_back(OplogEntry(op));
        return Status::OK();
    };

    MultiApplier::OperationPtrs ops;
    for (auto&& op : operationsToApply) {
        ops.push_back(&op);
    }
    ASSERT_OK(multiSyncApply_noAbort(_txn.get(), &ops, syncApply));

    // On failing to apply the grouped insert operation, multiSyncApply should apply the operations
    // as given in "operationsToApply":
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    ASSERT_EQUALS(limit + 2, operationsApplied.size());
    ASSERT_EQUALS(createOp, operationsApplied[0]);

    for (std::size_t i = 0; i < limit + 1; ++i) {
        const auto& insertOp = insertOps[i];
        ASSERT_EQUALS(insertOp, operationsApplied[i + 1]);
    }

    // Ensure that multiSyncApply does not attempt to group remaining operations in first failed
    // grouped insert operation.
    ASSERT_EQUALS(1U, numFailedGroupedInserts);
}

TEST_F(SyncTailTest, MultiInitialSyncApplyDisablesDocumentValidationWhileApplyingOperations) {
    SyncTailWithOperationContextChecker syncTail;
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_FALSE(_txn->lockState()->isBatchWriter());
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), &ops, &syncTail));
}

TEST_F(SyncTailTest,
       MultiInitialSyncApplyDoesNotRetryFailedUpdateIfDocumentIsMissingFromSyncSource) {
    BSONObj emptyDoc;
    SyncTailWithLocalDocumentFetcher syncTail(emptyDoc);
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), &ops, &syncTail));

    // Since the missing document is not found on the sync source, the collection referenced by
    // the failed operation should not be automatically created.
    ASSERT_FALSE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

TEST_F(SyncTailTest, MultiInitialSyncApplySkipsDocumentOnNamespaceNotFound) {
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
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), &ops, &syncTail));

    OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_EQUALS(doc3, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(doc1, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(SyncTailTest, MultiInitialSyncApplyRetriesFailedUpdateIfDocumentIsAvailableFromSyncSource) {
    SyncTailWithLocalDocumentFetcher syncTail(BSON("_id" << 0 << "x" << 1));
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto updatedDocument = BSON("_id" << 0 << "x" << 2);
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), updatedDocument);
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_OK(multiInitialSyncApply_noAbort(_txn.get(), &ops, &syncTail));

    // The collection referenced by "ns" in the failed operation is automatically created to hold
    // the missing document fetched from the sync source. We verify the contents of the collection
    // with the OplogInterfaceLocal class.
    OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_EQUALS(updatedDocument, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(SyncTailTest, MultiInitialSyncApplyPassesThroughSyncApplyErrorAfterFailingToRetryBadOp) {
    SyncTailWithLocalDocumentFetcher syncTail(BSON("_id" << 0 << "x" << 1));
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    OplogEntry op(BSON("op"
                       << "x"
                       << "ns"
                       << nss.ns()));
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_EQUALS(ErrorCodes::BadValue, multiInitialSyncApply_noAbort(_txn.get(), &ops, &syncTail));
}

TEST_F(SyncTailTest, MultiInitialSyncApplyPassesThroughShouldSyncTailRetryError) {
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    ASSERT_THROWS_CODE(
        syncTail.shouldRetry(_txn.get(), op.raw), mongo::UserException, ErrorCodes::FailedToParse);
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  multiInitialSyncApply_noAbort(_txn.get(), &ops, &syncTail));
}

}  // namespace
