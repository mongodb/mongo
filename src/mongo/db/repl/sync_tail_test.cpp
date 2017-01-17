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

#include <memory>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"


namespace {

using namespace mongo;
using namespace mongo::repl;

class BackgroundSyncMock : public BackgroundSyncInterface {
public:
    bool peek(BSONObj* op) override;
    void consume() override;
    void waitForMore() override;
};

bool BackgroundSyncMock::peek(BSONObj* op) {
    return false;
}
void BackgroundSyncMock::consume() {}
void BackgroundSyncMock::waitForMore() {}

class SyncTailTest : public unittest::Test {
protected:
    void _testSyncApplyInsertDocument(LockMode expectedMode);

    std::unique_ptr<OperationContext> _txn;
    unsigned int _opsApplied;
    SyncTail::ApplyOperationInLockFn _applyOp;
    SyncTail::ApplyCommandInLockFn _applyCmd;
    SyncTail::IncrementOpsAppliedStatsFn _incOps;

private:
    void setUp() override;
    void tearDown() override;
};

void SyncTailTest::setUp() {
    ServiceContext* serviceContext = getGlobalServiceContext();
    if (!serviceContext->getGlobalStorageEngine()) {
        // When using the 'devnull' storage engine, it is fine for the temporary directory to
        // go away after the global storage engine is initialized.
        unittest::TempDir tempDir("sync_tail_test");
        mongo::storageGlobalParams.dbpath = tempDir.path();
        mongo::storageGlobalParams.engine = "ephemeralForTest";
        mongo::storageGlobalParams.engineSetByUser = true;
        serviceContext->initializeGlobalStorageEngine();
    }
    ReplSettings replSettings;
    replSettings.setOplogSizeBytes(5 * 1024 * 1024);

    setGlobalReplicationCoordinator(new ReplicationCoordinatorMock(replSettings));

    Client::initThreadIfNotAlready();
    _txn.reset(new OperationContextReplMock(&cc(), 0));
    _opsApplied = 0;
    _applyOp =
        [](OperationContext* txn, Database* db, const BSONObj& op, bool inSteadyStateReplication) {
            return Status::OK();
        };
    _applyCmd = [](OperationContext* txn, const BSONObj& op, bool) { return Status::OK(); };
    _incOps = [this]() { _opsApplied++; };
}

void SyncTailTest::tearDown() {
    ON_BLOCK_EXIT([&] { Client::destroy(); });

    dropAllDatabasesExceptLocal(_txn.get());
    {
        ScopedTransaction transaction(_txn.get(), MODE_X);
        Lock::GlobalWrite globalLock(_txn->lockState());
        AutoGetDb autoDBLocal(_txn.get(), "local", MODE_X);
        auto localDB = autoDBLocal.getDb();
        if (localDB) {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                // Do not wrap in a WriteUnitOfWork until SERVER-17103 is addressed.
                dropDatabase(_txn.get(), localDB);
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(_txn.get(), "_dropAllDBs", "local");
        }
        BSONObjBuilder unused;
        invariant(mongo::dbHolder().closeAll(_txn.get(), unused, false));
    }
    _txn.reset();
    setGlobalReplicationCoordinator(nullptr);
}

Status failedApplyCommand(OperationContext* txn, const BSONObj& theOperation, bool) {
    FAIL("applyCommand unexpectedly invoked.");
    return Status::OK();
}

TEST_F(SyncTailTest, Peek) {
    BackgroundSyncMock bgsync;
    SyncTail syncTail(&bgsync, [](const std::vector<BSONObj>& ops, SyncTail* st) {});
    BSONObj obj;
    ASSERT_FALSE(syncTail.peek(&obj));
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
                                                   bool inSteadyStateReplication) {
        applyOpCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isDbLockedForMode("test", MODE_X));
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(db);
        ASSERT_EQUALS(op, theOperation);
        ASSERT_FALSE(inSteadyStateReplication);
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, failedApplyCommand, _incOps));
    ASSERT_TRUE(applyOpCalled);
    ASSERT_EQUALS(1U, _opsApplied);
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
                                                   bool inSteadyStateReplication) {
        applyOpCalled++;
        if (applyOpCalled < 5) {
            throw WriteConflictException();
        }
        return Status::OK();
    };
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, failedApplyCommand, _incOps));
    ASSERT_EQUALS(5, applyOpCalled);
    ASSERT_EQUALS(1U, _opsApplied);
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
                                                   bool inSteadyStateReplication) {
        applyOpCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isDbLockedForMode("test", expectedMode));
        ASSERT_TRUE(txn->lockState()->isCollectionLockedForMode("test.t", expectedMode));
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(db);
        ASSERT_EQUALS(op, theOperation);
        ASSERT_TRUE(inSteadyStateReplication);
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, true, applyOp, failedApplyCommand, _incOps));
    ASSERT_TRUE(applyOpCalled);
    ASSERT_EQUALS(1U, _opsApplied);
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
        WriteUnitOfWork wunit(_txn.get());
        bool justCreated = false;
        Database* db = dbHolder().openDb(_txn.get(), "test", &justCreated);
        ASSERT_TRUE(db);
        ASSERT_TRUE(justCreated);
        Collection* collection = db->createCollection(_txn.get(), "test.t");
        wunit.commit();
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
                                                   bool inSteadyStateReplication) {
        applyOpCalled = true;
        ASSERT_TRUE(txn);
        ASSERT_TRUE(txn->lockState()->isDbLockedForMode("test", MODE_X));
        ASSERT_FALSE(txn->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(txn));
        ASSERT_TRUE(db);
        ASSERT_EQUALS(op, theOperation);
        ASSERT_FALSE(inSteadyStateReplication);
        return Status::OK();
    };
    ASSERT_TRUE(_txn->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_txn.get()));
    ASSERT_OK(SyncTail::syncApply(_txn.get(), op, false, applyOp, failedApplyCommand, _incOps));
    ASSERT_TRUE(applyOpCalled);
    ASSERT_EQUALS(1U, _opsApplied);
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
                                                   bool inSteadyStateReplication) {
        FAIL("applyOperation unexpectedly invoked.");
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation, bool) {
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
                                                   bool inSteadyStateReplication) {
        FAIL("applyOperation unexpectedly invoked.");
        return Status::OK();
    };
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation, bool) {
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

/**
 * Creates a command oplog entry with given optime and namespace.
 */
BSONObj makeCommandOplogEntry(OpTime opTime, const NamespaceString& nss, const BSONObj& command) {
    BSONObjBuilder bob;
    bob.appendElements(opTime.toBSON());
    bob.append("h", 1LL);
    bob.append("v", 2);
    bob.append("op", "c");
    bob.append("ns", nss.getCommandNS());
    bob.append("o", command);
    return bob.obj();
}

/**
 * Creates a create collection oplog entry with given optime.
 */
BSONObj makeCreateCollectionOplogEntry(OpTime opTime,
                                       const NamespaceString& nss = NamespaceString("test.foo"),
                                       const BSONObj& options = BSONObj()) {
    BSONObjBuilder bob;
    bob.append("create", nss.coll());
    bob.appendElements(options);
    return makeCommandOplogEntry(opTime, nss, bob.obj());
}

/**
 * Creates an insert oplog entry with given optime and namespace.
 */
BSONObj makeInsertDocumentOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const BSONObj& documentToInsert) {
    BSONObjBuilder bob;
    bob.appendElements(opTime.toBSON());
    bob.append("h", 1LL);
    bob.append("op", "i");
    bob.append("ns", nss.ns());
    bob.append("o", documentToInsert);
    return bob.obj();
}

class IdempotencyTest : public SyncTailTest {
protected:
    BSONObj createCollection();
    BSONObj buildIndex(const BSONObj& indexSpec, const BSONObj& options = BSONObj());
    BSONObj dropIndex(const std::string& indexName);
    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }
    Status runOp(const BSONObj& entry);
    Status runOps(std::initializer_list<BSONObj> ops);
    // Validate data and indexes. Return the MD5 hash of the documents ordered by _id.
    std::string validate();

    NamespaceString nss{"test.foo"};
    NamespaceString nssIndex{"test.system.indexes"};
};

Status IdempotencyTest::runOp(const BSONObj& op) {
    return runOps({op});
}

Status IdempotencyTest::runOps(std::initializer_list<BSONObj> ops) {
    std::vector<BSONObj> opsVector(ops);
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc());
    return multiInitialSyncApply_noAbort(_txn.get(), opsVector, &syncTail);
}

BSONObj IdempotencyTest::createCollection() {
    return makeCreateCollectionOplogEntry(nextOpTime(), nss);
}

BSONObj IdempotencyTest::buildIndex(const BSONObj& indexSpec, const BSONObj& options) {
    BSONObjBuilder bob;
    bob.append("v", 1);
    bob.append("key", indexSpec);
    bob.append("name", std::string(indexSpec.firstElementFieldName()) + "_index");
    bob.append("ns", nss.ns());
    bob.appendElementsUnique(options);
    return makeInsertDocumentOplogEntry(nextOpTime(), nssIndex, bob.obj());
}

BSONObj IdempotencyTest::dropIndex(const std::string& indexName) {
    auto cmd = BSON("deleteIndex" << nss.coll() << "index" << indexName);
    return makeCommandOplogEntry(nextOpTime(), nss, cmd);
}

std::string IdempotencyTest::validate() {
    auto collection = AutoGetCollectionForRead(_txn.get(), nss).getCollection();
    if (!collection) {
        return "CollectionNotFound";
    }
    ValidateResults validateResults;
    BSONObjBuilder bob;

    Lock::DBLock lk(_txn->lockState(), nss.db(), MODE_IS);
    Lock::CollectionLock lock(_txn->lockState(), nss.ns(), MODE_IS);
    ASSERT_OK(collection->validate(_txn.get(), true, true, &validateResults, &bob));
    ASSERT_TRUE(validateResults.valid);

    IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(_txn.get());
    ASSERT_TRUE(desc);
    auto exec = InternalPlanner::indexScan(_txn.get(),
                                           collection,
                                           desc,
                                           BSONObj(),
                                           BSONObj(),
                                           false,
                                           PlanExecutor::YIELD_MANUAL,
                                           InternalPlanner::FORWARD,
                                           InternalPlanner::IXSCAN_FETCH);
    ASSERT(NULL != exec.get());
    md5_state_t st;
    md5_init(&st);

    PlanExecutor::ExecState state;
    BSONObj c;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&c, NULL))) {
        md5_append(&st, (const md5_byte_t*)c.objdata(), c.objsize());
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    md5digest d;
    md5_finish(&st, d);
    return digestToString(d);
}

TEST_F(IdempotencyTest, CollModNamespaceNotFound) {
    getGlobalReplicationCoordinator()->setFollowerMode(MemberState::RS_RECOVERING);

    ASSERT_OK(runOp(createCollection()));
    ASSERT_OK(runOp(buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600))));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto collModOp = makeCommandOplogEntry(nextOpTime(), nss, collModCmd);
    auto dropCollOp = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));

    auto ops = {collModOp, dropCollOp};

    ASSERT_OK(runOps(ops));
    auto hash = validate();
    ASSERT_OK(runOps(ops));
    ASSERT_EQUALS(hash, validate());
}

TEST_F(IdempotencyTest, CollModIndexNotFound) {
    getGlobalReplicationCoordinator()->setFollowerMode(MemberState::RS_RECOVERING);

    ASSERT_OK(runOp(createCollection()));
    ASSERT_OK(runOp(buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600))));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto collModOp = makeCommandOplogEntry(nextOpTime(), nss, collModCmd);
    auto dropIndexOp = dropIndex("createdAt_index");

    auto ops = {collModOp, dropIndexOp};

    ASSERT_OK(runOps(ops));
    auto hash = validate();
    ASSERT_OK(runOps(ops));
    ASSERT_EQUALS(hash, validate());
}

TEST_F(IdempotencyTest, ResyncOnRenameCollection) {
    ReplicationCoordinator::get(_txn.get())->setFollowerMode(MemberState::RS_RECOVERING);

    auto cmd = BSON("renameCollection" << nss.ns() << "to"
                                       << "test.bar"
                                       << "stayTemp" << false << "dropTarget" << false);
    auto op = makeCommandOplogEntry(nextOpTime(), nss, cmd);
    ASSERT_EQUALS(runOp(op), ErrorCodes::OplogOperationUnsupported);
}

TEST_F(IdempotencyTest, MultiInitialSyncApplySkipsDocumentOnNamespaceNotFound) {
    BSONObj emptyDoc;
    NamespaceString nss("test", "foo");
    NamespaceString badNss("test", "bad");
    auto doc1 = BSON("_id" << 1);
    auto doc2 = BSON("_id" << 2);
    auto doc3 = BSON("_id" << 3);
    auto op0 = makeCreateCollectionOplogEntry(nextOpTime(), nss);
    auto op1 = makeInsertDocumentOplogEntry(nextOpTime(), nss, doc1);
    auto op2 = makeInsertDocumentOplogEntry(nextOpTime(), badNss, doc2);
    auto op3 = makeInsertDocumentOplogEntry(nextOpTime(), nss, doc3);
    runOps({op0, op1, op2, op3});

    OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
    auto iter = collectionReader.makeIterator();
    ASSERT_EQUALS(doc3, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(doc1, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
}

}  // namespace
