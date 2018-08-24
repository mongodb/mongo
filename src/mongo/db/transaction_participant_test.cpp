/**
 *    Copyright (C) 2018 MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_session_mongod.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/socket_utils.h"

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
    stdx::function<void()> onTransactionPrepareFn = [this]() { transactionPrepared = true; };

    void onTransactionCommit(OperationContext* opCtx, bool wasPrepared) override;
    bool onTransactionCommitThrowsException = false;
    bool transactionCommitted = false;
    stdx::function<void(bool)> onTransactionCommitFn = [this](bool wasPrepared) {
        transactionCommitted = true;
    };
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

void OpObserverMock::onTransactionCommit(OperationContext* opCtx, bool wasPrepared) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onTransactionCommit(opCtx, wasPrepared);
    uassert(ErrorCodes::OperationFailed,
            "onTransactionCommit() failed",
            !onTransactionCommitThrowsException);
    transactionCommitted = true;
    onTransactionCommitFn(wasPrepared);
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

        auto service = opCtx()->getServiceContext();
        SessionCatalog::get(service)->onStepUp(opCtx());

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
        // Clear all sessions to free up any stashed resources.
        SessionCatalog::get(opCtx()->getServiceContext())->reset_forTest();

        MockReplCoordServerFixture::tearDown();
        _opObserver = nullptr;
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
            ASSERT_LT(session->getActiveTxnNumber(), newTxnNum);

            // Check that the transaction has some operations, so we can ensure they are cleared.
            ASSERT_GT(txnParticipant->transactionOperationsForTest().size(), 0u);

            // Bump the active transaction number on the txnParticipant. This should clear all state
            // from the previous transaction.
            session->beginOrContinueTxn(opCtx, newTxnNum);
            ASSERT_EQ(session->getActiveTxnNumber(), newTxnNum);

            txnParticipant->checkForNewTxnNumber();
            ASSERT_FALSE(txnParticipant->transactionIsAborted());
            ASSERT_EQ(txnParticipant->transactionOperationsForTest().size(), 0u);
        };

        runFunctionFromDifferentOpCtx(func);
    }

    OpObserverMock* _opObserver = nullptr;
    LogicalSessionId _sessionId;
    TxnNumber _txnNumber;
};

// Test that transaction lock acquisition times out in `maxTransactionLockRequestTimeoutMillis`
// milliseconds.
TEST_F(TxnParticipantTest, TransactionThrowsLockTimeoutIfLockIsUnavailable) {
    const std::string dbName = "TestDB";

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

        OperationContextSessionMongod newOpCtxSession(newOpCtx.get(), true, false, true);

        auto newTxnParticipant = TransactionParticipant::get(newOpCtx.get());
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

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);


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

TEST_F(TxnParticipantTest, ReportStashedResources) {
    Date_t startTime = Date_t::now();
    const bool autocommit = false;

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    OperationContextSessionMongod opCtxSession(opCtx(), true, autocommit, true);

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
    ASSERT_GTE(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        Date_t::fromMillisSinceEpoch(txnParticipant->getSingleTransactionStats().getStartTime() /
                                     1000) +
            stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()});

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
    txnParticipant->commitUnpreparedTransaction(opCtx());
}

TEST_F(TxnParticipantTest, ReportUnstashedResources) {
    Date_t startTime = Date_t::now();
    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    const auto autocommit = false;
    OperationContextSessionMongod opCtxSession(opCtx(), true, autocommit, true);

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

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant->reportUnstashedState(repl::ReadConcernArgs::get(opCtx()),
                                         &unstashedStateBuilder);
    auto unstashedState = unstashedStateBuilder.obj();
    auto transactionDocument = unstashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), *opCtx()->getTxnNumber());
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_GTE(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        Date_t::fromMillisSinceEpoch(txnParticipant->getSingleTransactionStats().getStartTime() /
                                     1000) +
            stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()});

    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that the
    // values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant->stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // With the resources stashed, verify that the Session reports an empty unstashed state.
    BSONObjBuilder builder;
    txnParticipant->reportUnstashedState(repl::ReadConcernArgs::get(opCtx()), &builder);
    ASSERT(builder.obj().isEmpty());
}

TEST_F(TxnParticipantTest, ReportUnstashedResourcesForARetryableWrite) {
    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    OperationContextSessionMongod opCtxSession(opCtx(), true, boost::none, boost::none);
    auto txnParticipant = TransactionParticipant::get(opCtx());
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
    txnParticipant->reportUnstashedState(repl::ReadConcernArgs::get(opCtx()),
                                         &unstashedStateBuilder);
    ASSERT_BSONOBJ_EQ(unstashedStateBuilder.obj(), reportBuilder.obj());
}

TEST_F(TxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    // Must specify startTransaction=true and autocommit=false to start a transaction.
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant->inMultiDocumentTransaction());

    // Cannot try to start a transaction that already started.
    ASSERT_THROWS_CODE(txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, true),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, AutocommitRequiredOnEveryTxnOp) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'autocommit=true' is not allowed and should crash.
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), true, boost::none);
}

DEATH_TEST_F(TxnParticipantTest, StartTransactionCannotBeFalse, "invariant") {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'startTransaction=false' is not allowed and should crash.
    txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, false);
}

TEST_F(TxnParticipantTest, SameTransactionPreservesStoredStatements) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->commitUnpreparedTransaction(opCtx());
    txnParticipant->stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
}

TEST_F(TxnParticipantTest, CommitTransactionSetsCommitTimestampOnPreparedTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

    Timestamp actualCommitTimestamp;
    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](bool wasPrepared) {
        originalFn(wasPrepared);
        ASSERT(wasPrepared);
        actualCommitTimestamp = opCtx()->recoveryUnit()->getCommitTimestamp();
    };

    const auto commitTimestamp = Timestamp(6, 6);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->prepareTransaction(opCtx());
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);

    ASSERT_EQ(commitTimestamp, actualCommitTimestamp);
    // The recovery unit is reset on commit.
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());

    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithCommitTimestampFailsOnUnpreparedTransaction) {
    const auto commitTimestamp = Timestamp(6, 6);

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionDoesNotSetCommitTimestampOnUnpreparedTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

    Timestamp actualCommitTimestamp;
    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](bool wasPrepared) {
        originalFn(wasPrepared);
        ASSERT_FALSE(wasPrepared);
        actualCommitTimestamp = opCtx()->recoveryUnit()->getCommitTimestamp();
    };

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->commitUnpreparedTransaction(opCtx());

    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
    ASSERT(actualCommitTimestamp.isNull());

    txnParticipant->stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant->transactionIsCommitted());
    ASSERT(opCtx()->recoveryUnit()->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithoutCommitTimestampFailsOnPreparedTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->prepareTransaction(opCtx());
    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionWithNullCommitTimestampFailsOnPreparedTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    txnParticipant->prepareTransaction(opCtx());
    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(opCtx(), Timestamp()),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyTransactionAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());
    txnParticipant->abortArbitraryTransaction();
    ASSERT_TRUE(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, ConcurrencyOfUnstashAndAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // An unstash after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "find"),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, ConcurrencyOfUnstashAndMigration) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "find");

    // The transaction may be aborted without checking out the txnParticipant->
    txnParticipant->abortArbitraryTransaction();

    // A stash after an abort should be a noop.
    txnParticipant->stashTransactionResources(opCtx());
}

TEST_F(TxnParticipantTest, ConcurrencyOfStashAndMigration) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

TEST_F(TxnParticipantTest, ConcurrencyOfCommitTransactionAndAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();

    // An commitPreparedTransaction() after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, ConcurrencyOfActiveAbortAndArbitraryAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

TEST_F(TxnParticipantTest, ConcurrencyOfActiveAbortAndMigration) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    ASSERT(txnParticipant->inMultiDocumentTransaction());

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // The operation throws for some reason and aborts implicitly.
    // Abort active transaction after it's been aborted by migration is a no-op.
    txnParticipant->abortActiveTransaction(opCtx());

    // The session's state is None after migration, but we should have cleared
    // the states of opCtx.
    ASSERT(opCtx()->getWriteUnitOfWork() == nullptr);
}

TEST_F(TxnParticipantTest, ConcurrencyOfPrepareTransactionAndAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    // The transaction may be aborted without checking out the txnParticipant.
    txnParticipant->abortArbitraryTransaction();
    ASSERT(txnParticipant->transactionIsAborted());

    // A prepareTransaction() after an abort should uassert.
    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, KillSessionsDuringPrepareDoesNotAbortTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx());
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);
    ASSERT(_opObserver->transactionPrepared);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
}

DEATH_TEST_F(TxnParticipantTest, AbortDuringPrepareIsFatal, "Fatal assertion 50906") {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    auto originalFn = _opObserver->onTransactionPrepareFn;
    _opObserver->onTransactionPrepareFn = [&]() {
        originalFn();

        // The transaction may be aborted without checking out the txnParticipant.
        txnParticipant->abortActiveTransaction(opCtx());
        ASSERT(txnParticipant->transactionIsAborted());
    };

    txnParticipant->prepareTransaction(opCtx());
}

TEST_F(TxnParticipantTest, ThrowDuringOnTransactionPrepareAbortsTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    _opObserver->onTransactionPrepareThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant->transactionIsAborted());
}

TEST_F(TxnParticipantTest, KillSessionsDuringPreparedCommitDoesNotAbortTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    const auto commitTimestamp = Timestamp(1, 1);
    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](bool wasPrepared) {
        originalFn(wasPrepared);
        ASSERT(wasPrepared);

        // The transaction may be aborted without checking out the txnParticipant.
        txnParticipant->abortArbitraryTransaction();
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    };

    txnParticipant->prepareTransaction(opCtx());
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);

    ASSERT(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(txnParticipant->transactionIsCommitted());
}

// This tests documents behavior, though it is not necessarily the behavior we want.
TEST_F(TxnParticipantTest, AbortDuringPreparedCommitDoesNotAbortTransaction) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    const auto commitTimestamp = Timestamp(1, 1);
    auto originalFn = _opObserver->onTransactionCommitFn;
    _opObserver->onTransactionCommitFn = [&](bool wasPrepared) {
        originalFn(wasPrepared);
        ASSERT(wasPrepared);

        // The transaction may be aborted without checking out the txnParticipant.
        auto func = [&](OperationContext* opCtx) { txnParticipant->abortArbitraryTransaction(); };
        runFunctionFromDifferentOpCtx(func);
        ASSERT_FALSE(txnParticipant->transactionIsAborted());
    };

    txnParticipant->prepareTransaction(opCtx());
    txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp);

    ASSERT(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(txnParticipant->transactionIsCommitted());
}

// This tests documents behavior, though it is not necessarily the behavior we want.
TEST_F(TxnParticipantTest, ThrowDuringPreparedOnTransactionCommitDoesNothing) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    const auto commitTimestamp = Timestamp(1, 1);
    _opObserver->onTransactionCommitThrowsException = true;
    txnParticipant->prepareTransaction(opCtx());

    ASSERT_THROWS_CODE(txnParticipant->commitPreparedTransaction(opCtx(), commitTimestamp),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionCommitted);
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT_FALSE(txnParticipant->transactionIsCommitted());
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedCommitLetsTheAbortAtEntryPointToCleanUp) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

TEST_F(TxnParticipantTest, ConcurrencyOfCommitTransactionAndMigration) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // An commitPreparedTransaction() after a migration that bumps the active transaction number
    // should uassert.
    ASSERT_THROWS_CODE(txnParticipant->commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, ConcurrencyOfPrepareTransactionAndMigration) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the txnParticipant.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    bumpTxnNumberFromDifferentOpCtx(*opCtx()->getLogicalSessionId(), higherTxnNum);

    // A prepareTransaction() after a migration that bumps the active transaction number should
    // uassert.
    ASSERT_THROWS_CODE(txnParticipant->prepareTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
    ASSERT_FALSE(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, ContinuingATransactionWithNoResourcesAborts) {
    OperationContextSessionMongod(opCtx(), true, false, true);
    ASSERT_THROWS_CODE(OperationContextSessionMongod(opCtx(), true, false, boost::none),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, KillSessionsDoesNotAbortPreparedTransactions) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx());
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);
    txnParticipant->stashTransactionResources(opCtx());

    txnParticipant->abortArbitraryTransaction();
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, TransactionTimeoutDoesNotAbortPreparedTransactions) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx());
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);
    txnParticipant->stashTransactionResources(opCtx());

    txnParticipant->abortArbitraryTransactionIfExpired();
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotStartNewTransactionWhilePreparedTransactionInProgress) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx());
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);

    txnParticipant->stashTransactionResources(opCtx());

    {
        // Try to start a new transaction while there is already a prepared transaction on the
        // session. This should fail with a PreparedTransactionInProgress error.
        auto func = [&](OperationContext* newOpCtx) {
            auto session = SessionCatalog::get(newOpCtx)->getOrCreateSession(
                newOpCtx, *opCtx()->getLogicalSessionId());

            ASSERT_THROWS_CODE(
                session->onMigrateBeginOnPrimary(newOpCtx, *opCtx()->getTxnNumber() + 1, 1),
                AssertionException,
                ErrorCodes::PreparedTransactionInProgress);
        };

        runFunctionFromDifferentOpCtx(func);
    }

    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotInsertInPreparedTransaction) {
    OperationContextSessionMongod outerScopedSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    txnParticipant->prepareTransaction(opCtx());

    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "insert"),
                       AssertionException,
                       ErrorCodes::PreparedTransactionInProgress);

    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, MigrationThrowsOnPreparedTransaction) {
    OperationContextSessionMongod outerScopedSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    txnParticipant->prepareTransaction(opCtx());

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
    OperationContextSessionMongod outerScopedSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);

    txnParticipant->prepareTransaction(opCtx());

    // The next command throws an exception and wants to abort the transaction.
    // This is a no-op.
    txnParticipant->abortActiveUnpreparedOrStashPreparedTransaction(opCtx());
    ASSERT_FALSE(txnParticipant->transactionIsAborted());
    ASSERT_TRUE(_opObserver->transactionPrepared);
}

DEATH_TEST_F(TxnParticipantTest, AbortIsIllegalDuringCommittingPreparedTransaction, "invariant") {
    OperationContextSessionMongod outerScopedSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx());

    auto sessionId = *opCtx()->getLogicalSessionId();
    auto txnNum = *opCtx()->getTxnNumber();
    _opObserver->onTransactionCommitFn = [&](bool wasPrepared) {
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

    txnParticipant->commitPreparedTransaction(opCtx(), prepareTimestamp);
}

TEST_F(TxnParticipantTest, CannotContinueNonExistentTransaction) {
    ASSERT_THROWS_CODE(OperationContextSessionMongod(opCtx(), true, false, boost::none),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

// Tests that a transaction aborts if it becomes too large before trying to commit it.
TEST_F(TxnParticipantTest, TransactionTooLargeWhileBuilding) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod outerScopedSession(opCtx(), true, false, true);
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
        OperationContextSessionMongod innerScopedSession(opCtx(), true, boost::none, boost::none);

        txnParticipant->stashTransactionResources(opCtx());

        // The stash was a noop, so the locker, RecoveryUnit, and WriteUnitOfWork on the
        // OperationContext are unaffected.
        ASSERT_EQUALS(originalLocker, opCtx()->lockState());
        ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
        ASSERT(opCtx()->getWriteUnitOfWork());
    }
}

TEST_F(TxnParticipantTest, UnstashInNestedSessionIsANoop) {

    OperationContextSessionMongod outerScopedSession(opCtx(), true, false, true);

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
        OperationContextSessionMongod innerScopedSession(opCtx(), true, boost::none, boost::none);

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
        OperationContextSessionMongod opCtxSession(
            opCtx(), true /* shouldCheckOutSession */, autocommit, startTransaction);

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant->inMultiDocumentTransaction());

        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction);
        ASSERT(txnParticipant->inMultiDocumentTransaction());
    }

    void canSpecifyStartTransactionOnAbortedTxn() {
        auto autocommit = false;
        auto startTransaction = true;
        OperationContextSessionMongod opCtxSession(
            opCtx(), true /* shouldCheckOutSession */, autocommit, startTransaction);

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
        OperationContextSessionMongod opCtxSession(
            opCtx(), true /* shouldCheckOutSession */, autocommit, startTransaction);

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
        OperationContextSessionMongod opCtxSession(
            opCtx(), true /* shouldCheckOutSession */, autocommit, startTransaction);

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant->inMultiDocumentTransaction());

        txnParticipant->unstashTransactionResources(opCtx(), "insert");
        auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
        txnParticipant->addTransactionOperation(opCtx(), operation);
        txnParticipant->prepareTransaction(opCtx());

        ASSERT_THROWS_CODE(
            txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), autocommit, startTransaction),
            AssertionException,
            50911);
    }

    void cannotSpecifyStartTransactionOnStartedRetryableWrite() {
        boost::optional<bool> autocommit = boost::none;
        boost::optional<bool> startTransaction = boost::none;
        OperationContextSessionMongod opCtxSession(
            opCtx(), true /* shouldCheckOutSession */, autocommit, startTransaction);

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_FALSE(txnParticipant->inMultiDocumentTransaction());

        autocommit = false;
        startTransaction = true;
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

/**
 * Test fixture for transactions metrics.
 */
class TransactionsMetricsTest : public TxnParticipantTest {};

TEST_F(TransactionsMetricsTest, IncrementTotalStartedUponStartTransaction) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getTotalStarted();

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

    // Tests that the total transactions started counter is incremented by 1 when a new transaction
    // is started.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalStarted(),
              beforeTransactionStart + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalCommittedOnCommit) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");

    unsigned long long beforeCommitCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalCommitted();

    txnParticipant->commitUnpreparedTransaction(opCtx());

    // Assert that the committed counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalCommitted(), beforeCommitCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalAbortedUponAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    unsigned long long beforeAbortCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    txnParticipant->abortArbitraryTransaction();

    // Assert that the aborted counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortCount + 1U);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithAbort) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

TEST_F(TransactionsMetricsTest, SingleTransactionStatsStartTimeShouldBeSetUponTransactionStart) {
    // Save the time before the transaction is created.
    unsigned long long timeBeforeTxn = curTimeMicros64();
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

    unsigned long long timeAfterTxn = curTimeMicros64();

    // Start time should be greater than or equal to the time before the transaction was created.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_GTE(txnParticipant->getSingleTransactionStats().getStartTime(), timeBeforeTxn);

    // Start time should be less than or equal to the time after the transaction was started.
    ASSERT_LTE(txnParticipant->getSingleTransactionStats().getStartTime(), timeAfterTxn);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponCommit) {
    unsigned long long timeBeforeTxnStart = curTimeMicros64();
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    unsigned long long timeAfterTxnStart = curTimeMicros64();
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Sleep here to allow enough time to elapse.
    sleepmillis(10);

    unsigned long long timeBeforeTxnCommit = curTimeMicros64();
    txnParticipant->commitUnpreparedTransaction(opCtx());
    unsigned long long timeAfterTxnCommit = curTimeMicros64();

    ASSERT_GTE(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
               timeBeforeTxnCommit - timeAfterTxnStart);
    ASSERT_LTE(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
               timeAfterTxnCommit - timeBeforeTxnStart);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponAbort) {
    unsigned long long timeBeforeTxnStart = curTimeMicros64();
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    unsigned long long timeAfterTxnStart = curTimeMicros64();
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // Sleep here to allow enough time to elapse.
    sleepmillis(10);

    unsigned long long timeBeforeTxnAbort = curTimeMicros64();
    txnParticipant->abortArbitraryTransaction();
    unsigned long long timeAfterTxnAbort = curTimeMicros64();

    ASSERT_GTE(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
               timeBeforeTxnAbort - timeAfterTxnStart);
    ASSERT_LTE(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
               timeAfterTxnAbort - timeBeforeTxnStart);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilCommit) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Save the transaction's duration at this point.
    unsigned long long txnDurationAfterStart =
        txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64());
    sleepmillis(10);

    // The transaction's duration should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
              txnDurationAfterStart);
    sleepmillis(10);
    txnParticipant->commitUnpreparedTransaction(opCtx());
    unsigned long long txnDurationAfterCommit =
        txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64());

    // The transaction has committed, so the duration should have not increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
              txnDurationAfterCommit);

    ASSERT_GT(txnDurationAfterCommit, txnDurationAfterStart);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Save the transaction's duration at this point.
    unsigned long long txnDurationAfterStart =
        txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64());
    sleepmillis(10);

    // The transaction's duration should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
              txnDurationAfterStart);
    sleepmillis(10);
    txnParticipant->abortArbitraryTransaction();
    unsigned long long txnDurationAfterAbort =
        txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64());

    // The transaction has aborted, so the duration should have not increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getDuration(curTimeMicros64()),
              txnDurationAfterAbort);

    ASSERT_GT(txnDurationAfterAbort, txnDurationAfterStart);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndStash) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // Sleep a bit to make sure time active is nonzero.
    sleepmillis(1);

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // Time active should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // Sleep here to allow enough time to elapse.
    sleepmillis(10);
    txnParticipant->stashTransactionResources(opCtx());

    // Time active should have increased again.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);

    // Start a new transaction.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    txnParticipant->beginOrContinue(higherTxnNum, false, true);

    // Time active should be zero for a new transaction.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // Sleep here to allow enough time to elapse.
    sleepmillis(10);
    txnParticipant->abortArbitraryTransaction();

    // Time active should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());

    // The transaction is no longer active, so time active should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetUponAbortOnly) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    txnParticipant->abortArbitraryTransaction();

    // Time active should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilStash) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    sleepmillis(1);

    // Time active should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);

    // Time active should have increased again.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // The transaction is no longer active, so time active should not have increased.
    timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilCommit) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    sleepmillis(1);

    // Time active should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);

    // Time active should have increased again.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
    txnParticipant->commitUnpreparedTransaction(opCtx());

    // The transaction is no longer active, so time active should not have increased.
    timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetIfUnstashHasBadReadConcernArgs) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize bad read concern args (!readConcernArgs.isEmpty()).
    repl::ReadConcernArgs readConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Transaction resources do not exist yet.
    txnParticipant->unstashTransactionResources(opCtx(), "find");

    // Sleep a bit to make sure time active is nonzero.
    sleepmillis(1);

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // Time active should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64());

    // Transaction resources already exist here and should throw an exception due to bad read
    // concern arguments.
    ASSERT_THROWS_CODE(txnParticipant->unstashTransactionResources(opCtx(), "find"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    // Time active should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponStash) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_GTE(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
               Microseconds{0});

    // Save time inactive at this point.
    auto timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // Time inactive should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction is still inactive, so time inactive should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction is currently active, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->stashTransactionResources(opCtx());

    // The transaction is inactive again, so time inactive should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_GTE(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
               Microseconds{0});

    // Save time inactive at this point.
    auto timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // Time inactive should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    txnParticipant->abortArbitraryTransaction();

    timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction has aborted, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldIncreaseUntilCommit) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_GTE(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
               Microseconds{0});

    // Save time inactive at this point.
    auto timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // Time inactive should have increased.
    ASSERT_GT(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    txnParticipant->unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant->commitUnpreparedTransaction(opCtx());

    timeInactiveSoFar =
        txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction has committed, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);
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

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);
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
                                   unsigned long long curTime) {
    // Add time active micros to string.
    (*sb) << " timeActiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant->getSingleTransactionStats().getTimeActiveMicros(curTime));

    // Add time inactive micros to string.
    (*sb) << " timeInactiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant->getSingleTransactionStats().getTimeInactiveMicros(curTime));
}

/*
 * Builds the entire expected transaction info string and returns it.
 */
std::string buildTransactionInfoString(OperationContext* opCtx,
                                       TransactionParticipant* txnParticipant,
                                       std::string terminationCause,
                                       const LogicalSessionId sessionId,
                                       const TxnNumber txnNum,
                                       const int metricValue) {
    // Calling transactionInfoForLog to get the actual transaction info string.
    const auto lockerInfo = opCtx->lockState()->getLockerInfo();
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

    auto curTime = curTimeMicros64();
    StringBuilder timeActiveAndInactiveInfo;
    buildTimeActiveInactiveString(&timeActiveAndInactiveInfo, txnParticipant, curTime);

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
    // acquireCount: { W: 1 } } } 0ms
    StringBuilder expectedTransactionInfo;
    expectedTransactionInfo
        << parametersInfo.str() << readTimestampInfo.str() << singleTransactionStatsInfo.str()
        << " terminationCause:" << terminationCause << timeActiveAndInactiveInfo.str()
        << " numYields:" << 0 << " locks:" << locks.done().toString() << " "
        << Milliseconds{static_cast<long long>(
                            txnParticipant->getSingleTransactionStats().getDuration(curTime)) /
                        1000};
    return expectedTransactionInfo.str();
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterCommit) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

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

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo();
    ASSERT(lockerInfo);
    std::string testTransactionInfo =
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "committed",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterAbort) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

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

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo();
    ASSERT(lockerInfo);

    std::string testTransactionInfo =
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   *opCtx()->getTxnNumber(),
                                   metricValue);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

DEATH_TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogWithNoLockerInfoStats, "invariant") {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo();
    ASSERT(lockerInfo);

    txnParticipant->unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant->commitUnpreparedTransaction(opCtx());

    txnParticipant->transactionInfoForLogForTest(nullptr, true, readConcernArgs);
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowCommit) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

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
    sleepmillis(serverGlobalParams.slowMS + 1);

    startCapturingLogMessages();
    txnParticipant->commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo();
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

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
    sleepmillis(serverGlobalParams.slowMS + 1);

    startCapturingLogMessages();
    txnParticipant->abortActiveTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo();
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowStashedAbort) {
    OperationContextSessionMongod opCtxSession(opCtx(), true, false, true);

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
    const auto lockerInfo = txnResourceStashLocker->getLockerInfo();

    serverGlobalParams.slowMS = 10;
    sleepmillis(serverGlobalParams.slowMS + 1);

    startCapturingLogMessages();
    txnParticipant->abortArbitraryTransaction();
    stopCapturingLogMessages();

    std::string expectedTransactionInfo = "transaction " +
        txnParticipant->transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

}  // namespace
}  // namespace mongo
