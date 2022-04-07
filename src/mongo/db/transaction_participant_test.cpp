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

#include <boost/optional/optional_io.hpp>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/logger/logger.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");

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
    void onTransactionPrepare(OperationContext* opCtx,
                              const std::vector<OplogSlot>& reservedSlots,
                              std::vector<repl::ReplOperation>& statements) override;

    bool onTransactionPrepareThrowsException = false;
    bool transactionPrepared = false;
    stdx::function<void()> onTransactionPrepareFn = []() {};

    void onUnpreparedTransactionCommit(OperationContext* opCtx,
                                       const std::vector<repl::ReplOperation>& statements) override;
    bool onUnpreparedTransactionCommitThrowsException = false;
    bool unpreparedTransactionCommitted = false;
    stdx::function<void(const std::vector<repl::ReplOperation>&)> onUnpreparedTransactionCommitFn =
        [](const std::vector<repl::ReplOperation>& statements) {};


    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept override;
    bool onPreparedTransactionCommitThrowsException = false;
    bool preparedTransactionCommitted = false;
    stdx::function<void(OplogSlot, Timestamp, const std::vector<repl::ReplOperation>&)>
        onPreparedTransactionCommitFn = [](OplogSlot commitOplogEntryOpTime,
                                           Timestamp commitTimestamp,
                                           const std::vector<repl::ReplOperation>& statements) {};

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override;
    bool onTransactionAbortThrowsException = false;
    bool transactionAborted = false;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType) override;

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

void OpObserverMock::onTransactionPrepare(OperationContext* opCtx,
                                          const std::vector<OplogSlot>& reservedSlots,
                                          std::vector<repl::ReplOperation>& statements) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onTransactionPrepare(opCtx, reservedSlots, statements);

    uassert(ErrorCodes::OperationFailed,
            "onTransactionPrepare() failed",
            !onTransactionPrepareThrowsException);
    transactionPrepared = true;
    onTransactionPrepareFn();
}

void OpObserverMock::onUnpreparedTransactionCommit(
    OperationContext* opCtx, const std::vector<repl::ReplOperation>& statements) {
    ASSERT(opCtx->lockState()->inAWriteUnitOfWork());

    OpObserverNoop::onUnpreparedTransactionCommit(opCtx, statements);

    uassert(ErrorCodes::OperationFailed,
            "onUnpreparedTransactionCommit() failed",
            !onUnpreparedTransactionCommitThrowsException);

    unpreparedTransactionCommitted = true;
    onUnpreparedTransactionCommitFn(statements);
}

void OpObserverMock::onPreparedTransactionCommit(
    OperationContext* opCtx,
    OplogSlot commitOplogEntryOpTime,
    Timestamp commitTimestamp,
    const std::vector<repl::ReplOperation>& statements) noexcept {
    ASSERT_FALSE(opCtx->lockState()->inAWriteUnitOfWork());
    // The 'commitTimestamp' must be cleared before we write the oplog entry.
    ASSERT(opCtx->recoveryUnit()->getCommitTimestamp().isNull());

    OpObserverNoop::onPreparedTransactionCommit(
        opCtx, commitOplogEntryOpTime, commitTimestamp, statements);
    uassert(ErrorCodes::OperationFailed,
            "onPreparedTransactionCommit() failed",
            !onPreparedTransactionCommitThrowsException);
    preparedTransactionCommitted = true;
    onPreparedTransactionCommitFn(commitOplogEntryOpTime, commitTimestamp, statements);
}

void OpObserverMock::onTransactionAbort(OperationContext* opCtx,
                                        boost::optional<OplogSlot> abortOplogEntryOpTime) {
    OpObserverNoop::onTransactionAbort(opCtx, abortOplogEntryOpTime);
    uassert(ErrorCodes::OperationFailed,
            "onTransactionAbort() failed",
            !onTransactionAbortThrowsException);
    transactionAborted = true;
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType) {
    // If the oplog is not disabled for this namespace, then we need to reserve an op time for the
    // drop.
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
    }
    return {};
}

/**
 * When this class is in scope, makes the system behave as if we're in a DBDirectClient.
 */
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
        const auto service = opCtx()->getServiceContext();
        auto _storageInterfaceImpl = std::make_unique<repl::StorageInterfaceImpl>();

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(service, std::move(_storageInterfaceImpl));
        MongoDSessionCatalog::onStepUp(opCtx());

        // We use the mocked storage interface here since StorageInterfaceImpl does not support
        // getPointInTimeReadTimestamp().
        auto _storageInterfaceMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::StorageInterface::set(service, std::move(_storageInterfaceMock));

        OpObserverRegistry* opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        auto mockObserver = stdx::make_unique<OpObserverMock>();
        _opObserver = mockObserver.get();
        opObserverRegistry->addObserver(std::move(mockObserver));

        {
            // Set up a collection so that TransactionParticipant::prepareTransaction() can safely
            // access it.
            AutoGetOrCreateDb autoDb(opCtx(), kNss.db(), MODE_X);
            auto db = autoDb.getDb();
            ASSERT_TRUE(db);

            WriteUnitOfWork wuow(opCtx());
            CollectionOptions options;
            options.uuid = _uuid;
            db->createCollection(opCtx(), kNss, options);
            wuow.commit();
        }

        opCtx()->setLogicalSessionId(_sessionId);
        opCtx()->setTxnNumber(_txnNumber);
        opCtx()->setInMultiDocumentTransaction();

        // Normally, committing a transaction is supposed to usassert if the corresponding prepare
        // has not been majority committed. We excempt our unit tests from this expectation.
        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode"
                                << "alwaysOn"));
    }

    void tearDown() override {
        _opObserver = nullptr;

        // Clear all sessions to free up any stashed resources.
        SessionCatalog::get(opCtx()->getServiceContext())->reset_forTest();

        MockReplCoordServerFixture::tearDown();

        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode"
                                << "off"));
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
    }

    void runFunctionFromDifferentOpCtx(std::function<void(OperationContext*)> func) {
        // Create a new client (e.g. for migration) and opCtx.
        auto newClientOwned = getServiceContext()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto newOpCtx = cc().makeOperationContext();
        func(newOpCtx.get());
    }

    std::unique_ptr<MongoDOperationContextSession> checkOutSession(
        boost::optional<bool> startNewTxn = true) {
        opCtx()->lockState()->setShouldConflictWithSecondaryBatchApplication(false);
        opCtx()->setInMultiDocumentTransaction();
        auto opCtxSession = std::make_unique<MongoDOperationContextSession>(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, startNewTxn);
        return opCtxSession;
    }

    const LogicalSessionId _sessionId{makeLogicalSessionIdForTest()};
    const TxnNumber _txnNumber{20};
    const OptionalCollectionUUID _uuid = UUID::gen();

    OpObserverMock* _opObserver = nullptr;
};

// Test that transaction lock acquisition times out in `maxTransactionLockRequestTimeoutMillis`
// milliseconds.
TEST_F(TxnParticipantTest, TransactionThrowsLockTimeoutIfLockIsUnavailable) {
    const std::string dbName = "TestDB";

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    { Lock::DBLock dbXLock(opCtx(), dbName, MODE_X); }
    txnParticipant.stashTransactionResources(opCtx());
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
        newOpCtx.get()->setInMultiDocumentTransaction();

        MongoDOperationContextSession newOpCtxSession(newOpCtx.get());
        auto newTxnParticipant = TransactionParticipant::get(newOpCtx.get());
        newTxnParticipant.beginOrContinue(newOpCtx.get(), newTxnNum, false, true);
        newTxnParticipant.unstashTransactionResources(newOpCtx.get(), "insert");

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
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());
    ASSERT(opCtx()->lockState()->isLocked());

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_NOT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_NOT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());

    // Commit the transaction. This allows us to release locks.
    txnParticipant.commitUnpreparedTransaction(opCtx());
}

TEST_F(TxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    // Must specify startTransaction=true and autocommit=false to start a transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());

    // Cannot try to start a transaction that already started.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, true),
        AssertionException,
        ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, AutocommitRequiredOnEveryTxnOp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // We must have stashed transaction resources to do a second operation on the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    auto txnNum = *opCtx()->getTxnNumber();
    // Omitting 'autocommit' after the first statement of a transaction should throw an error.
    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(opCtx(), txnNum, boost::none, boost::none),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);

    // Including autocommit=false should succeed.
    txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, boost::none);
}

DEATH_TEST_F(TxnParticipantTest, AutocommitCannotBeTrue, "invariant") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'autocommit=true' is not allowed and should crash.
    txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), true, boost::none);
}

DEATH_TEST_F(TxnParticipantTest, StartTransactionCannotBeFalse, "invariant") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'startTransaction=false' is not allowed and should crash.
    txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, false);
}

TEST_F(TxnParticipantTest, SameTransactionPreservesStoredStatements) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // We must have stashed transaction resources to re-open the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    // Check the transaction operations before re-opening the transaction.
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());

    // Re-opening the same transaction should have no effect.
    txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, boost::none);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());
}

TEST_F(TxnParticipantTest, AbortClearsStoredStatements) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.getTransactionOperationsForTest().empty());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

// This test makes sure the commit machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyUnpreparedTransactionCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

// This test makes sure the commit machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyPreparedTransactionCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, PrepareSucceedsWithNestedLocks) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    {
        Lock::GlobalLock lk1(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        Lock::GlobalLock lk2(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    }

    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, PrepareFailsOnTemporaryCollection) {
    NamespaceString tempCollNss(kNss.db(), "tempCollection");
    UUID tempCollUUID = UUID::gen();

    // Create a temporary collection so that we can write to it.
    {
        AutoGetOrCreateDb autoDb(opCtx(), kNss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx());
        CollectionOptions options;
        options.uuid = tempCollUUID;
        options.temp = true;
        db->createCollection(opCtx(), tempCollNss, options);
        wuow.commit();
    }

    // Set up a transaction on the temp collection
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    auto operation =
        repl::OplogEntry::makeInsertOperation(tempCollNss, tempCollUUID, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(TxnParticipantTest, CommitTransactionSetsCommitTimestampOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    auto originalFn = _opObserver->onPreparedTransactionCommitFn;
    _opObserver->onPreparedTransactionCommitFn =
        [&](OplogSlot commitOplogEntryOpTime,
            Timestamp commitTimestamp,
            const std::vector<repl::ReplOperation>& statements) {
            originalFn(commitOplogEntryOpTime, commitTimestamp, statements);

            ASSERT_GT(commitTimestamp, prepareTimestamp);

            ASSERT(statements.empty());
        };

    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});

    // The recovery unit is reset on commit.
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithCommitTimestampFailsOnUnpreparedTransaction) {
    const auto commitTimestamp = Timestamp(6, 6);

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionDoesNotSetCommitTimestampOnUnpreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto originalFn = _opObserver->onUnpreparedTransactionCommitFn;
    _opObserver->onUnpreparedTransactionCommitFn =
        [&](const std::vector<repl::ReplOperation>& statements) {
            originalFn(statements);
            ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
            ASSERT(statements.empty());
        };

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithoutCommitTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant.commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionWithNullCommitTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), Timestamp(), {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest,
       CommitTransactionWithCommitTimestampLessThanPrepareTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(
                           opCtx(), Timestamp(prepareTimestamp.getSecs() - 1, 1), {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyPreparedTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.prepareTransaction(opCtx(), {});
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, KillOpBeforeCommittingPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Prepare the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    opCtx()->markKilled(ErrorCodes::Interrupted);
    try {
        // The commit should throw, since the operation was killed.
        txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, boost::none);
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::Interrupted, ex.code());
    }

    // Check the session back in.
    txnParticipant.stashTransactionResources(opCtx());
    sessionCheckout->checkIn(opCtx());

    // The transaction state should have been unaffected.
    ASSERT_TRUE(txnParticipant.transactionIsPrepared());

    auto commitPreparedFunc = [&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(_sessionId);
        opCtx->setTxnNumber(_txnNumber);
        opCtx->setInMultiDocumentTransaction();

        // Check out the session and continue the transaction.
        auto opCtxSession = std::make_unique<MongoDOperationContextSession>(opCtx);
        auto newTxnParticipant = TransactionParticipant::get(opCtx);
        newTxnParticipant.beginOrContinue(
            opCtx, *(opCtx->getTxnNumber()), false, boost::none /*startNewTxn*/);

        newTxnParticipant.unstashTransactionResources(opCtx, "commitTransaction");
        newTxnParticipant.commitPreparedTransaction(opCtx, prepareTimestamp, boost::none);
    };

    // Now try to commit the transaction again, with a fresh operation context.
    runFunctionFromDifferentOpCtx(commitPreparedFunc);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, KillOpBeforeAbortingPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Prepare the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    opCtx()->markKilled(ErrorCodes::Interrupted);
    try {
        // The abort should throw, since the operation was killed.
        txnParticipant.abortTransaction(opCtx());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::Interrupted, ex.code());
    }

    // Check the session back in.
    txnParticipant.stashTransactionResources(opCtx());
    sessionCheckout->checkIn(opCtx());

    // The transaction state should have been unaffected.
    ASSERT_TRUE(txnParticipant.transactionIsPrepared());

    auto commitPreparedFunc = [&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(_sessionId);
        opCtx->setTxnNumber(_txnNumber);
        opCtx->setInMultiDocumentTransaction();

        // Check out the session and continue the transaction.
        auto opCtxSession = std::make_unique<MongoDOperationContextSession>(opCtx);
        auto newTxnParticipant = TransactionParticipant::get(opCtx);
        newTxnParticipant.beginOrContinue(
            opCtx, *(opCtx->getTxnNumber()), false, boost::none /*startNewTxn*/);

        newTxnParticipant.unstashTransactionResources(opCtx, "commitTransaction");
        newTxnParticipant.commitPreparedTransaction(opCtx, prepareTimestamp, boost::none);
    };

    // Now try to commit the transaction again, with a fresh operation context.
    runFunctionFromDifferentOpCtx(commitPreparedFunc);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}
TEST_F(TxnParticipantTest, StashedRollbackDoesntHoldClientLock) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    txnParticipant.prepareTransaction(opCtx(), {});

    unittest::Barrier startedRollback(2);
    unittest::Barrier finishRollback(2);

    // Rollback changes are executed in reverse order.
    opCtx()->recoveryUnit()->onRollback([&] { finishRollback.countDownAndWait(); });
    opCtx()->recoveryUnit()->onRollback([&] { startedRollback.countDownAndWait(); });

    auto future = stdx::async(stdx::launch::async, [&] {
        startedRollback.countDownAndWait();

        // Verify we can take the Client lock during the rollback of the stashed transaction.
        stdx::lock_guard<Client> lk(*opCtx()->getClient());

        finishRollback.countDownAndWait();
    });

    txnParticipant.stashTransactionResources(opCtx());
    txnParticipant.abortTransaction(opCtx());

    future.get();
}

TEST_F(TxnParticipantTest, ThrowDuringOnTransactionPrepareAbortsTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    _opObserver->onTransactionPrepareThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, UnstashFailsShouldLeaveTxnResourceStashUnchanged) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    ASSERT_TRUE(opCtx()->lockState()->isLocked());

    // Simulate the locking of an insert.
    {
        Lock::DBLock dbLock(opCtx(), "test", MODE_IX);
        Lock::CollectionLock collLock(opCtx(), NamespaceString("test.foo"), MODE_IX);
    }

    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    // Simulate a secondary style lock stashing such that the locks are yielded.
    {
        repl::UnreplicatedWritesBlock uwb(opCtx());
        opCtx()->lockState()->unsetMaxLockTimeout();
        txnParticipant.stashTransactionResources(opCtx());
    }
    ASSERT_FALSE(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());

    // Enable fail point.
    getGlobalFailPointRegistry()->getFailPoint("restoreLocksFail")->setMode(FailPoint::alwaysOn);

    ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction"),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // Above unstash attempt fail should leave the txnResourceStash unchanged.
    ASSERT_FALSE(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());

    // Disable fail point.
    getGlobalFailPointRegistry()->getFailPoint("restoreLocksFail")->setMode(FailPoint::off);

    // Should be successfully able to perform lock restore.
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_TRUE(opCtx()->lockState()->isLocked());

    // Commit the transaction to release the locks.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, boost::none);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, StepDownAfterPrepareDoesNotBlock) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    txnParticipant.prepareTransaction(opCtx(), {});

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* opCtx) {
        opCtx->lockState()->lock(opCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);

    txnParticipant.abortTransaction(opCtx());
    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, StepDownAfterPrepareDoesNotBlockThenCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* opCtx) {
        opCtx->lockState()->lock(opCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT(_opObserver->preparedTransactionCommitted);
    ASSERT(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, StepDownDuringAbortSucceeds) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    txnParticipant.abortTransaction(opCtx());
    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedAbortFails) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(txnParticipant.abortTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedCommitFails) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {}),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedAbortReleasesRSTL) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    // Simulate the locking of an insert.
    {
        Lock::DBLock dbLock(opCtx(), "test", MODE_IX);
        Lock::CollectionLock collLock(opCtx(), NamespaceString("test.foo"), MODE_IX);
    }

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(txnParticipant.abortTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* newOpCtx) {
        newOpCtx->lockState()->lock(newOpCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        newOpCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedCommitReleasesRSTL) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    // Simulate the locking of an insert.
    {
        Lock::DBLock dbLock(opCtx(), "test", MODE_IX);
        Lock::CollectionLock collLock(opCtx(), NamespaceString("test.foo"), MODE_IX);
    }

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(
        txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, boost::none),
        AssertionException,
        ErrorCodes::NotWritablePrimary);

    ASSERT_EQ(opCtx()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* newOpCtx) {
        newOpCtx->lockState()->lock(newOpCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        newOpCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedCommitLetsTheAbortAtEntryPointToCleanUp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    _opObserver->onUnpreparedTransactionCommitThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant.commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->unpreparedTransactionCommitted);
    ASSERT_FALSE(txnParticipant.transactionIsAborted());
    ASSERT_FALSE(txnParticipant.transactionIsCommitted());

    // Simulate the abort at entry point.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, ContinuingATransactionWithNoResourcesAborts) {
    // Check out a session, start the transaction and check it in.
    checkOutSession();

    // Check out the session again for a new operation.
    MongoDOperationContextSession sessionCheckout(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, boost::none),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, CannotStartNewTransactionIfNotPrimary) {
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));

    auto opCtxSession = std::make_unique<MongoDOperationContextSession>(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Include 'autocommit=false' for transactions.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, true),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, CannotStartRetryableWriteIfNotPrimary) {
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));

    auto opCtxSession = std::make_unique<MongoDOperationContextSession>(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Omit the 'autocommit' field for retryable writes.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), boost::none, true),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, CannotContinueTransactionIfNotPrimary) {
    // Will start the transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));

    // Technically, the transaction should have been aborted on stepdown anyway, but it
    // doesn't hurt to have this kind of coverage.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, false),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, OlderTransactionFailsOnSessionWithNewerTransaction) {
    // Will start the transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    auto autocommit = false;
    auto startTransaction = true;
    const auto& sessionId = *opCtx()->getLogicalSessionId();

    StringBuilder sb;
    sb << "Cannot start transaction 19 on session " << sessionId
       << " because a newer transaction with txnNumber 20 has already started on this session.";
    ASSERT_THROWS_WHAT(txnParticipant.beginOrContinue(
                           opCtx(), *opCtx()->getTxnNumber() - 1, autocommit, startTransaction),
                       AssertionException,
                       sb.str());
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}


TEST_F(TxnParticipantTest, OldRetryableWriteFailsOnSessionWithNewerTransaction) {
    // Will start the transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    const auto& sessionId = *opCtx()->getLogicalSessionId();

    StringBuilder sb;
    sb << "Retryable write with txnNumber 19 is prohibited on session " << sessionId
       << " because a newer transaction with txnNumber 20 has already started on this session.";
    ASSERT_THROWS_WHAT(txnParticipant.beginOrContinue(
                           opCtx(), *opCtx()->getTxnNumber() - 1, boost::none, boost::none),
                       AssertionException,
                       sb.str());
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}

TEST_F(TxnParticipantTest, CannotStartNewTransactionWhilePreparedTransactionInProgress) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    auto ruPrepareTimestamp = Timestamp();
    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();

        ruPrepareTimestamp = opCtx()->recoveryUnit()->getPrepareTimestamp();
        ASSERT_FALSE(ruPrepareTimestamp.isNull());
    };

    // Check that prepareTimestamp gets set.
    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);

    txnParticipant.stashTransactionResources(opCtx());
    OperationContextSession::checkIn(opCtx());
    {
        auto guard = makeGuard([&]() { OperationContextSession::checkOut(opCtx()); });
        // Try to start a new transaction while there is already a prepared transaction on the
        // session. This should fail with a PreparedTransactionInProgress error.
        runFunctionFromDifferentOpCtx(
            [lsid = *opCtx()->getLogicalSessionId(),
             txnNumberToStart = *opCtx()->getTxnNumber() + 1](OperationContext* newOpCtx) {
                newOpCtx->setLogicalSessionId(lsid);
                newOpCtx->setTxnNumber(txnNumberToStart);
                newOpCtx->setInMultiDocumentTransaction();

                MongoDOperationContextSession ocs(newOpCtx);
                auto txnParticipant = TransactionParticipant::get(newOpCtx);
                ASSERT_THROWS_CODE(
                    txnParticipant.beginOrContinue(newOpCtx, txnNumberToStart, false, true),
                    AssertionException,
                    ErrorCodes::PreparedTransactionInProgress);
            });
    }

    ASSERT_FALSE(txnParticipant.transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotInsertInPreparedTransaction) {
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);

    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx(), "insert"),
                       AssertionException,
                       ErrorCodes::PreparedTransactionInProgress);

    ASSERT_FALSE(txnParticipant.transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotContinueNonExistentTransaction) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, boost::none),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

// Tests that a transaction aborts if it becomes too large based on the server parameter
// 'transactionLimitBytes'.
TEST_F(TxnParticipantTest, TransactionExceedsSizeParameter) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto oldLimit = gTransactionSizeLimitBytes.load();
    ON_BLOCK_EXIT([oldLimit] { gTransactionSizeLimitBytes.store(oldLimit); });

    // Set a limit of 2.5 MB
    gTransactionSizeLimitBytes.store(2 * 1024 * 1024 + 512 * 1024);

    // Two 1MB operations should succeed; three 1MB operations should fail.
    constexpr size_t kBigDataSize = 1 * 1024 * 1024;
    std::unique_ptr<uint8_t[]> bigData(new uint8_t[kBigDataSize]());
    auto operation = repl::OplogEntry::makeInsertOperation(
        kNss,
        _uuid,
        BSON("_id" << 0 << "data" << BSONBinData(bigData.get(), kBigDataSize, BinDataGeneral)));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_THROWS_CODE(txnParticipant.addTransactionOperation(opCtx(), operation),
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
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash, which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());

    {
        // Make it look like we're in a DBDirectClient running a nested operation.
        DirectClientSetter inDirectClient(opCtx());
        txnParticipant.stashTransactionResources(opCtx());

        // The stash was a noop, so the locker, RecoveryUnit, and WriteUnitOfWork on the
        // OperationContext are unaffected.
        ASSERT_EQUALS(originalLocker, opCtx()->lockState());
        ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
        ASSERT(opCtx()->getWriteUnitOfWork());
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
    void cannotSpecifyStartTransactionOnInProgressTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void canSpecifyStartTransactionOnAbortedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.abortTransaction(opCtx());
        ASSERT(txnParticipant.transactionIsAborted());

        txnParticipant.beginOrContinue(
            opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction);
        ASSERT(txnParticipant.transactionIsOpen());
    }

    void cannotSpecifyStartTransactionOnCommittedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
        txnParticipant.commitUnpreparedTransaction(opCtx());

        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void cannotSpecifyStartTransactionOnPreparedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.unstashTransactionResources(opCtx(), "insert");
        auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
        txnParticipant.addTransactionOperation(opCtx(), operation);
        txnParticipant.prepareTransaction(opCtx(), {});

        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void cannotSpecifyStartTransactionOnAbortedPreparedTransaction() {
        auto autocommit = false;
        auto startTransaction = true;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
        txnParticipant.prepareTransaction(opCtx(), {});
        ASSERT(txnParticipant.transactionIsPrepared());

        txnParticipant.abortTransaction(opCtx());
        ASSERT(txnParticipant.transactionIsAborted());

        startTransaction = true;
        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted() {
        MongoDOperationContextSession opCtxSession(opCtx());

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), boost::none, boost::none);
        ASSERT_FALSE(txnParticipant.transactionIsOpen());

        auto autocommit = false;
        auto startTransaction = true;

        txnParticipant.beginOrContinue(
            opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction);

        ASSERT(txnParticipant.transactionIsOpen());
    }
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

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    cannotSpecifyStartTransactionOnInProgressTxn();
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

TEST_F(ShardTxnParticipantTest, canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted) {
    canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnAbortedPreparedTransaction) {
    cannotSpecifyStartTransactionOnAbortedPreparedTransaction();
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

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    cannotSpecifyStartTransactionOnInProgressTxn();
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

TEST_F(ConfigTxnParticipantTest, canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted) {
    canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnAbortedPreparedTransaction) {
    cannotSpecifyStartTransactionOnAbortedPreparedTransaction();
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedOnTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    _opObserver->onTransactionAbortThrowsException = true;

    ASSERT_THROWS_CODE(
        txnParticipant.abortTransaction(opCtx()), AssertionException, ErrorCodes::OperationFailed);
}

DEATH_TEST_F(TxnParticipantTest,
             ThrowDuringPreparedOnTransactionAbortIsFatal,
             "Caught exception during abort of transaction") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    _opObserver->onTransactionAbortThrowsException = true;

    txnParticipant.abortTransaction(opCtx());
}

TEST_F(TxnParticipantTest, InterruptedSessionsCannotBePrepared) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    unittest::Barrier barrier(2);

    auto future = stdx::async(stdx::launch::async, [this, &barrier] {
        ThreadClient tc(getServiceContext());
        auto sideOpCtx = tc->makeOperationContext();
        auto killToken = catalog()->killSession(_sessionId);
        barrier.countDownAndWait();

        auto scopedSession =
            catalog()->checkOutSessionForKill(sideOpCtx.get(), std::move(killToken));
    });

    barrier.countDownAndWait();

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::Interrupted);

    sessionCheckout.reset();
    future.get();
}

TEST_F(TxnParticipantTest, ReacquireLocksForPreparedTransactionsOnStepUp) {
    ASSERT(opCtx()->writesAreReplicated());

    // Prepare a transaction on secondary.
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());

        // Simulate a transaction on secondary.
        repl::UnreplicatedWritesBlock uwb(opCtx());
        ASSERT(!opCtx()->writesAreReplicated());

        txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
        // Simulate the locking of an insert.
        {
            Lock::DBLock dbLock(opCtx(), "test", MODE_IX);
            Lock::CollectionLock collLock(opCtx(), NamespaceString("test.foo"), MODE_IX);
        }
        txnParticipant.prepareTransaction(opCtx(), repl::OpTime({1, 1}, 1));
        txnParticipant.stashTransactionResources(opCtx());
        // Secondary yields locks for prepared transactions.
        ASSERT_FALSE(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());
    }

    // Step-up will restore the locks of prepared transactions.
    ASSERT(opCtx()->writesAreReplicated());
    const auto service = opCtx()->getServiceContext();
    // onStepUp() relies on the storage interface to create the config.transactions table.
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    MongoDSessionCatalog::onStepUp(opCtx());
    {
        auto sessionCheckout = checkOutSession({});
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());
        txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
        txnParticipant.abortTransaction(opCtx());
    }
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
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforePrepareCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalCommittedOnCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    unsigned long long beforeCommitCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalCommitted();

    txnParticipant.commitUnpreparedTransaction(opCtx());

    // Assert that the committed counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalCommitted(), beforeCommitCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalPreparedThenCommitted) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    unsigned long long beforePreparedThenCommittedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforePreparedThenCommittedCount + 1U);
}


TEST_F(TransactionsMetricsTest, IncrementTotalAbortedUponAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    unsigned long long beforeAbortCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    txnParticipant.abortTransaction(opCtx());

    // Assert that the aborted counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalPreparedThenAborted) {
    unsigned long long beforePreparedThenAbortedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforePreparedThenAbortedCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementCurrentPreparedWithCommit) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(),
              beforeCurrentPrepared + 1U);
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
}

TEST_F(TransactionsMetricsTest, IncrementCurrentPreparedWithAbort) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(),
              beforeCurrentPrepared + 1U);
    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
}

TEST_F(TransactionsMetricsTest, NoPreparedMetricsChangesAfterExceptionInPrepare) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();
    unsigned long long beforeTotalPrepared =
        ServerTransactionsMetrics::get(opCtx())->getTotalPrepared();
    unsigned long long beforeTotalPreparedThenCommitted =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();
    unsigned long long beforeTotalPreparedThenAborted =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    _opObserver->onTransactionPrepareThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforeTotalPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforeTotalPreparedThenCommitted);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforeTotalPreparedThenAborted);

    if (txnParticipant.transactionIsOpen())
        txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforeTotalPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforeTotalPreparedThenCommitted);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforeTotalPreparedThenAborted);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithAbort) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that aborting a transaction decrements the open transactions counter by 1.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithCommit) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // Tests that committing a transaction decrements the open transactions counter by 1.
    txnParticipant.commitUnpreparedTransaction(opCtx());
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
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that the second unstash increments the active counter and decrements the inactive
    // counter.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that committing a transaction decrements the active counter only.
    txnParticipant.commitUnpreparedTransaction(opCtx());
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
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that aborting a stashed transaction decrements the inactive counter only.
    txnParticipant.abortTransaction(opCtx());
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
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that aborting a stashed transaction decrements the active counter only.
    txnParticipant.abortTransaction(opCtx());
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

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforePrepareCount + 1U);

    // Tests that the first stash decrements the active counter and increments the inactive counter.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter + 1U);

    // Tests that unstashing increments the active counter and decrements the inactive counter.
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);

    // Tests that committing decrements the active counter only.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});

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
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);

    // Tests that stashing a prepared transaction decrements the active counter and increments the
    // inactive counter.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter + 1U);

    // Tests that aborting a stashed prepared transaction decrements the inactive counter only.
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
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
    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                           opCtx(), *opCtx()->getTxnNumber(), autocommit, startTransaction),
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
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Advance the clock.
    tickSource->advance(Microseconds(100));

    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsPreparedDurationShouldBeSetUponCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Advance the clock.
    tickSource->advance(Microseconds(10));

    // Prepare the transaction and extend the duration in the prepared state.
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    tickSource->advance(Microseconds(100));

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // Advance the clock.
    tickSource->advance(Microseconds(100));

    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsPreparedDurationShouldBeSetUponAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // Advance the clock.
    tickSource->advance(Microseconds(10));

    // Prepare the transaction and extend the duration in the prepared state.
    txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    tickSource->advance(Microseconds(100));

    // The transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Commit the transaction and check duration.
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));

    // The transaction committed, so the duration shouldn't have increased even if more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest,
       SingleTransactionStatsPreparedDurationShouldKeepIncreasingUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Prepare the transaction and extend the duration in the prepared state.
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    // The prepared transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Commit the prepared transaction and check the prepared duration.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The prepared transaction committed, so the prepared duration shouldn't have increased even if
    // more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    tickSource->advance(Microseconds(100));

    // The transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Abort the transaction and check duration.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));

    // The transaction aborted, so the duration shouldn't have increased even if more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest,
       SingleTransactionStatsPreparedDurationShouldKeepIncreasingUntilAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Prepare the transaction and extend the duration in the prepared state.
    txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    // The prepared transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Abort the prepared transaction and check the prepared duration.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The prepared transaction aborted, so the prepared duration shouldn't have increased even if
    // more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndStash) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    // Time active should have increased only during active period.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    txnParticipant.stashTransactionResources(opCtx());

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    // Start a new transaction.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    txnParticipant.beginOrContinue(opCtx(), higherTxnNum, false, true);

    // Time active should be zero for a new transaction.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    txnParticipant.abortTransaction(opCtx());

    // Time active should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // The transaction is not active after abort, so time active should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetUponAbortOnly) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    txnParticipant.abortTransaction(opCtx());

    // Time active should still be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilStash) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));

    // Time active should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is no longer active, so time active should have stopped increasing.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    tickSource->advance(Microseconds(100));

    // Time active should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    txnParticipant.commitUnpreparedTransaction(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is no longer active, so time active should have stopped increasing.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponStash) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysExamined =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 5;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.docsExamined =
        2;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 0;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nMatched = 3;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 4;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysInserted =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysDeleted = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.prepareReadConflicts.store(5);
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts.store(4);

    auto additiveMetricsToCompare =
        txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT(txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysExamined =
        3;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 2;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.docsExamined =
        0;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 2;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nMatched = 4;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nModified = 5;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 1;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.ndeleted = 4;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 0;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysInserted =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.prepareReadConflicts.store(0);
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts.store(0);
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.writeConflicts.store(6);
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts.store(3);

    auto additiveMetricsToCompare =
        txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    ASSERT(txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysExamined =
        2;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 4;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.docsExamined =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 3;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nMatched = 2;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nModified = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 5;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysInserted =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysDeleted = 6;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.writeConflicts.store(3);
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts.store(3);

    auto additiveMetricsToCompare =
        txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.abortTransaction(opCtx());

    ASSERT(txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndStash) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should have increased.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    // Time inactive should have increased again.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    tickSource->advance(Microseconds(100));

    // The transaction is currently active, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is inactive again, so time inactive should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{300});
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    tickSource->advance(Microseconds(100));

    // Time inactive should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.abortTransaction(opCtx());

    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // The transaction has aborted, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldIncreaseUntilCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    tickSource->advance(Microseconds(100));

    // Time inactive should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction has committed, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
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

    // Create a ClientMetadata object and set it.
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
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT(opCtx()->getWriteUnitOfWork());
    ASSERT(opCtx()->lockState()->isLocked());

    // Prepare the transaction and extend the duration in the prepared state.
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});
    const long preparedDuration = 10;
    tickSource->advance(Microseconds(preparedDuration));

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Verify that the Session's report of its own stashed state aligns with our expectations.
    auto stashedState = txnParticipant.reportStashedState(opCtx());
    auto transactionDocument = stashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(stashedState.getField("host").valueStringData().toString(),
              getHostNameCachedAndPort());
    ASSERT_EQ(stashedState.getField("desc").valueStringData().toString(), "inactive transaction");
    ASSERT_BSONOBJ_EQ(stashedState.getField("lsid").Obj(), _sessionId.toBSON());
    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), _txnNumber);
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
        startTime + Seconds(gTransactionLifetimeLimitSeconds.load()));
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
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT(opCtx()->getWriteUnitOfWork());

    // With the resources unstashed, verify that the Session reports an empty stashed state.
    ASSERT(txnParticipant.reportStashedState(opCtx()).isEmpty());

    // Commit the transaction. This allows us to release locks.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
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
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT(opCtx()->getWriteUnitOfWork());
    ASSERT(opCtx()->lockState()->isLocked());

    // Prepare transaction and extend duration in the prepared state.
    txnParticipant.prepareTransaction(opCtx(), {});
    const long prepareDuration = 10;
    tickSource->advance(Microseconds(prepareDuration));

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant.reportUnstashedState(opCtx(), &unstashedStateBuilder);
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
        startTime + Seconds(gTransactionLifetimeLimitSeconds.load()));
    ASSERT_EQ(transactionDocument.getField("timePreparedMicros").numberLong(), prepareDuration);

    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that
    // the values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // With the resources stashed, verify that the Session reports an empty unstashed state.
    BSONObjBuilder builder;
    txnParticipant.reportUnstashedState(opCtx(), &builder);
    ASSERT(builder.obj().isEmpty());
}

TEST_F(TransactionsMetricsTest, ReportUnstashedResourcesForARetryableWrite) {
    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), boost::none, boost::none);
    txnParticipant.unstashTransactionResources(opCtx(), "find");

    // Build a BSONObj containing the details which we expect to see reported when we invoke
    // reportUnstashedState. For a retryable write, we should only include the txnNumber.
    BSONObjBuilder reportBuilder;
    BSONObjBuilder transactionBuilder(reportBuilder.subobjStart("transaction"));
    BSONObjBuilder parametersBuilder(transactionBuilder.subobjStart("parameters"));
    parametersBuilder.append("txnNumber", *opCtx()->getTxnNumber());
    parametersBuilder.done();
    transactionBuilder.done();

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant.reportUnstashedState(opCtx(), &unstashedStateBuilder);
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
    // Create a ClientMetadata object and set it.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());

    // Create another ClientMetadata object.
    auto newObj = constructClientMetadata("newAppName");
    auto newClientMetadata = ClientMetadata::parse(newObj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(newClientMetadata.getValue()));

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());

    // LastClientInfo's clientMetadata should have been updated to the new ClientMetadata object.
    lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.appName, "newAppName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, newObj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponCommit) {
    // Create a ClientMetadata object and set it.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponAbort) {
    // Create a ClientMetadata object and set it.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.abortTransaction(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
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
    CurOp::get(opCtx)->debug().additiveMetrics.keysInserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysDeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.prepareReadConflicts.store(metricValue);
    CurOp::get(opCtx)->debug().additiveMetrics.writeConflicts.store(metricValue);
}

/*
 * Builds expected parameters info string.
 */
void buildParametersInfoString(StringBuilder* sb,
                               LogicalSessionId sessionId,
                               const TxnNumber txnNum,
                               const repl::ReadConcernArgs readConcernArgs,
                               bool autocommitVal) {
    BSONObjBuilder lsidBuilder;
    sessionId.serialize(&lsidBuilder);
    auto autocommitString = autocommitVal ? "true" : "false";
    (*sb) << "parameters:{ lsid: " << lsidBuilder.done().toString() << ", txnNumber: " << txnNum
          << ", autocommit: " << autocommitString
          << ", readConcern: " << readConcernArgs.toBSON().getObjectField("readConcern") << " },";
}

/*
 * Builds expected single transaction stats info string.
 */
void buildSingleTransactionStatsString(StringBuilder* sb, const int metricValue) {
    (*sb) << " keysExamined:" << metricValue << " docsExamined:" << metricValue
          << " nMatched:" << metricValue << " nModified:" << metricValue
          << " ninserted:" << metricValue << " ndeleted:" << metricValue
          << " keysInserted:" << metricValue << " keysDeleted:" << metricValue
          << " prepareReadConflicts:" << metricValue << " writeConflicts:" << metricValue;
}

/*
 * Builds the time active and time inactive info string.
 */
void buildTimeActiveInactiveString(StringBuilder* sb,
                                   TransactionParticipant::Participant txnParticipant,
                                   TickSource* tickSource,
                                   TickSource::Tick curTick) {
    // Add time active micros to string.
    (*sb) << " timeActiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(tickSource,
                                                                                       curTick));

    // Add time inactive micros to string.
    (*sb) << " timeInactiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(tickSource,
                                                                                         curTick));
}

/*
 * Builds the total prepared duration info string.
 */
void buildPreparedDurationString(StringBuilder* sb,
                                 TransactionParticipant::Participant txnParticipant,
                                 TickSource* tickSource,
                                 TickSource::Tick curTick) {
    (*sb) << " totalPreparedDurationMicros:"
          << durationCount<Microseconds>(
                 txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(tickSource,
                                                                                       curTick));
}

/*
 * Builds the entire expected transaction info string and returns it.
 */
std::string buildTransactionInfoString(OperationContext* opCtx,
                                       TransactionParticipant::Participant txnParticipant,
                                       std::string terminationCause,
                                       const LogicalSessionId sessionId,
                                       const TxnNumber txnNum,
                                       const int metricValue,
                                       const bool wasPrepared,
                                       bool autocommitVal = false,
                                       boost::optional<repl::OpTime> prepareOpTime = boost::none) {
    // Calling transactionInfoForLog to get the actual transaction info string.
    const auto lockerInfo =
        opCtx->lockState()->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase());
    // Building expected transaction info string.
    StringBuilder parametersInfo;
    // autocommit must be false for a multi statement transaction, so
    // getTransactionInfoForLogForTest should theoretically always print false. In certain unit
    // tests, we compare its output to the output generated in this function.
    //
    // Since we clear the state of a transaction on abort, if getTransactionInfoForLogForTest is
    // called after a transaction is already aborted, it will encounter boost::none for the
    // autocommit value. In that case, it will print out true.
    //
    // In cases where we call getTransactionInfoForLogForTest after aborting a transaction
    // and check if the output matches this function's output, we must explicitly set autocommitVal
    // to true.
    buildParametersInfoString(
        &parametersInfo, sessionId, txnNum, repl::ReadConcernArgs::get(opCtx), autocommitVal);

    StringBuilder readTimestampInfo;
    readTimestampInfo
        << " readTimestamp:"
        << txnParticipant.getSingleTransactionStatsForTest().getReadTimestamp().toString() << ",";

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
    // nModified:1 ninserted:1 ndeleted:1 keysInserted:1 keysDeleted:1
    // prepareReadConflicts:1 writeConflicts:1 terminationCause:committed timeActiveMicros:3
    // timeInactiveMicros:2 numYields:0 locks:{ Global: { acquireCount: { r: 6, w: 4 } }, Database:
    // { acquireCount: { r: 1, w: 1, W: 2 } }, Collection: { acquireCount: { R: 1 } }, oplog: {
    // acquireCount: { W: 1 } } } wasPrepared:1 totalPreparedDurationMicros:10
    // prepareOpTime:<OpTime> 0ms
    StringBuilder expectedTransactionInfo;
    expectedTransactionInfo << parametersInfo.str() << readTimestampInfo.str()
                            << singleTransactionStatsInfo.str()
                            << " terminationCause:" << terminationCause
                            << timeActiveAndInactiveInfo.str() << " numYields:" << 0
                            << " locks:" << locks.done().toString()
                            << " wasPrepared:" << wasPrepared;
    if (wasPrepared) {
        StringBuilder totalPreparedDuration;
        buildPreparedDurationString(
            &totalPreparedDuration, txnParticipant, tickSource, tickSource->getTicks());
        expectedTransactionInfo << totalPreparedDuration.str();
        expectedTransactionInfo << " prepareOpTime:"
                                << (prepareOpTime ? prepareOpTime->toString()
                                                  : txnParticipant.getPrepareOpTime().toString());
    }

    expectedTransactionInfo << ", "
                            << duration_cast<Milliseconds>(
                                   txnParticipant.getSingleTransactionStatsForTest().getDuration(
                                       tickSource, tickSource->getTicks()));
    return expectedTransactionInfo.str();
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterCommit) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));

    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo->stats, true, readConcernArgs);

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
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));

    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Prepare the transaction and extend the duration in the prepared state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    tickSource->advance(Microseconds(10));

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo->stats, true, readConcernArgs);

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
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.abortTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo->stats, false, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   false,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestPreparedTransactionInfoForLogAfterAbort) {
    auto tickSource = initMockTickSource();

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Prepare the transaction and extend the duration in the prepared state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(10));

    txnParticipant.abortTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo->stats, false, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   true,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

DEATH_TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogWithNoLockerInfoStats, "invariant") {
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());

    txnParticipant.getTransactionInfoForLogForTest(opCtx(), nullptr, true, readConcernArgs);
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant.getTransactionInfoForLogForTest(
            opCtx(), &lockerInfo->stats, true, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogPreparedTransactionInfoAfterSlowCommit) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    startCapturingLogMessages();
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant.getTransactionInfoForLogForTest(
            opCtx(), &lockerInfo->stats, true, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.abortTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   false);

    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogPreparedTransactionInfoAfterSlowAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));
    auto prepareOpTime = txnParticipant.getPrepareOpTime();

    startCapturingLogMessages();
    txnParticipant.abortTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   true,
                                   false,
                                   prepareOpTime);

    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterExceptionInPrepare) {
    auto tickSource = initMockTickSource();
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    _opObserver->onTransactionPrepareThrowsException = true;

    startCapturingLogMessages();
    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant.transactionIsAborted());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue,
                                   false);

    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowStashedAbort) {
    auto tickSource = initMockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    txnParticipant.stashTransactionResources(opCtx());
    const auto txnResourceStashLocker = txnParticipant.getTxnResourceStashLockerForTest();
    ASSERT(txnResourceStashLocker);
    const auto lockerInfo = txnResourceStashLocker->getLockerInfo(boost::none);

    serverGlobalParams.slowMS = 10;
    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.abortTransaction(opCtx());
    stopCapturingLogMessages();

    std::string expectedTransactionInfo = "transaction parameters";
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoVerbosityInfo) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Set a high slow operation threshold to avoid the transaction being logged as slow.
    serverGlobalParams.slowMS = 10000;

    // Set verbosity level of transaction components to info.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Info());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    // Test that the transaction is not logged.
    ASSERT_EQUALS(0, countLogLinesContaining("transaction parameters"));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoVerbosityDebug) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Set a high slow operation threshold to avoid the transaction being logged as slow.
    serverGlobalParams.slowMS = 10000;

    // Set verbosity level of transaction components to debug.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Debug(1));

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    // Reset verbosity level of transaction components.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Info());

    // Test that the transaction is still logged.
    ASSERT_EQUALS(1, countLogLinesContaining("transaction parameters"));
}

TEST_F(TxnParticipantTest, RollbackResetsInMemoryStateOfPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Perform an insert as a part of a transaction so that we have a transaction operation.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());

    auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_FALSE(txnParticipant.transactionIsAborted());

    // Make sure the state of txnParticipant is populated correctly after a prepared transaction.
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getTransactionOperationsForTest().size(), 1U);
    ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), prepareTimestamp);
    ASSERT_NE(txnParticipant.getActiveTxnNumber(), kUninitializedTxnNumber);

    txnParticipant.abortTransaction(opCtx());
    txnParticipant.invalidate(opCtx());

    // After calling abortTransaction and invalidate, the state of txnParticipant should be
    // invalidated.
    ASSERT_FALSE(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getTransactionOperationsForTest().size(), 0U);
    ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), Timestamp());
    ASSERT_EQ(txnParticipant.getActiveTxnNumber(), kUninitializedTxnNumber);
}

TEST_F(TxnParticipantTest, PrepareTransactionAsSecondarySetsThePrepareOpTime) {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    // If _prepareOptime was not set and was null, then commitPreparedTransaction would falsely
    // succeed everytime. We set the commitTimestamp to be less than the prepareTimestamp to make
    // sure this is not the case.
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() - 1);
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitPreparedTransactionAsSecondarySetsTheFinishOpTime) {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Simulate committing a prepared transaction on a secondary.
    repl::UnreplicatedWritesBlock uwb(opCtx());
    ASSERT(!opCtx()->writesAreReplicated());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    const auto commitOplogEntryOpTime = repl::OpTime({10, 10}, 0);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, commitOplogEntryOpTime);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

DEATH_TEST_F(TxnParticipantTest,
             CommitPreparedTransactionAsSecondaryWithNullCommitOplogEntryOpTimeShouldFail,
             "invariant") {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Simulate committing a prepared transaction on a secondary.
    repl::UnreplicatedWritesBlock uwb(opCtx());
    ASSERT(!opCtx()->writesAreReplicated());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {});
}

DEATH_TEST_F(TxnParticipantTest,
             CommitPreparedTransactionAsPrimaryWithNonNullCommitOplogEntryOpTimeShouldFail,
             "invariant") {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    const auto commitOplogEntryOpTime = repl::OpTime({10, 10}, 0);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, commitOplogEntryOpTime);
}

TEST_F(TxnParticipantTest, AbortTransactionOnSessionCheckoutWithoutRefresh) {
    // This test is intended to mimic the behavior in applyAbortTransaction from
    // transaction_oplog_application.cpp. This is to ensure that when secondaries see an abort oplog
    // entry for a non-existent transaction, the state of the transaction is set to aborted.

    // Simulate aborting a transaction on a secondary.
    repl::UnreplicatedWritesBlock uwb(opCtx());
    ASSERT(!opCtx()->writesAreReplicated());

    const auto txnNumber = *opCtx()->getTxnNumber();

    // MongoDOperationContextSessionWithoutRefresh will begin a new transaction with txnNumber
    // unconditionally since the participant's _activeTxnNumber is kUninitializedTxnNumber at time
    // of session checkout.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx());

    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsOpen());
    ASSERT_EQ(txnParticipant.getActiveTxnNumber(), txnNumber);

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, ResponseMetadataHasHasReadOnlyFalseIfNothingInProgress) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());
}

TEST_F(TxnParticipantTest, ResponseMetadataHasReadOnlyFalseIfInRetryableWrite) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());

    // Start a retryable write.
    txnParticipant.beginOrContinue(opCtx(),
                                   *opCtx()->getTxnNumber(),
                                   boost::none /* autocommit */,
                                   boost::none /* startTransaction */);
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());
}

TEST_F(TxnParticipantTest, ResponseMetadataHasReadOnlyTrueIfInProgressAndOperationsVectorEmpty) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());

    // Start a transaction.
    txnParticipant.beginOrContinue(
        opCtx(), *opCtx()->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getReadOnly());

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getReadOnly());
}

TEST_F(TxnParticipantTest,
       ResponseMetadataHasReadOnlyFalseIfInProgressAndOperationsVectorNotEmpty) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());

    // Start a transaction.
    txnParticipant.beginOrContinue(
        opCtx(), *opCtx()->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getReadOnly());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getReadOnly());

    // Simulate an insert.
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, _uuid, BSON("TestValue" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());
}

TEST_F(TxnParticipantTest, ResponseMetadataHasReadOnlyFalseIfAborted) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());

    // Start a transaction.
    txnParticipant.beginOrContinue(
        opCtx(), *opCtx()->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getReadOnly());

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getReadOnly());

    txnParticipant.abortTransaction(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getReadOnly());
}

TEST_F(TxnParticipantTest, OldestActiveTransactionTimestamp) {
    auto nss = NamespaceString::kSessionTransactionsTableNamespace;

    auto insertTxnRecord = [&](unsigned i) {
        Timestamp ts(1, i);
        SessionTxnRecord record;
        record.setStartOpTime(repl::OpTime(ts, 0));
        record.setState(DurableTxnStateEnum::kPrepared);
        record.setSessionId(makeLogicalSessionIdForTest());
        record.setTxnNum(1);
        record.setLastWriteOpTime(repl::OpTime(ts, 0));
        record.setLastWriteDate(Date_t::now());

        AutoGetOrCreateDb autoDb(opCtx(), nss.db(), MODE_X);
        WriteUnitOfWork wuow(opCtx());
        auto coll = autoDb.getDb()->getCollection(opCtx(), nss);
        ASSERT(coll);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(
            coll->insertDocument(opCtx(), InsertStatement(record.toBSON()), nullOpDebug, false));
        wuow.commit();
    };

    auto deleteTxnRecord = [&](unsigned i) {
        Timestamp ts(1, i);
        AutoGetOrCreateDb autoDb(opCtx(), nss.db(), MODE_X);
        WriteUnitOfWork wuow(opCtx());
        auto coll = autoDb.getDb()->getCollection(opCtx(), nss);
        ASSERT(coll);
        auto cursor = coll->getCursor(opCtx());
        while (auto record = cursor->next()) {
            auto bson = record.get().data.toBson();
            if (bson["state"].String() != "prepared"_sd) {
                continue;
            }

            if (bson["startOpTime"]["ts"].timestamp() == ts) {
                coll->deleteDocument(opCtx(), kUninitializedStmtId, record->id, nullptr);
                wuow.commit();
                return;
            }
        }
        FAIL(str::stream() << "No prepared transaction with start timestamp (1, " << i << ")");
    };

    auto oldestActiveTransactionTS = [&]() {
        return TransactionParticipant::getOldestActiveTimestamp(Timestamp()).getValue();
    };

    auto assertOldestActiveTS = [&](boost::optional<unsigned> i) {
        if (i.has_value()) {
            ASSERT_EQ(Timestamp(1, i.value()), oldestActiveTransactionTS());
        } else {
            ASSERT_EQ(boost::none, oldestActiveTransactionTS());
        }
    };

    assertOldestActiveTS(boost::none);
    insertTxnRecord(1);
    assertOldestActiveTS(1);
    insertTxnRecord(2);
    assertOldestActiveTS(1);
    deleteTxnRecord(1);
    assertOldestActiveTS(2);
    deleteTxnRecord(2);
    assertOldestActiveTS(boost::none);

    // Add a newer transaction, then an older one, to test that order doesn't matter.
    insertTxnRecord(4);
    insertTxnRecord(3);
    assertOldestActiveTS(3);
    deleteTxnRecord(4);
    assertOldestActiveTS(3);
    deleteTxnRecord(3);
    assertOldestActiveTS(boost::none);
};

TEST_F(TxnParticipantTest, OldestActiveTransactionTimestampTimeout) {
    // Block getOldestActiveTimestamp() by locking the config database.
    auto nss = NamespaceString::kSessionTransactionsTableNamespace;
    AutoGetOrCreateDb autoDb(opCtx(), nss.db(), MODE_X);
    auto statusWith = TransactionParticipant::getOldestActiveTimestamp(Timestamp());
    ASSERT_FALSE(statusWith.isOK());
    ASSERT_TRUE(ErrorCodes::isInterruption(statusWith.getStatus().code()));
};

TEST_F(TxnParticipantTest, ExitPreparePromiseIsFulfilledOnAbortAfterPrepare) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.beginOrContinue(
        opCtx(), *opCtx()->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    const auto exitPrepareFuture = txnParticipant.onExitPrepare();
    ASSERT_FALSE(exitPrepareFuture.isReady());

    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(exitPrepareFuture.isReady());

    // Once the promise has been fulfilled, new callers of onExitPrepare should immediately be
    // ready.
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    // abortTransaction is retryable, but does not cause the completion promise to be set again.
    txnParticipant.abortTransaction(opCtx());
}

TEST_F(TxnParticipantTest, ExitPreparePromiseIsFulfilledOnCommitAfterPrepare) {
    MongoDOperationContextSession opCtxSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.beginOrContinue(
        opCtx(), *opCtx()->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    const auto exitPrepareFuture = txnParticipant.onExitPrepare();
    ASSERT_FALSE(exitPrepareFuture.isReady());

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {});
    ASSERT_TRUE(exitPrepareFuture.isReady());

    // Once the promise has been fulfilled, new callers of onExitPrepare should immediately be
    // ready.
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());
}

}  // namespace
}  // namespace mongo
