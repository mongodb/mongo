/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
// TODO SERVER-41882 This is a temporary inclusion to avoid duplicate definitions of free
// functions. Remove once sync_tail_test.cpp is fully merged in.
#include "mongo/db/repl/sync_tail_test_fixture.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace repl {
namespace {

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
        auto coll = db->createCollection(opCtx, nss, options);
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
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, dbName, &justCreated);
    ASSERT_TRUE(db);
    ASSERT_TRUE(justCreated);
}

DEATH_TEST_F(OplogApplierImplTest, MultiApplyAbortsWhenNoOperationsAreGiven, "!ops.empty()") {
    // TODO SERVER-43344 Currently, this file only tests multiApply, so several parameters have been
    // set to nullptr during OplogAppierImpl construction as they are not needed in multiApply.
    // Update constructors as needed once the rest of sync_tail_test.cpp has been merged in.
    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        noopApplyOperationFn,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());
    oplogApplier.multiApply(_opCtx.get(), {}).getStatus().ignore();
}

bool _testOplogEntryIsForCappedCollection(OperationContext* opCtx,
                                          ReplicationCoordinator* const replCoord,
                                          ReplicationConsistencyMarkers* const consistencyMarkers,
                                          StorageInterface* const storageInterface,
                                          const NamespaceString& nss,
                                          const CollectionOptions& options) {
    auto writerPool = makeReplWriterPool();
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

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        replCoord,
        consistencyMarkers,
        storageInterface,
        applyOperationFn,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());
    auto lastOpTime = unittest::assertGet(oplogApplier.multiApply(opCtx, {op}));
    ASSERT_EQUALS(op.getOpTime(), lastOpTime);

    ASSERT_EQUALS(1U, operationsApplied.size());
    const auto& opApplied = operationsApplied.front();
    ASSERT_EQUALS(op, opApplied);
    // "isForCappedCollection" is not parsed from raw oplog entry document.
    return opApplied.isForCappedCollection;
}

TEST_F(
    OplogApplierImplTest,
    MultiApplyDoesNotSetOplogEntryIsForCappedCollectionWhenProcessingNonCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_FALSE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                      ReplicationCoordinator::get(_opCtx.get()),
                                                      getConsistencyMarkers(),
                                                      getStorageInterface(),
                                                      nss,
                                                      CollectionOptions()));
}

TEST_F(OplogApplierImplTest,
       MultiApplySetsOplogEntryIsForCappedCollectionWhenProcessingCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                     ReplicationCoordinator::get(_opCtx.get()),
                                                     getConsistencyMarkers(),
                                                     getStorageInterface(),
                                                     nss,
                                                     createOplogCollectionOptions()));
}

class MultiOplogEntryOplogApplierImplTest : public OplogApplierImplTest {
public:
    MultiOplogEntryOplogApplierImplTest()
        : _nss1("test.preptxn1"), _nss2("test.preptxn2"), _txnNum(1) {}

protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        const NamespaceString cmdNss{"admin", "$cmd"};

        _uuid1 = createCollectionWithUuid(_opCtx.get(), _nss1);
        _uuid2 = createCollectionWithUuid(_opCtx.get(), _nss2);
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);

        _lsid = makeLogicalSessionId(_opCtx.get());

        _insertOp1 = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 1), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                               << BSON("_id" << 1)))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            StmtId(0),
            OpTime());
        _insertOp2 = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 2), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss2.ns() << "ui" << *_uuid2 << "o"
                                               << BSON("_id" << 2)))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            StmtId(1),
            _insertOp1->getOpTime());
        _commitOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 3), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss2.ns() << "ui" << *_uuid2 << "o"
                                               << BSON("_id" << 3)))),
            _lsid,
            _txnNum,
            StmtId(2),
            _insertOp2->getOpTime());
        _opObserver->onInsertsFn =
            [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
                stdx::lock_guard<Latch> lock(_insertMutex);
                if (nss.isOplog() || nss == _nss1 || nss == _nss2 ||
                    nss == NamespaceString::kSessionTransactionsTableNamespace) {
                    _insertedDocs[nss].insert(_insertedDocs[nss].end(), docs.begin(), docs.end());
                } else
                    FAIL("Unexpected insert") << " into " << nss << " first doc: " << docs.front();
            };

        _writerPool = makeReplWriterPool();
    }

    void tearDown() override {
        OplogApplierImplTest::tearDown();
    }

    void checkTxnTable(const LogicalSessionId& lsid,
                       const TxnNumber& txnNum,
                       const repl::OpTime& expectedOpTime,
                       Date_t expectedWallClock,
                       boost::optional<repl::OpTime> expectedStartOpTime,
                       DurableTxnStateEnum expectedState) {
        repl::checkTxnTable(_opCtx.get(),
                            lsid,
                            txnNum,
                            expectedOpTime,
                            expectedWallClock,
                            expectedStartOpTime,
                            expectedState);
    }

    std::vector<BSONObj>& oplogDocs() {
        return _insertedDocs[NamespaceString::kRsOplogNamespace];
    }

protected:
    NamespaceString _nss1;
    NamespaceString _nss2;
    boost::optional<UUID> _uuid1;
    boost::optional<UUID> _uuid2;
    LogicalSessionId _lsid;
    TxnNumber _txnNum;
    boost::optional<OplogEntry> _insertOp1, _insertOp2;
    boost::optional<OplogEntry> _commitOp;
    std::map<NamespaceString, std::vector<BSONObj>> _insertedDocs;
    std::unique_ptr<ThreadPool> _writerPool;

private:
    Mutex _insertMutex = MONGO_MAKE_LATCH("MultiOplogEntryOplogApplierImplTest::_insertMutex");
};

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyUnpreparedTransactionSeparate) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Apply a batch with only the first operation.  This should result in the first oplog entry
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp1}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(oplogDocs().back(), _insertOp1->getRaw());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the second operation.  This should result in the second oplog entry
    // being put in the oplog, but with no effect because the operation is part of a pending
    // transaction.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp2}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(oplogDocs().back(), _insertOp2->getRaw());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    // The transaction table should not have been updated for partialTxn operations that are not the
    // first in a transaction.
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the two previous entries being applied.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    ASSERT_BSONOBJ_EQ(oplogDocs().back(), _commitOp->getRaw());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitOp->getOpTime(),
                  _commitOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyUnpreparedTransactionAllAtOnce) {
    // Skipping writes to oplog proves we're testing the code path which does not rely on reading
    // the oplog.
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kRecovering),
        _writerPool.get());

    // Apply both inserts and the commit in a single batch.  We expect no oplog entries to
    // be inserted (because we've set skipWritesToOplog), and both entries to be committed.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp1, *_insertOp2, *_commitOp}));
    ASSERT_EQ(0U, oplogDocs().size());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitOp->getOpTime(),
                  _commitOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyUnpreparedTransactionTwoBatches) {
    // Tests an unprepared transaction with ops both in the batch with the commit and prior
    // batches.
    // Populate transaction with 4 linked inserts, one in nss2 and the others in nss1.
    std::vector<OplogEntry> insertOps;
    std::vector<BSONObj> insertDocs;

    const NamespaceString cmdNss{"admin", "$cmd"};
    for (int i = 0; i < 4; i++) {
        insertDocs.push_back(BSON("_id" << i));
        insertOps.push_back(makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), i + 1), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << (i == 1 ? _nss2.ns() : _nss1.ns()) << "ui"
                                               << (i == 1 ? *_uuid2 : *_uuid1) << "o"
                                               << insertDocs.back()))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            StmtId(i),
            i == 0 ? OpTime() : insertOps.back().getOpTime()));
    }
    auto commitOp = makeCommandOplogEntryWithSessionInfoAndStmtId({Timestamp(Seconds(1), 5), 1LL},
                                                                  cmdNss,
                                                                  BSON("applyOps" << BSONArray()),
                                                                  _lsid,
                                                                  _txnNum,
                                                                  StmtId(4),
                                                                  insertOps.back().getOpTime());

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Insert the first entry in its own batch.  This should result in the oplog entry being written
    // but the entry should not be applied as it is part of a pending transaction.
    const auto expectedStartOpTime = insertOps[0].getOpTime();
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {insertOps[0]}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_EQ(0U, _insertedDocs[_nss1].size());
    ASSERT_EQ(0U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  insertOps[0].getOpTime(),
                  insertOps[0].getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Insert the rest of the entries, including the commit.  These entries should be added to the
    // oplog, and all the entries including the first should be applied.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(),
                                      {insertOps[1], insertOps[2], insertOps[3], commitOp}));
    ASSERT_EQ(5U, oplogDocs().size());
    ASSERT_EQ(3U, _insertedDocs[_nss1].size());
    ASSERT_EQ(1U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  commitOp.getOpTime(),
                  commitOp.getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);

    // Check docs and ordering of docs in nss1.
    // The insert into nss2 is unordered with respect to those.
    ASSERT_BSONOBJ_EQ(insertDocs[0], _insertedDocs[_nss1][0]);
    ASSERT_BSONOBJ_EQ(insertDocs[1], _insertedDocs[_nss2].front());
    ASSERT_BSONOBJ_EQ(insertDocs[2], _insertedDocs[_nss1][1]);
    ASSERT_BSONOBJ_EQ(insertDocs[3], _insertedDocs[_nss1][2]);
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyTwoTransactionsOneBatch) {
    // Tests that two transactions on the same session ID in the same batch both
    // apply correctly.
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);


    std::vector<OplogEntry> insertOps1, insertOps2;
    const NamespaceString cmdNss{"admin", "$cmd"};
    insertOps1.push_back(makeCommandOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(1), 1), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 1)))
                        << "partialTxn" << true),
        _lsid,
        txnNum1,
        StmtId(0),
        OpTime()));
    insertOps1.push_back(makeCommandOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(1), 2), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 2)))
                        << "partialTxn" << true),

        _lsid,
        txnNum1,
        StmtId(1),
        insertOps1.back().getOpTime()));
    insertOps2.push_back(makeCommandOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(2), 1), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 3)))
                        << "partialTxn" << true),
        _lsid,
        txnNum2,
        StmtId(0),
        OpTime()));
    insertOps2.push_back(makeCommandOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(2), 2), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 4)))
                        << "partialTxn" << true),
        _lsid,
        txnNum2,
        StmtId(1),
        insertOps2.back().getOpTime()));
    auto commitOp1 = makeCommandOplogEntryWithSessionInfoAndStmtId({Timestamp(Seconds(1), 3), 1LL},
                                                                   _nss1,
                                                                   BSON("applyOps" << BSONArray()),
                                                                   _lsid,
                                                                   txnNum1,
                                                                   StmtId(2),
                                                                   insertOps1.back().getOpTime());
    auto commitOp2 = makeCommandOplogEntryWithSessionInfoAndStmtId({Timestamp(Seconds(2), 3), 1LL},
                                                                   _nss1,
                                                                   BSON("applyOps" << BSONArray()),
                                                                   _lsid,
                                                                   txnNum2,
                                                                   StmtId(2),
                                                                   insertOps2.back().getOpTime());

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Note the insert counter so we can check it later.  It is necessary to use opCounters as
    // inserts are idempotent so we will not detect duplicate inserts just by checking inserts in
    // the opObserver.
    int insertsBefore = replOpCounters.getInsert()->load();
    // Insert all the oplog entries in one batch.  All inserts should be executed, in order, exactly
    // once.
    ASSERT_OK(oplogApplier.multiApply(
        _opCtx.get(),
        {insertOps1[0], insertOps1[1], commitOp1, insertOps2[0], insertOps2[1], commitOp2}));
    ASSERT_EQ(6U, oplogDocs().size());
    ASSERT_EQ(4, replOpCounters.getInsert()->load() - insertsBefore);
    ASSERT_EQ(4U, _insertedDocs[_nss1].size());
    checkTxnTable(_lsid,
                  txnNum2,
                  commitOp2.getOpTime(),
                  commitOp2.getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);

    // Check docs and ordering of docs in nss1.
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), _insertedDocs[_nss1][0]);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), _insertedDocs[_nss1][1]);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), _insertedDocs[_nss1][2]);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), _insertedDocs[_nss1][3]);
}


class MultiOplogEntryPreparedTransactionTest : public MultiOplogEntryOplogApplierImplTest {
protected:
    void setUp() override {
        MultiOplogEntryOplogApplierImplTest::setUp();

        _prepareWithPrevOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 3), 1LL},
            _nss1,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss2.ns() << "ui" << *_uuid2 << "o"
                                               << BSON("_id" << 3)))
                            << "prepare" << true),
            _lsid,
            _txnNum,
            StmtId(2),
            _insertOp2->getOpTime());
        _singlePrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 3), 1LL},
            _nss1,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                               << BSON("_id" << 0)))
                            << "prepare" << true),
            _lsid,
            _txnNum,
            StmtId(0),
            OpTime());
        _commitPrepareWithPrevOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 4), 1LL},
            _nss1,
            BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(Seconds(1), 4)),
            _lsid,
            _txnNum,
            StmtId(3),
            _prepareWithPrevOp->getOpTime());
        _commitSinglePrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
            {Timestamp(Seconds(1), 4), 1LL},
            _nss1,
            BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(Seconds(1), 4)),
            _lsid,
            _txnNum,
            StmtId(1),
            _prepareWithPrevOp->getOpTime());
        _abortPrepareWithPrevOp =
            makeCommandOplogEntryWithSessionInfoAndStmtId({Timestamp(Seconds(1), 4), 1LL},
                                                          _nss1,
                                                          BSON("abortTransaction" << 1),
                                                          _lsid,
                                                          _txnNum,
                                                          StmtId(3),
                                                          _prepareWithPrevOp->getOpTime());
        _abortSinglePrepareApplyOp = _abortPrepareWithPrevOp =
            makeCommandOplogEntryWithSessionInfoAndStmtId({Timestamp(Seconds(1), 4), 1LL},
                                                          _nss1,
                                                          BSON("abortTransaction" << 1),
                                                          _lsid,
                                                          _txnNum,
                                                          StmtId(1),
                                                          _singlePrepareApplyOp->getOpTime());
    }

protected:
    boost::optional<OplogEntry> _commitPrepareWithPrevOp, _abortPrepareWithPrevOp,
        _singlePrepareApplyOp, _prepareWithPrevOp, _commitSinglePrepareApplyOp,
        _abortSinglePrepareApplyOp;

private:
    Mutex _insertMutex = MONGO_MAKE_LATCH("MultiOplogEntryPreparedTransactionTest::_insertMutex");
};

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyPreparedTransactionSteadyState) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Apply a batch with the insert operations.  This should result in the oplog entries
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_insertOp1->getRaw(), oplogDocs()[0]);
    ASSERT_BSONOBJ_EQ(_insertOp2->getRaw(), oplogDocs()[1]);
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare.  This should result in the prepare being put in the
    // oplog, and the two previous entries being applied (but in a transaction) along with the
    // nested insert in the prepare oplog entry.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_prepareWithPrevOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_prepareWithPrevOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the three previous entries being committed.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitPrepareWithPrevOp}));
    ASSERT_BSONOBJ_EQ(_commitPrepareWithPrevOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitPrepareWithPrevOp->getOpTime(),
                  _commitPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyAbortPreparedTransactionCheckTxnTable) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Apply a batch with the insert operations.  This should result in the oplog entries
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare.  This should result in the prepare being put in the
    // oplog, and the two previous entries being applied (but in a transaction) along with the
    // nested insert in the prepare oplog entry.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_prepareWithPrevOp}));
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the abort.  This should result in the abort being put in the
    // oplog and the transaction table being updated accordingly.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_abortPrepareWithPrevOp}));
    ASSERT_BSONOBJ_EQ(_abortPrepareWithPrevOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _abortPrepareWithPrevOp->getOpTime(),
                  _abortPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kAborted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyPreparedTransactionInitialSync) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kInitialSync),
        _writerPool.get());
    // Apply a batch with the insert operations.  This should result in the oplog entries
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_insertOp1->getRaw(), oplogDocs()[0]);
    ASSERT_BSONOBJ_EQ(_insertOp2->getRaw(), oplogDocs()[1]);
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, but, since this is initial sync, nothing else.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_prepareWithPrevOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_prepareWithPrevOp->getRaw(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the three previous entries being applied.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitPrepareWithPrevOp}));
    ASSERT_BSONOBJ_EQ(_commitPrepareWithPrevOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitPrepareWithPrevOp->getOpTime(),
                  _commitPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyPreparedTransactionRecovery) {
    // For recovery, the oplog must contain the operations before starting.
    for (auto&& entry :
         {*_insertOp1, *_insertOp2, *_prepareWithPrevOp, *_commitPrepareWithPrevOp}) {
        ASSERT_OK(getStorageInterface()->insertDocument(
            _opCtx.get(),
            NamespaceString::kRsOplogNamespace,
            {entry.toBSON(), entry.getOpTime().getTimestamp()},
            entry.getOpTime().getTerm()));
    }
    // Ignore docs inserted into oplog in setup.
    oplogDocs().clear();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kRecovering),
        _writerPool.get());

    // Apply a batch with the insert operations.  This should have no effect, because this is
    // recovery.
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare applyOps. This should have no effect, since this is
    // recovery.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_prepareWithPrevOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the the three previous entries
    // being applied.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitPrepareWithPrevOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitPrepareWithPrevOp->getOpTime(),
                  _commitPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplySingleApplyOpsPreparedTransaction) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());
    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, and the nested insert being applied (but in a transaction).
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_singlePrepareApplyOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_singlePrepareApplyOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and prepared insert being committed.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_commitSinglePrepareApplyOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyEmptyApplyOpsPreparedTransaction) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    auto emptyPrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(1), 3), 1LL},
        _nss1,
        BSON("applyOps" << BSONArray() << "prepare" << true),
        _lsid,
        _txnNum,
        StmtId(0),
        OpTime());
    const auto expectedStartOpTime = emptyPrepareApplyOp.getOpTime();

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, and the nested insert being applied (but in a transaction).
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {emptyPrepareApplyOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(emptyPrepareApplyOp.getRaw(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  emptyPrepareApplyOp.getOpTime(),
                  emptyPrepareApplyOp.getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and prepared insert being committed.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_commitSinglePrepareApplyOp->getRaw(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyAbortSingleApplyOpsPreparedTransaction) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();
    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, and the nested insert being applied (but in a transaction).
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_singlePrepareApplyOp}));
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the abort.  This should result in the abort being put in the
    // oplog and the transaction table being updated accordingly.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_abortSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_abortSinglePrepareApplyOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _abortSinglePrepareApplyOp->getOpTime(),
                  _abortSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kAborted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest,
       MultiApplySingleApplyOpsPreparedTransactionInitialSync) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kInitialSync),
        _writerPool.get());

    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, but, since this is initial sync, nothing else.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_singlePrepareApplyOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_singlePrepareApplyOp->getRaw(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the previous entry being applied.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_commitSinglePrepareApplyOp->getRaw(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest,
       MultiApplySingleApplyOpsPreparedTransactionRecovery) {
    // For recovery, the oplog must contain the operations before starting.
    for (auto&& entry : {*_singlePrepareApplyOp, *_commitPrepareWithPrevOp}) {
        ASSERT_OK(getStorageInterface()->insertDocument(
            _opCtx.get(),
            NamespaceString::kRsOplogNamespace,
            {entry.toBSON(), entry.getOpTime().getTimestamp()},
            entry.getOpTime().getTerm()));
    }
    // Ignore docs inserted into oplog in setup.
    oplogDocs().clear();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kRecovering),
        _writerPool.get());

    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();

    // Apply a batch with only the prepare applyOps. This should have no effect, since this is
    // recovery.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_singlePrepareApplyOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the previous entry being
    // applied.
    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

class OplogApplierImplTxnTableTest : public OplogApplierImplTest {
public:
    void setUp() override {
        OplogApplierImplTest::setUp();

        MongoDSessionCatalog::onStepUp(_opCtx.get());

        DBDirectClient client(_opCtx.get());
        BSONObj result;
        ASSERT(client.runCommand(kNs.db().toString(), BSON("create" << kNs.coll()), result));
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
                                boost::none,    // hash
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

    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    repl::OplogEntry makeOplogEntryForMigrate(const NamespaceString& ns,
                                              repl::OpTime opTime,
                                              repl::OpTypeEnum opType,
                                              BSONObj object,
                                              boost::optional<BSONObj> object2,
                                              const OperationSessionInfo& sessionInfo,
                                              Date_t wallClockTime) {
        return repl::OplogEntry(opTime,         // optime
                                boost::none,    // hash
                                opType,         // opType
                                ns,             // namespace
                                boost::none,    // uuid
                                true,           // fromMigrate
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

        repl::checkTxnTable(_opCtx.get(),
                            *sessionInfo.getSessionId(),
                            *sessionInfo.getTxnNumber(),
                            expectedOpTime,
                            expectedWallClock,
                            {},
                            {});
    }

    static const NamespaceString& nss() {
        return kNs;
    }

private:
    static const NamespaceString kNs;
};

const NamespaceString OplogApplierImplTxnTableTest::kNs("test.foo");

TEST_F(OplogApplierImplTxnTableTest, SimpleWriteWithTxn) {
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

    auto writerPool = makeReplWriterPool();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {insertOp}));

    checkTxnTable(sessionInfo, {Timestamp(1, 0), 1}, date);
}

TEST_F(OplogApplierImplTxnTableTest, WriteWithTxnMixedWithDirectWriteToTxnTable) {
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

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());


    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {insertOp, deleteOp}));

    ASSERT_FALSE(docExists(
        _opCtx.get(),
        NamespaceString::kSessionTransactionsTableNamespace,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())));
}

TEST_F(OplogApplierImplTxnTableTest, InterleavedWriteWithTxnMixedWithDirectDeleteToTxnTable) {
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

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {insertOp, deleteOp, insertOp2}));

    checkTxnTable(sessionInfo, {Timestamp(3, 0), 2}, date);
}

TEST_F(OplogApplierImplTxnTableTest, InterleavedWriteWithTxnMixedWithDirectUpdateToTxnTable) {
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

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {insertOp, updateOp}));

    checkTxnTable(sessionInfo, newWriteOpTime, date);
}

TEST_F(OplogApplierImplTxnTableTest, RetryableWriteThenMultiStatementTxnWriteOnSameSession) {
    const NamespaceString cmdNss{"admin", "$cmd"};
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();
    auto uuid = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), nss()).getCollection()->uuid();
    }();

    repl::OpTime retryableInsertOpTime(Timestamp(1, 0), 1);

    auto retryableInsertOp = makeOplogEntry(nss(),
                                            retryableInsertOpTime,
                                            repl::OpTypeEnum::kInsert,
                                            BSON("_id" << 1),
                                            boost::none,
                                            sessionInfo,
                                            date);

    repl::OpTime txnInsertOpTime(Timestamp(2, 0), 1);
    sessionInfo.setTxnNumber(4);

    auto txnInsertOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
        txnInsertOpTime,
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss().ns() << "ui" << uuid << "o"
                                           << BSON("_id" << 2)))
                        << "partialTxn" << true),
        sessionId,
        *sessionInfo.getTxnNumber(),
        StmtId(0),
        OpTime());

    repl::OpTime txnCommitOpTime(Timestamp(3, 0), 1);
    auto txnCommitOp =
        makeCommandOplogEntryWithSessionInfoAndStmtId(txnCommitOpTime,
                                                      cmdNss,
                                                      BSON("applyOps" << BSONArray()),
                                                      sessionId,
                                                      *sessionInfo.getTxnNumber(),
                                                      StmtId(1),
                                                      txnInsertOpTime);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {retryableInsertOp, txnInsertOp, txnCommitOp}));

    repl::checkTxnTable(_opCtx.get(),
                        *sessionInfo.getSessionId(),
                        *sessionInfo.getTxnNumber(),
                        txnCommitOpTime,
                        txnCommitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
}

TEST_F(OplogApplierImplTxnTableTest, MultiStatementTxnWriteThenRetryableWriteOnSameSession) {
    const NamespaceString cmdNss{"admin", "$cmd"};
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();
    auto uuid = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), nss()).getCollection()->uuid();
    }();

    repl::OpTime txnInsertOpTime(Timestamp(1, 0), 1);
    auto txnInsertOp = makeCommandOplogEntryWithSessionInfoAndStmtId(
        txnInsertOpTime,
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss().ns() << "ui" << uuid << "o"
                                           << BSON("_id" << 2)))
                        << "partialTxn" << true),
        sessionId,
        *sessionInfo.getTxnNumber(),
        StmtId(0),
        OpTime());

    repl::OpTime txnCommitOpTime(Timestamp(2, 0), 1);
    auto txnCommitOp =
        makeCommandOplogEntryWithSessionInfoAndStmtId(txnCommitOpTime,
                                                      cmdNss,
                                                      BSON("applyOps" << BSONArray()),
                                                      sessionId,
                                                      *sessionInfo.getTxnNumber(),
                                                      StmtId(1),
                                                      txnInsertOpTime);

    repl::OpTime retryableInsertOpTime(Timestamp(3, 0), 1);
    sessionInfo.setTxnNumber(4);

    auto retryableInsertOp = makeOplogEntry(nss(),
                                            retryableInsertOpTime,
                                            repl::OpTypeEnum::kInsert,
                                            BSON("_id" << 1),
                                            boost::none,
                                            sessionInfo,
                                            date);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {txnInsertOp, txnCommitOp, retryableInsertOp}));

    repl::checkTxnTable(_opCtx.get(),
                        *sessionInfo.getSessionId(),
                        *sessionInfo.getTxnNumber(),
                        retryableInsertOpTime,
                        retryableInsertOp.getWallClockTime(),
                        boost::none,
                        boost::none);
}


TEST_F(OplogApplierImplTxnTableTest, MultiApplyUpdatesTheTransactionTable) {
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
    auto uuid0 = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), ns0).getCollection()->uuid();
    }();
    auto uuid1 = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), ns1).getCollection()->uuid();
    }();
    auto uuid2 = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), ns2).getCollection()->uuid();
    }();

    // Entries with a session id and a txnNumber update the transaction table.
    auto lsidSingle = makeLogicalSessionIdForTest();
    auto opSingle = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(1), 0), 1LL}, ns0, uuid0, BSON("_id" << 0), lsidSingle, 5LL, 0);

    // For entries with the same session, the entry with a larger txnNumber is saved.
    auto lsidDiffTxn = makeLogicalSessionIdForTest();
    auto opDiffTxnSmaller = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(2), 0), 1LL}, ns1, uuid1, BSON("_id" << 0), lsidDiffTxn, 10LL, 1);
    auto opDiffTxnLarger = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(3), 0), 1LL}, ns1, uuid1, BSON("_id" << 1), lsidDiffTxn, 20LL, 1);

    // For entries with the same session and txnNumber, the later optime is saved.
    auto lsidSameTxn = makeLogicalSessionIdForTest();
    auto opSameTxnLater = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(6), 0), 1LL}, ns2, uuid2, BSON("_id" << 0), lsidSameTxn, 30LL, 0);
    auto opSameTxnSooner = makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(
        {Timestamp(Seconds(5), 0), 1LL}, ns2, uuid2, BSON("_id" << 1), lsidSameTxn, 30LL, 1);

    // Entries with a session id but no txnNumber do not lead to updates.
    auto lsidNoTxn = makeLogicalSessionIdForTest();
    OperationSessionInfo info;
    info.setSessionId(lsidNoTxn);
    auto opNoTxn = makeInsertDocumentOplogEntryWithSessionInfo(
        {Timestamp(Seconds(7), 0), 1LL}, ns3, BSON("_id" << 0), info);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(
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

TEST_F(OplogApplierImplTxnTableTest, SessionMigrationNoOpEntriesShouldUpdateTxnTable) {
    const auto insertLsid = makeLogicalSessionIdForTest();
    OperationSessionInfo insertSessionInfo;
    insertSessionInfo.setSessionId(insertLsid);
    insertSessionInfo.setTxnNumber(3);
    auto date = Date_t::now();

    auto innerOplog = makeOplogEntry(nss(),
                                     {Timestamp(10, 10), 1},
                                     repl::OpTypeEnum::kInsert,
                                     BSON("_id" << 1),
                                     boost::none,
                                     insertSessionInfo,
                                     date);

    auto outerInsertDate = Date_t::now();
    auto insertOplog = makeOplogEntryForMigrate(nss(),
                                                {Timestamp(40, 0), 1},
                                                repl::OpTypeEnum::kNoop,
                                                BSON("$sessionMigrateInfo" << 1),
                                                innerOplog.toBSON(),
                                                insertSessionInfo,
                                                outerInsertDate);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {insertOplog}));

    checkTxnTable(insertSessionInfo, {Timestamp(40, 0), 1}, outerInsertDate);
}

TEST_F(OplogApplierImplTxnTableTest, PreImageNoOpEntriesShouldNotUpdateTxnTable) {
    const auto preImageLsid = makeLogicalSessionIdForTest();
    OperationSessionInfo preImageSessionInfo;
    preImageSessionInfo.setSessionId(preImageLsid);
    preImageSessionInfo.setTxnNumber(3);
    auto preImageDate = Date_t::now();

    auto preImageOplog = makeOplogEntryForMigrate(nss(),
                                                  {Timestamp(30, 0), 1},
                                                  repl::OpTypeEnum::kNoop,
                                                  BSON("_id" << 1),
                                                  boost::none,
                                                  preImageSessionInfo,
                                                  preImageDate);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {preImageOplog}));

    ASSERT_FALSE(docExists(_opCtx.get(),
                           NamespaceString::kSessionTransactionsTableNamespace,
                           BSON(SessionTxnRecord::kSessionIdFieldName
                                << preImageSessionInfo.getSessionId()->toBSON())));
}

TEST_F(OplogApplierImplTxnTableTest, NonMigrateNoOpEntriesShouldNotUpdateTxnTable) {
    const auto lsid = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(3);

    auto oplog = makeOplogEntry(nss(),
                                {Timestamp(30, 0), 1},
                                repl::OpTypeEnum::kNoop,
                                BSON("_id" << 1),
                                boost::none,
                                sessionInfo,
                                Date_t::now());

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        multiSyncApply,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.multiApply(_opCtx.get(), {oplog}));

    ASSERT_FALSE(docExists(
        _opCtx.get(),
        NamespaceString::kSessionTransactionsTableNamespace,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
