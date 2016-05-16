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

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

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

class SyncTailTest : public ServiceContextMongoDTest {
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
    ServiceContextMongoDTest::setUp();
    ReplSettings replSettings;
    replSettings.setOplogSizeBytes(5 * 1024 * 1024);

    setGlobalReplicationCoordinator(new ReplicationCoordinatorMock(replSettings));

    Client::initThreadIfNotAlready();
    _txn.reset(new OperationContextReplMock(&cc(), 0));
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
    {
        Lock::GlobalWrite globalLock(_txn->lockState());
        BSONObjBuilder unused;
        invariant(mongo::dbHolder().closeAll(_txn.get(), unused, false));
    }
    _txn.reset();
    setGlobalReplicationCoordinator(nullptr);
}

TEST_F(SyncTailTest, Peek) {
    BackgroundSyncMock bgsync;
    SyncTail syncTail(&bgsync, [](const std::vector<OplogEntry>& ops, SyncTail* st) {});
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
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation) {
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
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation) {
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
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation) {
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
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation) {
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
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation) {
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
    SyncTail::ApplyCommandInLockFn applyCmd =
        [&](OperationContext* txn, const BSONObj& theOperation) {
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

}  // namespace
