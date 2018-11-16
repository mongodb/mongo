
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

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/recover_transaction_decision_from_local_participant.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const OptionalCollectionUUID kUUID;

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                BSONObj object,
                                OperationSessionInfo sessionInfo,
                                boost::optional<Date_t> wallClockTime,
                                boost::optional<StmtId> stmtId,
                                boost::optional<repl::OpTime> prevWriteOpTimeInTransaction) {
    return repl::OplogEntry(
        opTime,                        // optime
        0,                             // hash
        opType,                        // opType
        kNss,                          // namespace
        boost::none,                   // uuid
        boost::none,                   // fromMigrate
        0,                             // version
        object,                        // o
        boost::none,                   // o2
        sessionInfo,                   // sessionInfo
        boost::none,                   // upsert
        wallClockTime,                 // wall clock time
        stmtId,                        // statement id
        prevWriteOpTimeInTransaction,  // optime of previous write within same transaction
        boost::none,                   // pre-image optime
        boost::none);                  // post-image optime
}

class OpObserverMock : public OpObserverNoop {
public:
    void onTransactionPrepare(OperationContext* opCtx, const OplogSlot& prepareOpTime) override;

    bool onTransactionPrepareThrowsException = false;
    bool transactionPrepared = false;
    stdx::function<void()> onTransactionPrepareFn = []() {};

    void onTransactionCommit(OperationContext* opCtx,
                             boost::optional<OplogSlot> commitOplogEntryOpTime,
                             boost::optional<Timestamp> commitTimestamp) override;
    bool onTransactionCommitThrowsException = false;
    bool transactionCommitted = false;
    stdx::function<void(boost::optional<OplogSlot>, boost::optional<Timestamp>)>
        onTransactionCommitFn = [](boost::optional<OplogSlot> commitOplogEntryOpTime,
                                   boost::optional<Timestamp> commitTimestamp) {};

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override;
    bool onTransactionAbortThrowsException = false;
    bool transactionAborted = false;
    stdx::function<void()> onTransactionAbortFn = []() {};

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  CollectionDropType dropType) override;

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

void OpObserverMock::onTransactionPrepare(OperationContext* opCtx, const OplogSlot& prepareOpTime) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onTransactionPrepare(opCtx, prepareOpTime);

    uassert(ErrorCodes::OperationFailed,
            "onTransactionPrepare() failed",
            !onTransactionPrepareThrowsException);
    transactionPrepared = true;
    onTransactionPrepareFn();
}

void OpObserverMock::onTransactionCommit(OperationContext* opCtx,
                                         boost::optional<OplogSlot> commitOplogEntryOpTime,
                                         boost::optional<Timestamp> commitTimestamp) {
    if (commitOplogEntryOpTime) {
        invariant(commitTimestamp);
        ASSERT_FALSE(opCtx->lockState()->inAWriteUnitOfWork());
        // The 'commitTimestamp' must be cleared before we write the oplog entry.
        ASSERT(opCtx->recoveryUnit()->getCommitTimestamp().isNull());
    } else {
        invariant(!commitTimestamp);
        ASSERT(opCtx->lockState()->inAWriteUnitOfWork());
    }

    OpObserverNoop::onTransactionCommit(opCtx, commitOplogEntryOpTime, commitTimestamp);
    uassert(ErrorCodes::OperationFailed,
            "onTransactionCommit() failed",
            !onTransactionCommitThrowsException);
    transactionCommitted = true;
    onTransactionCommitFn(commitOplogEntryOpTime, commitTimestamp);
}

void OpObserverMock::onTransactionAbort(OperationContext* opCtx,
                                        boost::optional<OplogSlot> abortOplogEntryOpTime) {
    OpObserverNoop::onTransactionAbort(opCtx, abortOplogEntryOpTime);
    uassert(ErrorCodes::OperationFailed,
            "onTransactionAbort() failed",
            !onTransactionAbortThrowsException);
    transactionAborted = true;
    onTransactionAbortFn();
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid,
                                              const CollectionDropType dropType) {
    // If the oplog is not disabled for this namespace, then we need to reserve an op time for the
    // drop.
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
    }
    return {};
}

// When this class is in scope, makes the system behave as if we're in a DBDirectClient
class DirectClientSetter {
public:
    explicit DirectClientSetter(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInDirectClient(opCtx->getClient()->isInDirectClient()) {
        opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientSetter() {
        _opCtx->getClient()->setInDirectClient(_wasInDirectClient);
    }

private:
    const OperationContext* _opCtx;
    const bool _wasInDirectClient;
};

class TxnParticipantTest : public MockReplCoordServerFixture {
protected:
    void setUp() override {
        MockReplCoordServerFixture::setUp();
        MongoDSessionCatalog::onStepUp(opCtx());

        const auto service = opCtx()->getServiceContext();
        OpObserverRegistry* opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        auto mockObserver = stdx::make_unique<OpObserverMock>();
        _opObserver = mockObserver.get();
        opObserverRegistry->addObserver(std::move(mockObserver));

        _sessionId = makeLogicalSessionIdForTest();
        _txnNumber = 20;

        opCtx()->setLogicalSessionId(_sessionId);
        opCtx()->setTxnNumber(_txnNumber);
    }

    void tearDown() override {
        _opObserver = nullptr;

        // Clear all sessions to free up any stashed resources.
        SessionCatalog::get(opCtx()->getServiceContext())->reset_forTest();

        MockReplCoordServerFixture::tearDown();
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
    }

    void runFunctionFromDifferentOpCtx(std::function<void(OperationContext*)> func) {
        // Stash the original client.
        auto originalClient = Client::releaseCurrent();

        // Create a new client (e.g. for migration) and opCtx.
        auto service = opCtx()->getServiceContext();
        auto newClientOwned = service->makeClient("newClient");
        auto newClient = newClientOwned.get();
        Client::setCurrent(std::move(newClientOwned));
        auto newOpCtx = newClient->makeOperationContext();

        ON_BLOCK_EXIT([&] {
            // Restore the original client.
            newOpCtx.reset();
            Client::releaseCurrent();
            Client::setCurrent(std::move(originalClient));
        });

        // Run the function on bahalf of another operation context.
        func(newOpCtx.get());
    }

    void bumpTxnNumberFromDifferentOpCtx(const LogicalSessionId& sessionId, TxnNumber newTxnNum) {
        auto func = [sessionId, newTxnNum](OperationContext* opCtx) {
            auto session = SessionCatalog::get(opCtx)->getOrCreateSession(opCtx, sessionId);
            auto txnParticipant =
                TransactionParticipant::getFromNonCheckedOutSession(session.get());

            // Check that there is a transaction in progress with a lower txnNumber.
            ASSERT(txnParticipant->inMultiDocumentTransaction());
            ASSERT_LT(txnParticipant->getActiveTxnNumber(), newTxnNum);

            // Check that the transaction has some operations, so we can ensure they are cleared.
            ASSERT_GT(txnParticipant->transactionOperationsForTest().size(), 0u);

            // Bump the active transaction number on the txnParticipant. This should clear all state
            // from the previous transaction.
            txnParticipant->beginOrContinue(newTxnNum, boost::none, boost::none);
            ASSERT_EQ(newTxnNum, txnParticipant->getActiveTxnNumber());
            ASSERT_FALSE(txnParticipant->transactionIsAborted());
            ASSERT_EQ(txnParticipant->transactionOperationsForTest().size(), 0u);
        };

        runFunctionFromDifferentOpCtx(func);
    }

    std::unique_ptr<MongoDOperationContextSession> checkOutSession() {
        auto opCtxSession = std::make_unique<MongoDOperationContextSession>(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, true);
        return opCtxSession;
    }

    OpObserverMock* _opObserver = nullptr;
    LogicalSessionId _sessionId;
    TxnNumber _txnNumber;
};

// Test that transaction lock acquisition times out in `maxTransactionLockRequestTimeoutMillis`
// milliseconds.
TEST_F(TxnParticipantTest, TransactionThrowsLockTimeoutIfLockIsUnavailable) {
    const std::string dbName = "TestDB";

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    { Lock::DBLock dbXLock(opCtx(), dbName, MODE_X); }
    txnParticipant->stashTransactionResources(opCtx());
    auto clientWithDatabaseXLock = Client::releaseCurrent();


    /**
     * Make a new Session, Client, OperationContext and transaction and then attempt to take the
     * same database exclusive lock, which should conflict because the other transaction already
     * took it.
     */

    auto service = opCtx()->getServiceContext();
    auto newClientOwned = service->makeClient("newTransactionClient");
    auto newClient = newClientOwned.get();
    Client::setCurrent(std::move(newClientOwned));

    const auto newSessionId = makeLogicalSessionIdForTest();
    const TxnNumber newTxnNum = 10;
    {
        // Limit the scope of the new opCtx to make sure that it gets destroyed before
        // new client is destroyed.
        auto newOpCtx = newClient->makeOperationContext();
        newOpCtx.get()->setLogicalSessionId(newSessionId);
        newOpCtx.get()->setTxnNumber(newTxnNum);

        MongoDOperationContextSession newOpCtxSession(newOpCtx.get());
        auto newTxnParticipant = TransactionParticipant::get(newOpCtx.get());
        newTxnParticipant->beginOrContinue(newTxnNum, false, true);
        newTxnParticipant->unstashTransactionResources(newOpCtx.get(), "insert");

        Date_t t1 = Date_t::now();
        ASSERT_THROWS_CODE(Lock::DBLock(newOpCtx.get(), dbName, MODE_X),
                           AssertionException,
                           ErrorCodes::LockTimeout);
        Date_t t2 = Date_t::now();
        int defaultMaxTransactionLockRequestTimeoutMillis = 5;
        ASSERT_GTE(t2 - t1, Milliseconds(defaultMaxTransactionLockRequestTimeoutMillis));

        // A non-conflicting lock acquisition should work just fine.
        { Lock::DBLock tempLock(newOpCtx.get(), "NewTestDB", MODE_X); }
    }
    // Restore the original client so that teardown works.
    Client::releaseCurrent();
    Client::setCurrent(std::move(clientWithDatabaseXLock));
}

TEST_F(TxnParticipantTest, StashAndUnstashResources) {
    Locker* originalLocker = opCtx()->lockState();
    RecoveryUnit* originalRecoveryUnit = opCtx()->recoveryUnit();
    ASSERT(originalLocker);
    ASSERT(originalRecoveryUnit);

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());
    ASSERT(opCtx()->lockState()->isLocked());

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_NOT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_NOT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    txnParticipant->unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());

    // Commit the transaction. This allows us to release locks.
    txnParticipant->commitUnpreparedTransaction(opCtx());
}

TEST_F(TxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    // Must specify startTransaction=true and autocommit=false to start a transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant->inMultiDocumentTransaction());

    // Cannot try to start a transaction that already started.
    ASSERT_THROWS_CODE(txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, true),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, AutocommitRequiredOnEveryTxnOp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // We must have stashed transaction resources to do a second operation on the transaction.
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    auto txnNum = *opCtx()->getTxnNumber();
    // Omitting 'autocommit' after the first statement of a transaction should throw an error.
    ASSERT_THROWS_CODE(txnParticipant->beginOrContinue(txnNum, boost::none, boost::none),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    // Including autocommit=false should succeed.
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, boost::none);
}

DEATH_TEST_F(TxnParticipantTest, AutocommitCannotBeTrue, "invariant") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'autocommit=true' is not allowed and should crash.
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), true, boost::none);
}

DEATH_TEST_F(TxnParticipantTest, StartTransactionCannotBeFalse, "invariant") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'startTransaction=false' is not allowed and should crash.
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, false);
}

TEST_F(TxnParticipantTest, SameTransactionPreservesStoredStatements) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // We must have stashed transaction resources to re-open the transaction.
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant->transactionOperationsForTest()[0].toBSON());
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // Check the transaction operations before re-opening the transaction.
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant->transactionOperationsForTest()[0].toBSON());

    // Re-opening the same transaction should have no effect.
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, boost::none);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant->transactionOperationsForTest()[0].toBSON());
}

TEST_F(TxnParticipantTest, AbortClearsStoredStatements) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant->transactionOperationsForTest()[0].toBSON());

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    txnParticipant->abortArbitraryTransaction();
    ASSERT_TRUE(txnParticipant->transactionOperationsForTest().empty());
    ASSERT_TRUE(txnParticipant->transactionIsAborted());
}

// This test makes sure the commit machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyTransactionCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->commitUnpreparedTransaction(opCtx());
    txnParticipant->stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
}

TEST_F(TxnParticipantTest, CommitTransactionSetsCommitTimestampOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](boost::optional<OplogSlot> commitOplogEntryOpTime,
                                             boost::optional<Timestamp> commitTimestamp) {
        originalFn(commitOplogEntryOpTime, commitTimestamp);
        ASSERT(commitOplogEntryOpTime);
        ASSERT(commitTimestamp);

        ASSERT_GT(commitTimestamp, prepareTimestamp);
    };

    txnParticipant->commitPreparedTransaction(opCtx(), commitTS);

    // The recovery unit is reset on commit.
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithCommitTimestampFailsOnUnpreparedTransaction) {
    const auto commitTimestamp = Timestamp(6, 6);

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionDoesNotSetCommitTimestampOnUnpreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](boost::optional<OplogSlot> commitOplogEntryOpTime,
                                             boost::optional<Timestamp> commitTimestamp) {
        originalFn(commitOplogEntryOpTime, commitTimestamp);
        ASSERT_FALSE(commitOplogEntryOpTime);
        ASSERT_FALSE(commitTimestamp);
        ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
    };

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->commitUnpreparedTransaction(opCtx());

    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithoutCommitTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionWithNullCommitTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(opCtx(), Timestamp()),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest,
       CommitTransactionWithCommitTimestampLessThanPrepareTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(
                           opCtx(), Timestamp(prepareTimestamp.getSecs() - 1, 1)),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest,
       CommitTransactionWithCommitTimestampEqualToPrepareTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(opCtx(), prepareTimestamp),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    txnParticipant->abortArbitraryTransaction();
    ASSERT_TRUE(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ConcurrencyOfUnstashAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // An unstash after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "find"),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, ConcurrencyOfUnstashAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    txnParticipant->stashTransactionResources(opCtx());

    // A migration may bump the active transaction number without checking out the
    // txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // An unstash after a migration that bumps the active transaction number should uassert.
    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "insert"),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, ConcurrencyOfStashAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "find");

    // The transaction may be aborted without checking out the txnParticipant->
    txnParticipant->abortArbitraryTransaction();

    // A stash after an abort should be a noop.
    txnParticipant->stashTransactionResources(opCtx());
}

TEST_F(TxnParticipantTest, ConcurrencyOfStashAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the
    // txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // A stash after a migration that bumps the active transaction number should uassert.
    ASSERT_THROWS_CODE(txnParticipant->stashTransactionResources(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, ConcurrencyOfAddTransactionOperationAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // An addTransactionOperation() after an abort should uassert.
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    ASSERT_THROWS_CODE(txnParticipant->addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, ConcurrencyOfAddTransactionOperationAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "find");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the
    // txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // An addTransactionOperation() after a migration that bumps the active transaction number
    // should uassert.
    ASSERT_THROWS_CODE(txnParticipant->addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, ConcurrencyOfEndTransactionAndRetrieveOperationsAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // An endTransactionAndRetrieveOperations() after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->endTransactionAndRetrieveOperations(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, ConcurrencyOfEndTransactionAndRetrieveOperationsAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // An endTransactionAndRetrieveOperations() after a migration that bumps the active transaction
    // number should uassert.
    ASSERT_THROWS_CODE(txnParticipant->endTransactionAndRetrieveOperations(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, ConcurrencyOfCommitUnpreparedTransactionAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // A commitUnpreparedTransaction() after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, ConcurrencyOfCommitPreparedTransactionAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    auto prepareTS = txnParticipant->prepareTransaction(opCtx(), {});
    auto commitTS = Timestamp(prepareTS.getSecs(), prepareTS.getInc() + 1);

    txnParticipant->abortArbitraryTransaction();

    // A commitPreparedTransaction() after an abort should succeed since the abort should fail.
    txnParticipant->commitPreparedTransaction(opCtx(), commitTS);

    ASSERT(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(txnParticipant->transactionIsCommitted());
}

TEST_F(TxnParticipantTest, ConcurrencyOfActiveUnpreparedAbortAndArbitraryAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT(txnParticipant->inMultiDocumentTransaction());

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // The operation throws for some reason and aborts implicitly.
    // Abort active transaction after it's been aborted by KillSession is a no-op.
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT(opCtx()->getWriteUnitOfWork() == nullptr);
}

TEST_F(TxnParticipantTest, ConcurrencyOfActiveUnpreparedAbortAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    ASSERT(txnParticipant->inMultiDocumentTransaction());

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    ASSERT_THROWS_CODE(txnParticipant->abortActiveTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);

    // The abort fails so the OperationContext state is not cleaned up until the operation is
    // complete. The session has already moved on to a new transaction so the transaction will not
    // remain active beyond this operation.
    ASSERT_FALSE(opCtx()->getWriteUnitOfWork() == nullptr);
}

TEST_F(TxnParticipantTest, ConcurrencyOfActivePreparedAbortAndArbitraryAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT(txnParticipant->inMultiDocumentTransaction());
    txnParticipant->prepareTransaction(opCtx(), {});

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // The operation throws for some reason and aborts implicitly.
    // Abort active transaction after it's been aborted by KillSession is a no-op.
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT(opCtx()->getWriteUnitOfWork() == nullptr);
}

TEST_F(TxnParticipantTest, ConcurrencyOfPrepareTransactionAndAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();
    ASSERT(txnParticipant->transactionIsAborted());

    // A prepareTransaction() after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, KillSessionsDuringPrepareDoesNotAbortTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    auto ruPrepareTimestamp = Timestamp();
    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();

        ruPrepareTimestamp = opCtx()->recoveryUnit()->getPrepareTimestamp();
        ASSERT_FALSE(ruPrepareTimestamp.isNull());

        // The transaction may be aborted without checking out the txnParticipant.
        txnParticipant->abortArbitraryTransaction();
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    };

    // Check that prepareTimestamp gets set.
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);
    // Check that the oldest prepareTimestamp is the one we just set.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), prepareTimestamp);
    ASSERT(_opObserver->transactionPrepared);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
}

DEATH_TEST_F(TxnParticipantTest, AbortDuringPrepareIsFatal, "Invariant") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();

        // The transaction may be aborted without checking out the txnParticipant.
        txnParticipant->abortActiveTransaction(opCtx());
        ASSERT(txnParticipant->transactionIsAborted());
    };

    txnParticipant->prepareTransaction(opCtx(), {});
}

TEST_F(TxnParticipantTest, ThrowDuringOnTransactionPrepareAbortsTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    _opObserver->onTransactionPrepareThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, KillSessionsDuringPreparedCommitDoesNotAbortTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](boost::optional<OplogSlot> commitOplogEntryOpTime,
                                             boost::optional<Timestamp> commitTimestamp) {
        originalFn(commitOplogEntryOpTime, commitTimestamp);
        ASSERT(commitOplogEntryOpTime);
        ASSERT(commitTimestamp);

        ASSERT_GT(*commitTimestamp, prepareTimestamp);

        // The transaction may be aborted without checking out the txnParticipant.
        txnParticipant->abortArbitraryTransaction();
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    };

    txnParticipant->commitPreparedTransaction(opCtx(), commitTS);

    // The recovery unit is reset on commit.
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    ASSERT(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(txnParticipant->transactionIsCommitted());
}

TEST_F(TxnParticipantTest, ArbitraryAbortDuringPreparedCommitDoesNotAbortTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](boost::optional<OplogSlot> commitOplogEntryOpTime,
                                             boost::optional<Timestamp> commitTimestamp) {
        originalFn(commitOplogEntryOpTime, commitTimestamp);
        ASSERT(commitOplogEntryOpTime);
        ASSERT(commitTimestamp);

        ASSERT_GT(*commitTimestamp, prepareTimestamp);

        // The transaction may be aborted without checking out the txnParticipant.
        auto func = [&](OperationContext* opCtx) { txnParticipant->abortArbitraryTransaction(); };
        runFunctionFromDifferentOpCtx(func);
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    };

    txnParticipant->commitPreparedTransaction(opCtx(), commitTS);

    // The recovery unit is reset on commit.
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    ASSERT(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(txnParticipant->transactionIsCommitted());
}

DEATH_TEST_F(TxnParticipantTest,
             ThrowDuringPreparedOnTransactionCommitIsFatal,
             "Caught exception during commit") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    _opObserver->onTransactionCommitThrowsException = true;
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    txnParticipant->commitPreparedTransaction(opCtx(), commitTS);
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedCommitLetsTheAbortAtEntryPointToCleanUp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    _opObserver->onTransactionCommitThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT_FALSE(txnParticipant->transactionIsCommitted());

    // Simulate the abort at entry point.
    txnParticipant->abortActiveUnpreparedOrStashPreparedTransaction(opCtx());
    ASSERT_TRUE(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ConcurrencyOfCommitUnpreparedTransactionAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // A commitUnpreparedTransaction() after a migration that bumps the active transaction number
    // should uassert.
    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, ConcurrencyOfPrepareTransactionAndMigration) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // A prepareTransaction() after a migration that bumps the active transaction number should
    // uassert.
    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
    ASSERT_FALSE(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, ContinuingATransactionWithNoResourcesAborts) {
    // Check out a session, start the transaction and check it in.
    checkOutSession();

    // Check out the session again for a new operation.
    MongoDOperationContextSession sessionCheckout(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_THROWS_CODE(
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, boost::none),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, KillSessionsDoesNotAbortPreparedTransactions) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    auto ruPrepareTimestamp = Timestamp();
    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();
        ruPrepareTimestamp = opCtx()->recoveryUnit()->getPrepareTimestamp();
        ASSERT_FALSE(ruPrepareTimestamp.isNull());
    };

    // Check that prepareTimestamp gets set.
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);
    // Check that the oldest prepareTimestamp is the one we just set.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), prepareTimestamp);
    txnParticipant->stashTransactionResources(opCtx());

    txnParticipant->abortArbitraryTransaction();
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, TransactionTimeoutDoesNotAbortPreparedTransactions) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    auto ruPrepareTimestamp = Timestamp();
    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();
        ruPrepareTimestamp = opCtx()->recoveryUnit()->getPrepareTimestamp();
        ASSERT_FALSE(ruPrepareTimestamp.isNull());
    };

    // Check that prepareTimestamp gets set.
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);
    // Check that the oldest prepareTimestamp is the one we just set.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), prepareTimestamp);
    txnParticipant->stashTransactionResources(opCtx());

    ASSERT(!txnParticipant->expired());
    txnParticipant->abortArbitraryTransaction();
    ASSERT(!txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotStartNewTransactionWhilePreparedTransactionInProgress) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    auto ruPrepareTimestamp = Timestamp();
    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();

        ruPrepareTimestamp = opCtx()->recoveryUnit()->getPrepareTimestamp();
        ASSERT_FALSE(ruPrepareTimestamp.isNull());
    };

    // Check that prepareTimestamp gets set.
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);

    // Check that the oldest prepareTimestamp is the one we just set.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), prepareTimestamp);

    txnParticipant->stashTransactionResources(opCtx());

    {
        // Try to start a new transaction while there is already a prepared transaction on the
        // session. This should fail with a PreparedTransactionInProgress error.
        auto func = [&](OperationContext* newOpCtx) {
            auto session = SessionCatalog::get(newOpCtx)->getOrCreateSession(
                newOpCtx, *opCtx()->getLogicalSessionId());
            auto txnParticipant =
                TransactionParticipant::getFromNonCheckedOutSession(session.get());

            ASSERT_THROWS_CODE(
                txnParticipant->beginOrContinue(*opCtx()->getTxnNumber() + 1, false, true),
                AssertionException,
                ErrorCodes::PreparedTransactionInProgress);
        };

        runFunctionFromDifferentOpCtx(func);
    }

    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotInsertInPreparedTransaction) {
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    txnParticipant->prepareTransaction(opCtx(), {});

    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "insert"),
                       AssertionException,
                       ErrorCodes::PreparedTransactionInProgress);

    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, MigrationThrowsOnPreparedTransaction) {
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    txnParticipant->prepareTransaction(opCtx(), {});

    // A migration may bump the active transaction number without checking out the session.
    auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    ASSERT_THROWS_CODE(
        bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum),
        AssertionException,
        ErrorCodes::PreparedTransactionInProgress);
    // The transaction is not affected.
    ASSERT_TRUE(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, ImplictAbortDoesNotAbortPreparedTransaction) {
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    txnParticipant->prepareTransaction(opCtx(), {});

    // The next command throws an exception and wants to abort the transaction.
    // This is a no-op.
    txnParticipant->abortActiveUnpreparedOrStashPreparedTransaction(opCtx());
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT_TRUE(_opObserver->transactionPrepared);
}

DEATH_TEST_F(TxnParticipantTest, AbortIsIllegalDuringCommittingPreparedTransaction, "invariant") {
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    // Check that the oldest prepareTimestamp is the one we just set.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), prepareTimestamp);

    auto sessionId = *opCtx()->getLogicalSessionId();
    auto txnNum = *opCtx()->getTxnNumber();
    _opObserver->onTransactionCommitFn = [&](boost::optional<OplogSlot> commitOplogEntryOpTime,
                                             boost::optional<Timestamp> commitTimestamp) {
        // This should never happen.
        auto func = [&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(sessionId);
            opCtx->setTxnNumber(txnNum);
            // Hit an invariant. This should never happen.
            txnParticipant->abortActiveTransaction(opCtx);
        };
        runFunctionFromDifferentOpCtx(func);
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    };

    txnParticipant->commitPreparedTransaction(opCtx(), commitTS);
    // Check that we removed the prepareTimestamp from the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime(), boost::none);
}

TEST_F(TxnParticipantTest, CannotContinueNonExistentTransaction) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_THROWS_CODE(
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, boost::none),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

// Tests that a transaction aborts if it becomes too large before trying to commit it.
TEST_F(TxnParticipantTest, TransactionTooLargeWhileBuilding) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // Two 6MB operations should succeed; three 6MB operations should fail.
    constexpr size_t kBigDataSize = 6 * 1024 * 1024;
    std::unique_ptr<uint8_t[]> bigData(new uint8_t[kBigDataSize]());
    auto operation = repl::OplogEntry::makeInsertOperation(
        kNss,
        kUUID,
        BSON("_id" << 0 << "data" << BSONBinData(bigData.get(), kBigDataSize, BinDataGeneral)));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    txnParticipant->addTransactionOperation(opCtx(), operation);
    ASSERT_THROWS_CODE(txnParticipant->addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
}

TEST_F(TxnParticipantTest, StashInNestedSessionIsANoop) {
    auto outerScopedSession = checkOutSession();
    Locker* originalLocker = opCtx()->lockState();
    RecoveryUnit* originalRecoveryUnit = opCtx()->recoveryUnit();
    ASSERT(originalLocker);
    ASSERT(originalRecoveryUnit);

    // Set the readConcern on the OperationContext.
    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash, which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());

    {
        // Make it look like we're in a DBDirectClient running a nested operation.
        DirectClientSetter inDirectClient(opCtx());
        MongoDOperationContextSession innerScopedSession(opCtx());

        txnParticipant->stashTransactionResources(opCtx());

        // The stash was a noop, so the locker, RecoveryUnit, and WriteUnitOfWork on the
        // OperationContext are unaffected.
        ASSERT_EQUALS(originalLocker, opCtx()->lockState());
        ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
        ASSERT(opCtx()->getWriteUnitOfWork());
    }
}

TEST_F(TxnParticipantTest, UnstashInNestedSessionIsANoop) {
    auto outerScopedSession = checkOutSession();

    Locker* originalLocker = opCtx()->lockState();
    RecoveryUnit* originalRecoveryUnit = opCtx()->recoveryUnit();
    ASSERT(originalLocker);
    ASSERT(originalRecoveryUnit);

    // Set the readConcern on the OperationContext.
    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    {
        // Make it look like we're in a DBDirectClient running a nested operation.
        DirectClientSetter inDirectClient(opCtx());
        MongoDOperationContextSession innerScopedSession(opCtx());

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "find");

        // The unstash was a noop, so the OperationContext did not get a WriteUnitOfWork.
        ASSERT_EQUALS(originalLocker, opCtx()->lockState());
        ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
        ASSERT_FALSE(opCtx()->getWriteUnitOfWork());
    }
}

/**
 * Test fixture for testing behavior that depends on a server's cluster role.
 *
 * Each test case relies on the txnNumber on the operation context, which cannot be changed, so
 * define tests for behavior shared by config and shard servers as methods here and call them in the
 * fixtures for config and shard servers defined below.
 */
class ShardedClusterParticipantTest : public TxnParticipantTest {
protected:
    void canSpecifyStartTransactionOnInProgressTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant->inMultiDocumentTransaction());

        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction);
        ASSERT(txnParticipant->inMultiDocumentTransaction());
    }

    void canSpecifyStartTransactionOnAbortedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant->inMultiDocumentTransaction());

        txnParticipant->abortActiveTransaction(opCtx());
        ASSERT(txnParticipant->transactionIsAborted());

        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction);
        ASSERT(txnParticipant->inMultiDocumentTransaction());
    }

    void cannotSpecifyStartTransactionOnCommittedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant->inMultiDocumentTransaction());

        txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
        txnParticipant->commitUnpreparedTransaction(opCtx());

        ASSERT_THROWS_CODE(
            txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction),
            AssertionException,
            50911);
    }

    void cannotSpecifyStartTransactionOnPreparedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant->inMultiDocumentTransaction());

        txnParticipant->unstashTransactionResources(opCtx(), "insert");
        auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
        txnParticipant->addTransactionOperation(opCtx(), operation);
        txnParticipant->prepareTransaction(opCtx(), {});

        ASSERT_THROWS_CODE(
            txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction),
            AssertionException,
            50911);
    }

    void cannotSpecifyStartTransactionOnStartedRetryableWrite() {
        MongoDOperationContextSession opCtxSession(opCtx());

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), boost::none, boost::none);
        ASSERT_FALSE(txnParticipant->inMultiDocumentTransaction());

        auto autocommit = false;
        auto startTransaction = true;
        ASSERT_THROWS_CODE(
            txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction),
            AssertionException,
            50911);
    }

    // TODO SERVER-36639: Add tests that the active transaction number cannot be reused if the
    // transaction is in the abort after prepare state (or any state indicating the participant
    // has been involved in a two phase commit).
};

/**
 * Test fixture for a transaction participant running on a shard server.
 */
class ShardTxnParticipantTest : public ShardedClusterParticipantTest {
protected:
    void setUp() final {
        TxnParticipantTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    }

    void tearDown() final {
        serverGlobalParams.clusterRole = ClusterRole::None;
        TxnParticipantTest::tearDown();
    }
};

TEST_F(ShardTxnParticipantTest, CanSpecifyStartTransactionOnInProgressTxn) {
    canSpecifyStartTransactionOnInProgressTxn();
}

TEST_F(ShardTxnParticipantTest, CanSpecifyStartTransactionOnAbortedTxn) {
    canSpecifyStartTransactionOnAbortedTxn();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnCommittedTxn) {
    cannotSpecifyStartTransactionOnCommittedTxn();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnPreparedTxn) {
    cannotSpecifyStartTransactionOnPreparedTxn();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnStartedRetryableWrite) {
    cannotSpecifyStartTransactionOnStartedRetryableWrite();
}

/**
 * Test fixture for a transaction participant running on a config server.
 */
class ConfigTxnParticipantTest : public ShardedClusterParticipantTest {
protected:
    void setUp() final {
        TxnParticipantTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    }

    void tearDown() final {
        serverGlobalParams.clusterRole = ClusterRole::None;
        TxnParticipantTest::tearDown();
    }
};

TEST_F(ConfigTxnParticipantTest, CanSpecifyStartTransactionOnInProgressTxn) {
    canSpecifyStartTransactionOnInProgressTxn();
}

TEST_F(ConfigTxnParticipantTest, CanSpecifyStartTransactionOnAbortedTxn) {
    canSpecifyStartTransactionOnAbortedTxn();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnCommittedTxn) {
    cannotSpecifyStartTransactionOnCommittedTxn();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnPreparedTxn) {
    cannotSpecifyStartTransactionOnPreparedTxn();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnStartedRetryableWrite) {
    cannotSpecifyStartTransactionOnStartedRetryableWrite();
}

TEST_F(TxnParticipantTest, KillSessionsDuringUnpreparedAbortSucceeds) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    auto originalFn = _opObserver->onTransactionAbortFn;
    _opObserver->onTransactionAbortFn = [&] {
        originalFn();

        // The transaction may be aborted without checking out the txnParticipant.
        txnParticipant->abortArbitraryTransaction();
        ASSERT(txnParticipant->transactionIsAborted());
    };

    txnParticipant->abortActiveTransaction(opCtx());

    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ActiveAbortIsLegalDuringUnpreparedAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    auto sessionId = *opCtx()->getLogicalSessionId();
    auto txnNumber = *opCtx()->getTxnNumber();
    auto originalFn = _opObserver->onTransactionAbortFn;
    _opObserver->onTransactionAbortFn = [&] {
        originalFn();

        auto func = [&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(sessionId);
            opCtx->setTxnNumber(txnNumber);

            // Prevent recursion.
            _opObserver->onTransactionAbortFn = originalFn;
            txnParticipant->abortActiveTransaction(opCtx);
            ASSERT(txnParticipant->transactionIsAborted());
        };
        runFunctionFromDifferentOpCtx(func);
    };

    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedOnTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    _opObserver->onTransactionAbortThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant->abortActiveTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::OperationFailed);
}

TEST_F(TxnParticipantTest, KillSessionsDuringPreparedAbortFails) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    auto originalFn = _opObserver->onTransactionAbortFn;
    _opObserver->onTransactionAbortFn = [&] {
        originalFn();

        // KillSessions may attempt to abort without checking out the txnParticipant.
        txnParticipant->abortArbitraryTransaction();
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
        ASSERT(txnParticipant->transactionIsPrepared());
    };

    txnParticipant->abortActiveTransaction(opCtx());

    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ActiveAbortSucceedsDuringPreparedAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    auto sessionId = *opCtx()->getLogicalSessionId();
    auto txnNumber = *opCtx()->getTxnNumber();
    auto originalFn = _opObserver->onTransactionAbortFn;
    _opObserver->onTransactionAbortFn = [&] {
        originalFn();

        auto func = [&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(sessionId);
            opCtx->setTxnNumber(txnNumber);

            // Prevent recursion.
            _opObserver->onTransactionAbortFn = originalFn;
            txnParticipant->abortActiveTransaction(opCtx);
            ASSERT(txnParticipant->transactionIsAborted());
        };
        runFunctionFromDifferentOpCtx(func);
    };

    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ThrowDuringPreparedOnTransactionAbortIsFatal) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    _opObserver->onTransactionAbortThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant->abortActiveTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::OperationFailed);
}

/**
 * Test fixture for transactions metrics.
 */
class TransactionsMetricsTest : public TxnParticipantTest {
protected:
    using TickSourceMicrosecondMock = TickSourceMock<Microseconds>;

    /**
     * Set up and return a mock clock source.
     */
    ClockSourceMock* initMockPreciseClockSource() {
        getServiceContext()->setPreciseClockSource(stdx::make_unique<ClockSourceMock>());
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    }

    /**
     * Set up and return a mock tick source.
     */
    TickSourceMicrosecondMock* initMockTickSource() {
        getServiceContext()->setTickSource(stdx::make_unique<TickSourceMicrosecondMock>());
        auto tickSource =
            dynamic_cast<TickSourceMicrosecondMock*>(getServiceContext()->getTickSource());
        // Ensure that the tick source is not initialized to zero.
        tickSource->reset(1);
        return tickSource;
    }
};

TEST_F(TransactionsMetricsTest, IncrementTotalStartedUponStartTransaction) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getTotalStarted();

    auto sessionCheckout = checkOutSession();

    // Tests that the total transactions started counter is incremented by 1 when a new transaction
    // is started.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalStarted(),
              beforeTransactionStart + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    unsigned long long beforePrepareCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPrepared();
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforePrepareCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalCommittedOnCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    unsigned long long beforeCommitCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalCommitted();

    txnParticipant->commitUnpreparedTransaction(opCtx());

    // Assert that the committed counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalCommitted(), beforeCommitCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalPreparedThenCommitted) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    unsigned long long beforePreparedThenCommittedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();

    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);

    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforePreparedThenCommittedCount + 1U);
}


TEST_F(TransactionsMetricsTest, IncrementTotalAbortedUponAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    unsigned long long beforeAbortCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    txnParticipant->abortArbitraryTransaction();

    // Assert that the aborted counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalPreparedThenAborted) {
    unsigned long long beforePreparedThenAbortedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforePreparedThenAbortedCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementCurrentPreparedWithCommit) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(),
              beforeCurrentPrepared + 1U);
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);
    ASSERT(txnParticipant->transactionIsCommitted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
}

TEST_F(TransactionsMetricsTest, IncrementCurrentPreparedWithAbort) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(),
              beforeCurrentPrepared + 1U);
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithAbort) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that aborting a transaction decrements the open transactions counter by 1.
    txnParticipant->abortArbitraryTransaction();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithCommit) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // Tests that committing a transaction decrements the open transactions counter by 1.
    txnParticipant->commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(TransactionsMetricsTest, TrackTotalActiveAndInactiveTransactionsWithCommit) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    auto sessionCheckout = checkOutSession();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that the second unstash increments the active counter and decrements the inactive
    // counter.
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that committing a transaction decrements the active counter only.
    txnParticipant->commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(TransactionsMetricsTest, TrackTotalActiveAndInactiveTransactionsWithStashedAbort) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    auto sessionCheckout = checkOutSession();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that aborting a stashed transaction decrements the inactive counter only.
    txnParticipant->abortArbitraryTransaction();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(TransactionsMetricsTest, TrackTotalActiveAndInactiveTransactionsWithUnstashedAbort) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    auto sessionCheckout = checkOutSession();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that aborting a stashed transaction decrements the active counter only.
    txnParticipant->abortArbitraryTransaction();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(TransactionsMetricsTest, TrackCurrentActiveAndInactivePreparedTransactionsOnCommit) {
    unsigned long long beforeActivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    auto sessionCheckout = checkOutSession();
    unsigned long long beforePrepareCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPrepared();
    unsigned long long beforePreparedThenCommittedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();

    // Tests that unstashing a transaction puts it into an active state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforePrepareCount + 1U);

    // Tests that the first stash decrements the active counter and increments the inactive counter.
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter + 1U);

    // Tests that unstashing increments the active counter and decrements the inactive counter.
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);

    // Tests that committing decrements the active counter only.
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforePreparedThenCommittedCount + 1U);
}

TEST_F(TransactionsMetricsTest,
       TrackCurrentActiveAndInactivePreparedTransactionsWithUnstashedAbort) {
    unsigned long long beforeActivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Tests that unstashing a transaction increments the active counter only.
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);

    // Tests that stashing a prepared transaction decrements the active counter and increments the
    // inactive counter.
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter + 1U);

    // Tests that aborting a stashed prepared transaction decrements the inactive counter only.
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
}

TEST_F(TransactionsMetricsTest, TransactionErrorsBeforeUnstash) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    unsigned long long beforeAbortedCounter =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    // The first transaction statement checks out the session and begins the transaction but returns
    // before unstashTransactionResources().
    auto sessionCheckout = checkOutSession();

    // The transaction is now inactive.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortedCounter);

    // The second transaction statement continues the transaction. Since there are no stashed
    // transaction resources, it is not safe to continue the transaction, so the transaction is
    // aborted.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    const bool autocommit = false;
    const boost::optional<bool> startTransaction = boost::none;
    ASSERT_THROWS_CODE(
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction),
        AssertionException,
        ErrorCodes::NoSuchTransaction);

    // The transaction is now aborted.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(),
              beforeAbortedCounter + 1U);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Advance the clock.
    tickSource->advance(Microseconds(100));

    txnParticipant->commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTranasactionStatsPreparedDurationShouldBeSetUponCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Advance the clock.
    tickSource->advance(Microseconds(10));

    // Prepare the transaction and extend the duration in the prepared state.
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    tickSource->advance(Microseconds(100));

    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // Advance the clock.
    tickSource->advance(Microseconds(100));

    txnParticipant->abortArbitraryTransaction();
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsPreparedDurationShouldBeSetUponAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    // Advance the clock.
    tickSource->advance(Microseconds(10));

    // Prepare the transaction and extend the duration in the prepared state.
    txnParticipant->prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    tickSource->advance(Microseconds(100));

    // The transaction's duration should have increased.
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Commit the transaction and check duration.
    txnParticipant->commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(200));

    // The transaction committed, so the duration shouldn't have increased even if more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(200));
}

TEST_F(TransactionsMetricsTest,
       SingleTransactionStatsPreparedDurationShouldKeepIncreasingUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Prepare the transaction and extend the duration in the prepared state.
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    tickSource->advance(Microseconds(100));

    // The prepared transaction's duration should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Commit the prepared transaction and check the prepared duration.
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The prepared transaction committed, so the prepared duration shouldn't have increased even if
    // more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    tickSource->advance(Microseconds(100));

    // The transaction's duration should have increased.
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Abort the transaction and check duration.
    txnParticipant->abortArbitraryTransaction();
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(200));

    // The transaction aborted, so the duration shouldn't have increased even if more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(
        txnParticipant->getSingleTransactionStats().getDuration(tickSource, tickSource->getTicks()),
        Microseconds(200));
}

TEST_F(TransactionsMetricsTest,
       SingleTransactionStatsPreparedDurationShouldKeepIncreasingUntilAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Prepare the transaction and extend the duration in the prepared state.
    txnParticipant->prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    // The prepared transaction's duration should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Abort the prepared transaction and check the prepared duration.
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The prepared transaction aborted, so the prepared duration shouldn't have increased even if
    // more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndStash) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    // Time active should have increased only during active period.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    txnParticipant->stashTransactionResources(opCtx());

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    // Start a new transaction.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    txnParticipant->beginOrContinue(higherTxnNum, false, true);

    // Time active should be zero for a new transaction.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    txnParticipant->abortArbitraryTransaction();

    // Time active should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // The transaction is not active after abort, so time active should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetUponAbortOnly) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    txnParticipant->abortArbitraryTransaction();

    // Time active should still be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilStash) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));

    // Time active should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is no longer active, so time active should have stopped increasing.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    tickSource->advance(Microseconds(100));

    // Time active should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    txnParticipant->commitUnpreparedTransaction(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is no longer active, so time active should have stopped increasing.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetIfUnstashHasBadReadConcernArgs) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize bad read concern args (!readConcernArgs.isEmpty()).
    repl::ReadConcernArgs readConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Transaction resources do not exist yet.
    txnParticipant->unstashTransactionResources(opCtx(), "find");

    tickSource->advance(Microseconds(100));

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // Time active should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    // Transaction resources already exist here and should throw an exception due to bad read
    // concern arguments.
    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "find"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    tickSource->advance(Microseconds(100));

    // Time active should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponStash) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysExamined = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 5;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.docsExamined = 2;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 0;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nMatched = 3;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 4;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nmoved = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.nmoved = 2;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysInserted = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysDeleted = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.prepareReadConflicts =
        5;
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts = 4;

    auto additiveMetricsToCompare =
        txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    ASSERT(txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysExamined = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 2;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.docsExamined = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 2;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nMatched = 4;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nModified = 5;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 1;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.ndeleted = 4;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 0;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysInserted = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.prepareReadConflicts =
        0;
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts = 0;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.writeConflicts = 6;
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts = 3;

    auto additiveMetricsToCompare =
        txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->commitUnpreparedTransaction(opCtx());

    ASSERT(txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysExamined = 2;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 4;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.docsExamined = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 3;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nMatched = 2;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nModified = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 5;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.nmoved = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.nmoved = 2;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysInserted = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.keysDeleted = 6;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.writeConflicts = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts = 3;

    auto additiveMetricsToCompare =
        txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->abortActiveTransaction(opCtx());

    ASSERT(txnParticipant->getSingleTransactionStats().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndStash) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should have increased.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    // Time inactive should have increased again.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    tickSource->advance(Microseconds(100));

    // The transaction is currently active, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is inactive again, so time inactive should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{300});
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    tickSource->advance(Microseconds(100));

    // Time inactive should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    txnParticipant->abortArbitraryTransaction();

    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // The transaction has aborted, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldIncreaseUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    tickSource->advance(Microseconds(100));

    // Time inactive should have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->commitUnpreparedTransaction(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction has committed, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}


TEST_F(TransactionsMetricsTest, ReportStashedResources) {
    auto tickSource = initMockTickSource();
    auto clockSource = initMockPreciseClockSource();
    auto startTime = Date_t::now();
    clockSource->reset(startTime);

    const bool autocommit = false;

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    auto sessionCheckout = checkOutSession();

    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               "appName",
                                               &builder));
    auto obj = builder.obj();
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "find");
    ASSERT(opCtx()->getWriteUnitOfWork());
    ASSERT(opCtx()->lockState()->isLocked());

    // Prepare the transaction and extend the duration in the prepared state.
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    const long preparedDuration = 10;
    tickSource->advance(Microseconds(preparedDuration));

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Verify that the Session's report of its own stashed state aligns with our expectations.
    auto stashedState = txnParticipant->reportStashedState();
    auto transactionDocument = stashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(stashedState.getField("host").valueStringData().toString(),
              getHostNameCachedAndPort());
    ASSERT_EQ(stashedState.getField("desc").valueStringData().toString(), "inactive transaction");
    ASSERT_BSONOBJ_EQ(stashedState.getField("lsid").Obj(), _sessionId.toBSON());
    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), *opCtx()->getTxnNumber());
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        startTime + stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()});
    ASSERT_EQ(transactionDocument.getField("timePreparedMicros").numberLong(), preparedDuration);

    ASSERT_EQ(stashedState.getField("client").valueStringData().toString(), "");
    ASSERT_EQ(stashedState.getField("connectionId").numberLong(), 0);
    ASSERT_EQ(stashedState.getField("appName").valueStringData().toString(), "appName");
    ASSERT_BSONOBJ_EQ(stashedState.getField("clientMetadata").Obj(), obj.getField("client").Obj());
    ASSERT_EQ(stashedState.getField("waitingForLock").boolean(), false);
    ASSERT_EQ(stashedState.getField("active").boolean(), false);

    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that the
    // values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT(opCtx()->getWriteUnitOfWork());

    // With the resources unstashed, verify that the Session reports an empty stashed state.
    ASSERT(txnParticipant->reportStashedState().isEmpty());

    // Commit the transaction. This allows us to release locks.
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);
}

TEST_F(TransactionsMetricsTest, ReportUnstashedResources) {
    auto tickSource = initMockTickSource();
    auto clockSource = initMockPreciseClockSource();
    auto startTime = Date_t::now();
    clockSource->reset(startTime);

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    const auto autocommit = false;
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "find");
    ASSERT(opCtx()->getWriteUnitOfWork());
    ASSERT(opCtx()->lockState()->isLocked());

    // Prepare transaction and extend duration in the prepared state.
    txnParticipant->prepareTransaction(opCtx(), {});
    const long prepareDuration = 10;
    tickSource->advance(Microseconds(prepareDuration));

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant->reportUnstashedState(opCtx(), &unstashedStateBuilder);
    auto unstashedState = unstashedStateBuilder.obj();
    auto transactionDocument = unstashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), *opCtx()->getTxnNumber());
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        startTime + stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()});
    ASSERT_EQ(transactionDocument.getField("timePreparedMicros").numberLong(), prepareDuration);

    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that
    // the values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // With the resources stashed, verify that the Session reports an empty unstashed state.
    BSONObjBuilder builder;
    txnParticipant->reportUnstashedState(opCtx(), &builder);
    ASSERT(builder.obj().isEmpty());
}

TEST_F(TransactionsMetricsTest, ReportUnstashedResourcesForARetryableWrite) {
    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), boost::none, boost::none);
    txnParticipant->unstashTransactionResources(opCtx(), "find");

    // Build a BSONObj containing the details which we expect to see reported when we call
    // Session::reportUnstashedState. For a retryable write, we should only include the txnNumber.
    BSONObjBuilder reportBuilder;
    BSONObjBuilder transactionBuilder(reportBuilder.subobjStart("transaction"));
    BSONObjBuilder parametersBuilder(transactionBuilder.subobjStart("parameters"));
    parametersBuilder.append("txnNumber", *opCtx()->getTxnNumber());
    parametersBuilder.done();
    transactionBuilder.done();

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant->reportUnstashedState(opCtx(), &unstashedStateBuilder);
    ASSERT_BSONOBJ_EQ(unstashedStateBuilder.obj(), reportBuilder.obj());
}

namespace {

/*
 * Constructs a ClientMetadata BSONObj with the given application name.
 */
BSONObj constructClientMetadata(StringData appName) {
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               appName,
                                               &builder));
    return builder.obj();
}
}  // namespace

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponStash) {
    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant->getSingleTransactionStats().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());

    // Create another ClientMetadata object.
    auto newObj = constructClientMetadata("newAppName");
    auto newClientMetadata = ClientMetadata::parse(newObj["client"]);
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(newClientMetadata.getValue()));

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    txnParticipant->stashTransactionResources(opCtx());

    // LastClientInfo's clientMetadata should have been updated to the new ClientMetadata object.
    lastClientInfo = txnParticipant->getSingleTransactionStats().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.appName, "newAppName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, newObj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponCommit) {
    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->commitUnpreparedTransaction(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant->getSingleTransactionStats().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponAbort) {
    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);

    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    txnParticipant->abortActiveTransaction(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant->getSingleTransactionStats().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());
}

/*
 * Sets up the additive metrics for Transactions Metrics test.
 */
void setupAdditiveMetrics(const int metricValue, OperationContext* opCtx) {
    CurOp::get(opCtx)->debug().additiveMetrics.keysExamined = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.docsExamined = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nMatched = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nModified = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.ninserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.ndeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nmoved = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysInserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysDeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.prepareReadConflicts = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.writeConflicts = metricValue;
}

/*
 * Builds expected parameters info string.
 */
void buildParametersInfoString(StringBuilder* sb,
                               LogicalSessionId sessionId,
                               const TxnNumber txnNum,
                               const repl::ReadConcernArgs readConcernArgs) {
    BSONObjBuilder lsidBuilder;
    sessionId.serialize(&lsidBuilder);
    (*sb) << "parameters:{ lsid: " << lsidBuilder.done().toString() << ", txnNumber: " << txnNum
          << ", autocommit: false"
          << ", readConcern: " << readConcernArgs.toBSON().getObjectField("readConcern") << " },";
}

/*
 * Builds expected single transaction stats info string.
 */
void buildSingleTransactionStatsString(StringBuilder* sb, const int metricValue) {
    (*sb) << " keysExamined:" << metricValue << " docsExamined:" << metricValue
          << " nMatched:" << metricValue << " nModified:" << metricValue
          << " ninserted:" << metricValue << " ndeleted:" << metricValue
          << " nmoved:" << metricValue << " keysInserted:" << metricValue
          << " keysDeleted:" << metricValue << " prepareReadConflicts:" << metricValue
          << " writeConflicts:" << metricValue;
}

/*
 * Builds the time active and time inactive info string.
 */
void buildTimeActiveInactiveString(StringBuilder* sb,
                                   TransactionParticipant* txnParticipant,
                                   TickSource* tickSource,
                                   TickSource::Tick curTick) {
    // Add time active micros to string.
    (*sb) << " timeActiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant->getSingleTransactionStats().getTimeActiveMicros(tickSource,
                                                                                 curTick));

    // Add time inactive micros to string.
    (*sb) << " timeInactiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(tickSource,
                                                                                   curTick));
}

/*
 * Builds the total prepared duration info string.
 */
void buildPreparedDurationString(StringBuilder* sb,
                                 TransactionParticipant* txnParticipant,
                                 TickSource* tickSource,
                                 TickSource::Tick curTick) {
    (*sb) << " totalPreparedDurationMicros:"
          << durationCount<Microseconds>(
                 txnParticipant->getSingleTransactionStats().getPreparedDuration(tickSource,
                                                                                 curTick));
}

/*
 * Builds the entire expected transaction info string and returns it.
 */
std::string buildTransactionInfoString(OperationContext* opCtx,
                                       TransactionParticipant* txnParticipant,
                                       std::string terminationCause,
                                       const LogicalSessionId sessionId,
                                       const TxnNumber txnNum,
                                       const int metricValue,
                                       const bool wasPrepared) {
    // Calling transactionInfoForLog to get the actual transaction info string.
    const auto lockerInfo =
        opCtx->lockState()->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase());
    // Building expected transaction info string.
    StringBuilder parametersInfo;
    buildParametersInfoString(
        &parametersInfo, sessionId, txnNum, repl::ReadConcernArgs::get(opCtx));

    StringBuilder readTimestampInfo;
    readTimestampInfo
        << " readTimestamp:"
        << txnParticipant->getSpeculativeTransactionReadOpTimeForTest().getTimestamp().toString()
        << ",";

    StringBuilder singleTransactionStatsInfo;
    buildSingleTransactionStatsString(&singleTransactionStatsInfo, metricValue);

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    StringBuilder timeActiveAndInactiveInfo;
    buildTimeActiveInactiveString(
        &timeActiveAndInactiveInfo, txnParticipant, tickSource, tickSource->getTicks());

    BSONObjBuilder locks;
    if (lockerInfo) {
        lockerInfo->stats.report(&locks);
    }

    // Puts all the substrings together into one expected info string. The expected info string will
    // look something like this:
    // parameters:{ lsid: { id: UUID("f825288c-100e-49a1-9fd7-b95c108049e6"), uid: BinData(0,
    // E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855) }, txnNumber: 1,
    // autocommit: false }, readTimestamp:Timestamp(0, 0), keysExamined:1 docsExamined:1 nMatched:1
    // nModified:1 ninserted:1 ndeleted:1 nmoved:1 keysInserted:1 keysDeleted:1
    // prepareReadConflicts:1 writeConflicts:1 terminationCause:committed timeActiveMicros:3
    // timeInactiveMicros:2 numYields:0 locks:{ Global: { acquireCount: { r: 6, w: 4 } }, Database:
    // { acquireCount: { r: 1, w: 1, W: 2 } }, Collection: { acquireCount: { R: 1 } }, oplog: {
    // acquireCount: { W: 1 } } } 0ms, wasPrepared:1, totalPreparedDurationMicros: 10
    StringBuilder expectedTransactionInfo;
    expectedTransactionInfo << parametersInfo.str() << readTimestampInfo.str()
                            << singleTransactionStatsInfo.str()
                            << " terminationCause:" << terminationCause
                            << timeActiveAndInactiveInfo.str() << " numYields:" << 0
                            << " locks:" << locks.done().toString() << " "
                            << duration_cast<Milliseconds>(
                                   txnParticipant->getSingleTransactionStats().getDuration(
                                       tickSource, tickSource->getTicks()))
                            << " wasPrepared:" << wasPrepared;
    if (wasPrepared) {
        StringBuilder totalPreparedDuration;
        buildPreparedDurationString(
            &totalPreparedDuration, txnParticipant, tickSource, tickSource->getTicks());
        expectedTransactionInfo << totalPreparedDuration.str();
    }
    return expectedTransactionInfo.str();
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterCommit) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));

    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant->commitUnpreparedTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string testTransactionInfo =
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "committed",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   false);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestPreparedTransactionInfoForLogAfterCommit) {
    auto tickSource = initMockTickSource();

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));

    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Prepare the transaction and extend the duration in the prepared state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    tickSource->advance(Microseconds(10));

    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string testTransactionInfo =
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "committed",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterAbort) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->abortActiveTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    std::string testTransactionInfo =
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   false);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestPreparedTransactionInfoForLogAfterAbort) {
    auto tickSource = initMockTickSource();

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Prepare the transaction and extend the duration in the prepared state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(10));

    txnParticipant->abortActiveTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    std::string testTransactionInfo =
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

DEATH_TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogWithNoLockerInfoStats, "invariant") {
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant->commitUnpreparedTransaction(opCtx());

    txnParticipant->transactionInfoForLogForTest(nullptr, true, readConcernArgs);
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant->commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogPreparedTransactionInfoAfterSlowCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    startCapturingLogMessages();
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant->abortActiveTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogPreparedTransactionInfoAfterSlowAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant->abortActiveTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterExceptionInPrepare) {
    auto tickSource = initMockTickSource();
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    _opObserver->onTransactionPrepareThrowsException = true;

    startCapturingLogMessages();
    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant->transactionIsAborted());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowStashedAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    txnParticipant->stashTransactionResources(opCtx());
    const auto txnResourceStashLocker = txnParticipant->getTxnResourceStashLockerForTest();
    ASSERT(txnResourceStashLocker);
    const auto lockerInfo = txnResourceStashLocker->getLockerInfo(boost::none);

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant->abortArbitraryTransaction();
    stopCapturingLogMessages();

    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TxnParticipantTest, WhenOldestTSRemovedNextOldestBecomesNewOldest) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Check that there are no Timestamps in the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 0U);

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    auto firstPrepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    // Check that we added a Timestamp to the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 1U);
    // Check that the oldest prepareTimestamp is equal to firstPrepareTimestamp because there is
    // only one prepared transaction on this Service.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), firstPrepareTimestamp);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());

    txnParticipant->stashTransactionResources(opCtx());
    auto originalClient = Client::releaseCurrent();

    /**
     * Make a new Session, Client, OperationContext and transaction.
     */
    auto service = opCtx()->getServiceContext();
    auto newClientOwned = service->makeClient("newClient");
    auto newClient = newClientOwned.get();
    Client::setCurrent(std::move(newClientOwned));

    const TxnNumber newTxnNum = 10;
    const auto newSessionId = makeLogicalSessionIdForTest();
    auto secondPrepareTimestamp = Timestamp();

    {
        auto newOpCtx = newClient->makeOperationContext();
        newOpCtx.get()->setLogicalSessionId(newSessionId);
        newOpCtx.get()->setTxnNumber(newTxnNum);

        MongoDOperationContextSession newOpCtxSession(newOpCtx.get());
        auto newTxnParticipant = TransactionParticipant::get(newOpCtx.get());
        newTxnParticipant->beginOrContinue(newTxnNum, false, true);
        newTxnParticipant->unstashTransactionResources(newOpCtx.get(), "prepareTransaction");

        // secondPrepareTimestamp should be greater than firstPreparedTimestamp because this
        // transaction was prepared after.
        secondPrepareTimestamp = newTxnParticipant->prepareTransaction(newOpCtx.get(), {});
        ASSERT_GT(secondPrepareTimestamp, firstPrepareTimestamp);
        // Check that we added a Timestamp to the set.
        ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 2U);
        // The oldest prepareTimestamp should still be firstPrepareTimestamp.
        prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
        ASSERT_EQ(prepareOpTime->getTimestamp(), firstPrepareTimestamp);
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    }

    Client::releaseCurrent();
    Client::setCurrent(std::move(originalClient));

    // Switch clients and abort the first transaction. This should cause the oldestActiveTS to be
    // equal to the secondPrepareTimestamp.
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 1U);
    prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), secondPrepareTimestamp);
}

TEST_F(TxnParticipantTest, ReturnNullTimestampIfNoOldestActiveTimestamp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Check that there are no Timestamps in the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 0U);

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->prepareTransaction(opCtx(), {});
    // Check that we added a Timestamp to the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 1U);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());

    txnParticipant->stashTransactionResources(opCtx());
    auto originalClient = Client::releaseCurrent();

    /**
    * Make a new Session, Client, OperationContext and transaction.
    */
    auto service = opCtx()->getServiceContext();
    auto newClientOwned = service->makeClient("newClient");
    auto newClient = newClientOwned.get();
    Client::setCurrent(std::move(newClientOwned));

    const TxnNumber newTxnNum = 10;
    const auto newSessionId = makeLogicalSessionIdForTest();

    {
        auto newOpCtx = newClient->makeOperationContext();
        newOpCtx.get()->setLogicalSessionId(newSessionId);
        newOpCtx.get()->setTxnNumber(newTxnNum);

        MongoDOperationContextSession newOpCtxSession(newOpCtx.get());
        auto newTxnParticipant = TransactionParticipant::get(newOpCtx.get());
        newTxnParticipant->beginOrContinue(newTxnNum, false, true);
        newTxnParticipant->unstashTransactionResources(newOpCtx.get(), "prepareTransaction");

        // secondPrepareTimestamp should be greater than firstPreparedTimestamp because this
        // transaction was prepared after.
        newTxnParticipant->prepareTransaction(newOpCtx.get(), {});
        // Check that we added a Timestamp to the set.
        ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 2U);
        // The oldest prepareTimestamp should still be firstPrepareTimestamp.
        ASSERT_FALSE(txnParticipant->transactionIsAborted());

        // Abort this transaction and check that we have decremented the total active timestamps
        // count.
        newTxnParticipant->abortActiveTransaction(newOpCtx.get());
        ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 1U);
    }

    Client::releaseCurrent();
    Client::setCurrent(std::move(originalClient));

    // Switch clients and abort the first transaction. This means we no longer have an oldest active
    // timestamp.
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 0U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime(), boost::none);
}

TEST_F(TxnParticipantTest, ProperlyMaintainOldestNonMajorityCommittedOpTimeSet) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Check that there are no Timestamps in the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 0U);

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
    // Check that we added a Timestamp to the set.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 1U);

    // Check that the oldest prepareTimestamp is equal to first prepareTimestamp because there is
    // only one prepared transaction on this Service.
    auto prepareOpTime = ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime();
    ASSERT_EQ(prepareOpTime->getTimestamp(), prepareTimestamp);

    // Check that oldestNonMajorityCommittedOpTimes also has this prepareTimestamp and that the
    // pair's finishOpTime is Timestamp::max() because this transaction has not been
    // committed/aborted.
    auto nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();
    ASSERT_EQ(nonMajorityCommittedOpTime->getTimestamp(), prepareTimestamp);
    auto nonMajorityCommittedOpTimeFinishOpTime =
        ServerTransactionsMetrics::get(opCtx())->getFinishOpTimeOfOldestNonMajCommitted_forTest();
    ASSERT_EQ(nonMajorityCommittedOpTimeFinishOpTime->getTimestamp(), Timestamp::max());

    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    // Since this test uses a mock opObserver, we have to manually set the finishTimestamp on the
    // txnParticipant.
    auto finishOpTime = repl::OpTime({10, 10}, 0);
    repl::ReplClientInfo::forClient(opCtx()->getClient()).setLastOp(finishOpTime);

    txnParticipant->abortActiveTransaction(opCtx());
    ASSERT(txnParticipant->transactionIsAborted());

    // Make sure that we moved the OpTime from the oldestActiveOplogEntryOpTimes to
    // oldestNonMajorityCommittedOpTimes along with the abort/commit oplog entry OpTime
    // associated with the transaction.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalActiveOpTimes(), 0U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getOldestActiveOpTime(), boost::none);

    nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();
    nonMajorityCommittedOpTimeFinishOpTime =
        ServerTransactionsMetrics::get(opCtx())->getFinishOpTimeOfOldestNonMajCommitted_forTest();
    ASSERT_FALSE(nonMajorityCommittedOpTime == boost::none);
    ASSERT_FALSE(nonMajorityCommittedOpTimeFinishOpTime == boost::none);
    ASSERT_EQ(nonMajorityCommittedOpTime->getTimestamp(), prepareTimestamp);
    ASSERT_EQ(nonMajorityCommittedOpTimeFinishOpTime, finishOpTime);

    // If we pass in a mock commit point that is greater than the finish timestamp of the
    // oldestNonMajorityCommittedOpTime, it should be removed from the set. This would mean that
    // the abort/commit oplog entry is majority committed.
    ServerTransactionsMetrics::get(opCtx())->removeOpTimesLessThanOrEqToCommittedOpTime(
        repl::OpTime::max());
    nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();
    ASSERT_EQ(nonMajorityCommittedOpTime, boost::none);
}

TEST_F(TxnParticipantTest, GetOldestNonMajorityCommittedOpTimeReturnsOldestEntry) {
    const auto earlierOpTime = repl::OpTime({1, 1}, 0);
    const auto earlierFinishOpTime = repl::OpTime({3, 2}, 0);

    const auto middleOpTime = repl::OpTime({1, 2}, 0);
    const auto middleFinishOpTime = repl::OpTime({3, 3}, 0);

    const auto laterOpTime = repl::OpTime({1, 3}, 0);
    const auto laterFinishOpTime = repl::OpTime({3, 4}, 0);

    ServerTransactionsMetrics::get(opCtx())->addActiveOpTime(earlierOpTime);
    ServerTransactionsMetrics::get(opCtx())->removeActiveOpTime(earlierOpTime, earlierFinishOpTime);

    ServerTransactionsMetrics::get(opCtx())->addActiveOpTime(middleOpTime);
    ServerTransactionsMetrics::get(opCtx())->removeActiveOpTime(middleOpTime, middleFinishOpTime);

    ServerTransactionsMetrics::get(opCtx())->addActiveOpTime(laterOpTime);
    ServerTransactionsMetrics::get(opCtx())->removeActiveOpTime(laterOpTime, laterFinishOpTime);

    auto nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();

    ASSERT_EQ(*nonMajorityCommittedOpTime, repl::OpTime({1, 1}, 0));

    // If we pass in a mock commit point that is greater than the finish timestamp of the
    // oldestNonMajorityCommittedOpTime, it should be removed from the set. This would mean that
    // the abort/commit oplog entry is majority committed.
    ServerTransactionsMetrics::get(opCtx())->removeOpTimesLessThanOrEqToCommittedOpTime(
        repl::OpTime::max());
    nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();
    ASSERT_EQ(nonMajorityCommittedOpTime, boost::none);

    // Test that we can remove only a part of the set by passing in a commit point that is only
    // greater than or equal to two of the optimes.
    ServerTransactionsMetrics::get(opCtx())->addActiveOpTime(earlierOpTime);
    ServerTransactionsMetrics::get(opCtx())->removeActiveOpTime(earlierOpTime, earlierFinishOpTime);

    ServerTransactionsMetrics::get(opCtx())->addActiveOpTime(middleOpTime);
    ServerTransactionsMetrics::get(opCtx())->removeActiveOpTime(middleOpTime, middleFinishOpTime);

    ServerTransactionsMetrics::get(opCtx())->addActiveOpTime(laterOpTime);
    ServerTransactionsMetrics::get(opCtx())->removeActiveOpTime(laterOpTime, laterFinishOpTime);

    nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();

    ASSERT_EQ(*nonMajorityCommittedOpTime, earlierOpTime);

    ServerTransactionsMetrics::get(opCtx())->removeOpTimesLessThanOrEqToCommittedOpTime(
        repl::OpTime({3, 3}, 0));
    nonMajorityCommittedOpTime =
        ServerTransactionsMetrics::get(opCtx())->getOldestNonMajorityCommittedOpTime();

    // earlierOpTime and middleOpTime must have been removed because their finishOpTime are less
    // than or equal to the mock commit point.
    ASSERT_EQ(nonMajorityCommittedOpTime, laterOpTime);
}

class RecoverDecisionFromLocalParticipantTest : public TxnParticipantTest {
private:
    void _putParticipantInProgressWithoutSessionCheckout() {
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "insert");
        ASSERT(txnParticipant->inMultiDocumentTransaction());
        txnParticipant->stashTransactionResources(opCtx());
    }

    Timestamp _putParticipantInPrepareWithoutSessionCheckout() {
        _putParticipantInProgressWithoutSessionCheckout();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
        const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx(), {});
        ASSERT(txnParticipant->transactionIsPrepared());
        txnParticipant->stashTransactionResources(opCtx());
        return prepareTimestamp;
    }

protected:
    void putParticipantInProgress() {
        auto sessionCheckout = checkOutSession();
        _putParticipantInProgressWithoutSessionCheckout();
    }

    void putParticipantInAbortedWithoutPrepare() {
        auto sessionCheckout = checkOutSession();

        _putParticipantInProgressWithoutSessionCheckout();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
        txnParticipant->abortActiveTransaction(opCtx());
        ASSERT(txnParticipant->transactionIsAborted());
        // No need to stash transaction resources after abort.
    }

    void putParticipantInCommittedWithoutPrepare() {
        auto sessionCheckout = checkOutSession();

        _putParticipantInProgressWithoutSessionCheckout();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
        txnParticipant->commitUnpreparedTransaction(opCtx());
        ASSERT(txnParticipant->transactionIsCommitted());
        // No need to stash transaction resources after commit.
    }

    Timestamp putParticipantInPrepare() {
        auto sessionCheckout = checkOutSession();
        return _putParticipantInPrepareWithoutSessionCheckout();
    }

    void putParticipantInAbortedAfterPrepare() {
        auto sessionCheckout = checkOutSession();

        _putParticipantInPrepareWithoutSessionCheckout();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
        txnParticipant->abortActiveTransaction(opCtx());
        ASSERT(txnParticipant->transactionIsAborted());
        // No need to stash transaction resources after abort.
    }

    void putParticipantInCommittedAfterPrepare() {
        auto sessionCheckout = checkOutSession();

        const auto prepareTimestamp = _putParticipantInPrepareWithoutSessionCheckout();
        const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
        txnParticipant->commitPreparedTransaction(opCtx(), commitTS);
        ASSERT_TRUE(txnParticipant->transactionIsCommitted());
        // No need to stash transaction resources after commit.
    }

    void putParticipantInRetryableWrite() {
        MongoDOperationContextSession checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), boost::none, boost::none);
        ASSERT(!txnParticipant->inMultiDocumentTransaction() &&
               !txnParticipant->transactionIsCommitted() &&
               !txnParticipant->transactionIsAborted() && !txnParticipant->transactionIsPrepared());
    }

    void assertParticipantIsInProgressWithTxnNumber(const TxnNumber expectedTxnNumber) {
        MongoDOperationContextSession checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_EQUALS(expectedTxnNumber, txnParticipant->getActiveTxnNumber());
        ASSERT(txnParticipant->inMultiDocumentTransaction() &&
               !txnParticipant->transactionIsPrepared());
    }

    void assertParticipantIsPreparedWithTxnNumber(const TxnNumber expectedTxnNumber) {
        MongoDOperationContextSession checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_EQUALS(expectedTxnNumber, txnParticipant->getActiveTxnNumber());
        ASSERT(txnParticipant->transactionIsPrepared());
    }

    void assertParticipantIsAbortedWithTxnNumber(const TxnNumber expectedTxnNumber) {
        MongoDOperationContextSession checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_EQUALS(expectedTxnNumber, txnParticipant->getActiveTxnNumber());
        ASSERT(txnParticipant->transactionIsAborted());
    }

    void assertParticipantIsCommittedWithTxnNumber(const TxnNumber expectedTxnNumber) {
        MongoDOperationContextSession checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_EQUALS(expectedTxnNumber, txnParticipant->getActiveTxnNumber());
        ASSERT(txnParticipant->transactionIsCommitted());
    }

    void assertParticipantIsInRetryableWriteWithTxnNumber(const TxnNumber expectedTxnNumber) {
        MongoDOperationContextSession checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_EQUALS(expectedTxnNumber, txnParticipant->getActiveTxnNumber());
        ASSERT(!txnParticipant->inMultiDocumentTransaction() &&
               !txnParticipant->transactionIsCommitted() &&
               !txnParticipant->transactionIsAborted() && !txnParticipant->transactionIsPrepared());
    }

    void assertRecoverDecisionFromDifferentOpCtxThrowsTransactionTooOld(
        const LogicalSessionId& lsid, const TxnNumber txnNumber) {
        auto recoverDecisionThrowsTransactionTooOldFunc = [&](OperationContext* newOpCtx) {
            newOpCtx->setLogicalSessionId(lsid);
            newOpCtx->setTxnNumber(txnNumber);

            ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(newOpCtx),
                               AssertionException,
                               ErrorCodes::TransactionTooOld);
        };
        runFunctionFromDifferentOpCtx(recoverDecisionThrowsTransactionTooOldFunc);
    }

    void assertRecoverDecisionFromDifferentOpCtxThrowsNoSuchTransaction(
        const LogicalSessionId& lsid, const TxnNumber txnNumber) {
        auto recoverDecisionThrowsNoSuchTransactionFunc = [&](OperationContext* newOpCtx) {
            newOpCtx->setLogicalSessionId(lsid);
            newOpCtx->setTxnNumber(txnNumber);

            ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(newOpCtx),
                               AssertionException,
                               ErrorCodes::NoSuchTransaction);
        };
        runFunctionFromDifferentOpCtx(recoverDecisionThrowsNoSuchTransactionFunc);
    }
};

//
// Local TransactionParticipant has *same* TxnNumber as recoverCommit request.
//

TEST_F(
    RecoverDecisionFromLocalParticipantTest,
    AbortsActiveTransactionAndThrowsNoSuchTransactionIfParticipantIsInProgressWithSameTxnNumber) {
    putParticipantInProgress();
    ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    assertParticipantIsAbortedWithTxnNumber(*opCtx()->getTxnNumber());
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsNoSuchTransactionIfParticipantIsAbortedWithoutPrepareWithSameTxnNumber) {
    putParticipantInAbortedWithoutPrepare();
    ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    assertParticipantIsAbortedWithTxnNumber(*opCtx()->getTxnNumber());
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       DoesNotThrowIfParticipantIsCommittedWithoutPrepareWithSameTxnNumber) {
    putParticipantInCommittedWithoutPrepare();
    recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx());
    assertParticipantIsCommittedWithTxnNumber(*opCtx()->getTxnNumber());
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsAnonymousErrorIfParticipantIsPreparedWithSameTxnNumber) {
    putParticipantInPrepare();
    ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx()),
                       AssertionException,
                       51021);
    assertParticipantIsPreparedWithTxnNumber(*opCtx()->getTxnNumber());
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsNoSuchTransactionIfParticipantIsAbortedAfterPrepareWithSameTxnNumber) {
    putParticipantInAbortedAfterPrepare();
    ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    assertParticipantIsAbortedWithTxnNumber(*opCtx()->getTxnNumber());
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       DoesNotThrowIfParticipantIsCommittedAfterPrepareWithSameTxnNumber) {
    putParticipantInCommittedAfterPrepare();
    recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx());
    assertParticipantIsCommittedWithTxnNumber(*opCtx()->getTxnNumber());
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsNoSuchTransactionIfTxnNumberCorrespondsToRetryableWrite) {
    putParticipantInRetryableWrite();
    ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    assertParticipantIsInRetryableWriteWithTxnNumber(*opCtx()->getTxnNumber());
}

//
// Local TransactionParticipant has *higher* TxnNumber than recoverCommit request.
//

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsTransactionTooOldIfTransactionParticipantInProgressHasHigherTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto oldTxnNumber = participantTxnNumber - 1;

    putParticipantInProgress();
    assertRecoverDecisionFromDifferentOpCtxThrowsTransactionTooOld(lsid, oldTxnNumber);
    assertParticipantIsInProgressWithTxnNumber(participantTxnNumber);
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsTransactionTooOldIfTransactionParticipantInPrepareHasHigherTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto oldTxnNumber = participantTxnNumber - 1;

    putParticipantInPrepare();
    assertRecoverDecisionFromDifferentOpCtxThrowsTransactionTooOld(lsid, oldTxnNumber);
    assertParticipantIsPreparedWithTxnNumber(participantTxnNumber);
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsTransactionTooOldIfTransactionParticipantInAbortedHasHigherTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto oldTxnNumber = participantTxnNumber - 1;

    putParticipantInAbortedAfterPrepare();
    assertRecoverDecisionFromDifferentOpCtxThrowsTransactionTooOld(lsid, oldTxnNumber);
    assertParticipantIsAbortedWithTxnNumber(participantTxnNumber);
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsTransactionTooOldIfTransactionParticipantInCommittedHasHigherTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto oldTxnNumber = participantTxnNumber - 1;

    putParticipantInCommittedAfterPrepare();
    assertRecoverDecisionFromDifferentOpCtxThrowsTransactionTooOld(lsid, oldTxnNumber);
    assertParticipantIsCommittedWithTxnNumber(participantTxnNumber);
}

TEST_F(RecoverDecisionFromLocalParticipantTest,
       ThrowsTransactionTooOldIfTransactionParticipantInRetryableWriteHasHigherTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto oldTxnNumber = participantTxnNumber - 1;

    putParticipantInRetryableWrite();
    assertRecoverDecisionFromDifferentOpCtxThrowsTransactionTooOld(lsid, oldTxnNumber);
    assertParticipantIsInRetryableWriteWithTxnNumber(participantTxnNumber);
}

//
// Local TransactionParticipant has *lower* TxnNumber than recoverCommit request.
//

TEST_F(
    RecoverDecisionFromLocalParticipantTest,
    AbortsOlderAndNewerTransactionAndThrowsNoSuchTransactionIfParticipantIsInProgressWithLowerTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto newTxnNumber = participantTxnNumber + 1;

    putParticipantInProgress();
    assertRecoverDecisionFromDifferentOpCtxThrowsNoSuchTransaction(lsid, newTxnNumber);
    assertParticipantIsAbortedWithTxnNumber(newTxnNumber);
}

TEST_F(RecoverDecisionFromLocalParticipantTest, ThrowsIfParticipantIsInPrepareWithLowerTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto newTxnNumber = participantTxnNumber + 1;

    putParticipantInPrepare();
    auto recoverDecisionThrowsAnonymousErrorFunc = [&](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(lsid);
        newOpCtx->setTxnNumber(newTxnNumber);

        ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(newOpCtx),
                           AssertionException,
                           51021);
    };
    runFunctionFromDifferentOpCtx(recoverDecisionThrowsAnonymousErrorFunc);
    assertParticipantIsPreparedWithTxnNumber(participantTxnNumber);
}

TEST_F(
    RecoverDecisionFromLocalParticipantTest,
    StartsAndAbortsNewerTransactionAndThrowsNoSuchTransactionIfParticipantIsAbortedWithLowerTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto newTxnNumber = participantTxnNumber + 1;

    putParticipantInAbortedAfterPrepare();
    assertRecoverDecisionFromDifferentOpCtxThrowsNoSuchTransaction(lsid, newTxnNumber);
    assertParticipantIsAbortedWithTxnNumber(newTxnNumber);
}

TEST_F(
    RecoverDecisionFromLocalParticipantTest,
    StartsAndAbortsNewerTransactionAndThrowsNoSuchTransactionIfParticipantIsCommittedWithLowerTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto newTxnNumber = participantTxnNumber + 1;

    putParticipantInCommittedAfterPrepare();
    assertRecoverDecisionFromDifferentOpCtxThrowsNoSuchTransaction(lsid, newTxnNumber);
    assertParticipantIsAbortedWithTxnNumber(newTxnNumber);
}

TEST_F(
    RecoverDecisionFromLocalParticipantTest,
    StartsAndAbortsNewerTransactionAndThrowsNoSuchTransactionIfParticipantIsInRetryableWriteWithLowerTxnNumber) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto newTxnNumber = participantTxnNumber + 1;

    putParticipantInRetryableWrite();
    assertRecoverDecisionFromDifferentOpCtxThrowsNoSuchTransaction(lsid, newTxnNumber);
    assertParticipantIsAbortedWithTxnNumber(newTxnNumber);
}

//
// Recovering the decision is retryable.
//

TEST_F(RecoverDecisionFromLocalParticipantTest,
       RecoverDecisionCanBeRetriedIfFirstTryThrowsLockTimeout) {
    const auto lsid = *opCtx()->getLogicalSessionId();
    const auto participantTxnNumber = *opCtx()->getTxnNumber();
    const auto newTxnNumber = participantTxnNumber + 1;

    putParticipantInCommittedAfterPrepare();

    // First recoverDecision attempt throws LockTimeout.
    {
        Lock::GlobalLock lk(opCtx(), MODE_X);
        auto recoverDecisionThrowsLockTimeoutFunc = [&](OperationContext* newOpCtx) {
            newOpCtx->setLogicalSessionId(lsid);
            newOpCtx->setTxnNumber(newTxnNumber);

            ASSERT_THROWS_CODE(recoverDecisionFromLocalParticipantOrAbortLocalParticipant(newOpCtx),
                               AssertionException,
                               ErrorCodes::LockTimeout);
        };
        runFunctionFromDifferentOpCtx(recoverDecisionThrowsLockTimeoutFunc);
    }
    assertParticipantIsInProgressWithTxnNumber(newTxnNumber);

    // Retry recoverDecision.
    assertRecoverDecisionFromDifferentOpCtxThrowsNoSuchTransaction(lsid, newTxnNumber);

    assertParticipantIsAbortedWithTxnNumber(newTxnNumber);
}

}  // namespace
}  // namespace mongo
