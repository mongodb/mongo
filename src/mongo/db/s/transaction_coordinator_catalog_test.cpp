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

#include "mongo/db/s/transaction_coordinator_catalog.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const StatusWith<BSONObj> kPrepareOk = BSON("ok" << 1 << "prepareTimestamp" << Timestamp(1, 1));

class TransactionCoordinatorCatalogTest : public TransactionCoordinatorTestFixture {
protected:
    void setUp() override {
        TransactionCoordinatorTestFixture::setUp();

        _coordinatorCatalog.emplace();
        _coordinatorCatalog->exitStepUp(Status::OK());
    }

    void tearDown() override {
        _coordinatorCatalog->onStepDown();
        _coordinatorCatalog.reset();

        TransactionCoordinatorTestFixture::tearDown();
    }

    void createCoordinatorInCatalog(OperationContext* opCtx,
                                    LogicalSessionId lsid,
                                    TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
        auto newCoordinator = std::make_shared<TransactionCoordinator>(
            operationContext(),
            lsid,
            txnNumberAndRetryCounter,
            std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
            Date_t::max());

        _coordinatorCatalog->insert(opCtx, lsid, txnNumberAndRetryCounter, newCoordinator);
    }

    boost::optional<TransactionCoordinatorCatalog> _coordinatorCatalog;
};

TEST_F(TransactionCoordinatorCatalogTest, GetOnSessionThatDoesNotExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter{1, 0};
    auto coordinator = _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter);
    ASSERT(coordinator == nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest,
       GetOnSessionThatExistsButTxnNumberThatDoesntExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{2, 0};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    auto coordinatorInCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter2);
    ASSERT(coordinatorInCatalog == nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest,
       GetOnSessionAndTxnNumberThatExistButTxnRetryCounterThatDoesntExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{1, 1};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    auto coordinatorInCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter2);
    ASSERT(coordinatorInCatalog == nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest, CreateFollowedByGetReturnsCoordinator) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter{1, 0};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter);
    auto coordinatorInCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter);
    ASSERT(coordinatorInCatalog != nullptr);
    ASSERT_EQ(coordinatorInCatalog->getTxnRetryCounterForTest(),
              *txnNumberAndRetryCounter.getTxnRetryCounter());
}

TEST_F(TransactionCoordinatorCatalogTest,
       SecondCreateForSessionDoesNotOverwriteFirstCreateDifferentTxnNumber) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{2, 0};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);

    auto coordinator1InCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter1);
    ASSERT(coordinator1InCatalog != nullptr);
    auto coordinator2InCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter2);
    ASSERT(coordinator2InCatalog != nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest,
       SecondCreateForSessionDoesNotOverwriteFirstCreateDifferentTxnRetryCounter) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{1, 1};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);

    auto coordinator1InCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter1);
    ASSERT(coordinator1InCatalog != nullptr);
    ASSERT_EQ(coordinator1InCatalog->getTxnRetryCounterForTest(),
              *txnNumberAndRetryCounter1.getTxnRetryCounter());
    auto coordinator2InCatalog =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter2);
    ASSERT(coordinator2InCatalog != nullptr);
    ASSERT_EQ(coordinator2InCatalog->getTxnRetryCounterForTest(),
              *txnNumberAndRetryCounter2.getTxnRetryCounter());
}

DEATH_TEST_F(TransactionCoordinatorCatalogTest,
             CreatingACoordinatorWithASessionIdTxnNumberAndRetryCounterThatAlreadyExistFails,
             "Invariant failure") {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter{1, 0};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter);
    // Re-creating w/ same session id and txn number should cause invariant failure
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter);
}

TEST_F(TransactionCoordinatorCatalogTest, GetLatestOnSessionWithNoCoordinatorsReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    auto latestTxnNumberRetryCounterAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_FALSE(latestTxnNumberRetryCounterAndCoordinator);
}

TEST_F(TransactionCoordinatorCatalogTest,
       CreateFollowedByGetLatestOnSessionReturnsOnlyCoordinatorLatestTxnNumber) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{2, 0};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);

    auto latestTxnNumberRetryCounterAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_TRUE(latestTxnNumberRetryCounterAndCoordinator);

    auto latestTxnNumberAndRetryCounter = latestTxnNumberRetryCounterAndCoordinator->first;
    ASSERT_EQ(latestTxnNumberAndRetryCounter.getTxnNumber(),
              txnNumberAndRetryCounter2.getTxnNumber());
    ASSERT_EQ(*latestTxnNumberAndRetryCounter.getTxnRetryCounter(),
              *txnNumberAndRetryCounter2.getTxnRetryCounter());
}

TEST_F(TransactionCoordinatorCatalogTest,
       CreateFollowedByGetLatestOnSessionReturnsOnlyCoordinatorLatestTxnNumberAndTxnRetryCounter) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{1, 1};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);

    auto latestTxnNumberRetryCounterAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_TRUE(latestTxnNumberRetryCounterAndCoordinator);

    auto latestTxnNumberAndRetryCounter = latestTxnNumberRetryCounterAndCoordinator->first;
    ASSERT_EQ(latestTxnNumberAndRetryCounter.getTxnNumber(),
              txnNumberAndRetryCounter2.getTxnNumber());
    ASSERT_EQ(*latestTxnNumberAndRetryCounter.getTxnRetryCounter(),
              *txnNumberAndRetryCounter2.getTxnRetryCounter());
}

TEST_F(TransactionCoordinatorCatalogTest, CoordinatorsRemoveThemselvesFromCatalogWhenTheyComplete) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter{1, 0};
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter);
    auto coordinator = _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter);

    coordinator->cancelIfCommitNotYetStarted();
    coordinator->onCompletion().wait();

    // Wait for the coordinator to be removed before attempting to call getLatestOnSession() since
    // the coordinator is removed from the catalog asynchronously.
    _coordinatorCatalog->join();

    auto latestTxnNumberRetryCounterAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_FALSE(latestTxnNumberRetryCounterAndCoordinator);
}

TEST_F(TransactionCoordinatorCatalogTest,
       TwoCreatesFollowedByGetLatestOnSessionReturnsCoordinatorWithHighestTxnNumber) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{2, 0};

    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);

    auto latestTxnNumberRetryCounterAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_TRUE(latestTxnNumberRetryCounterAndCoordinator);

    auto latestTxnNumberAndRetryCounter = latestTxnNumberRetryCounterAndCoordinator->first;
    ASSERT_EQ(latestTxnNumberAndRetryCounter.getTxnNumber(),
              txnNumberAndRetryCounter2.getTxnNumber());
    ASSERT_EQ(*latestTxnNumberAndRetryCounter.getTxnRetryCounter(),
              *txnNumberAndRetryCounter2.getTxnRetryCounter());
}

TEST_F(
    TransactionCoordinatorCatalogTest,
    TwoCreatesFollowedByGetLatestOnSessionReturnsCoordinatorWithHighestTxnNumberAndTxnRetryCounter) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{1, 1};

    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);

    auto latestTxnNumberRetryCounterAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_TRUE(latestTxnNumberRetryCounterAndCoordinator);

    auto latestTxnNumberAndRetryCounter = latestTxnNumberRetryCounterAndCoordinator->first;
    ASSERT_EQ(latestTxnNumberAndRetryCounter.getTxnNumber(),
              txnNumberAndRetryCounter2.getTxnNumber());
    ASSERT_EQ(*latestTxnNumberAndRetryCounter.getTxnRetryCounter(),
              *txnNumberAndRetryCounter2.getTxnRetryCounter());
}

DEATH_TEST_REGEX_F(TransactionCoordinatorCatalogTest,
                   CreateExistingSessionAndTxnNumberWithPreviouslyCommittedTxnRetryCounterFails,
                   "a previous coordinator.*already committed") {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter1{1, 0};
    TxnNumberAndRetryCounter txnNumberAndRetryCounter2{1, 1};

    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter1);
    auto coordinator1 =
        _coordinatorCatalog->get(operationContext(), lsid, txnNumberAndRetryCounter1);
    coordinator1->runCommit(operationContext(), kTwoShardIdList);

    assertCommandSentAndRespondWith("prepareTransaction", kPrepareOk, boost::none);
    assertCommandSentAndRespondWith("prepareTransaction", kPrepareOk, boost::none);

    coordinator1->getDecision().get();

    createCoordinatorInCatalog(operationContext(), lsid, txnNumberAndRetryCounter2);
}

TEST_F(TransactionCoordinatorCatalogTest, StepDownBeforeCoordinatorInsertedIntoCatalog) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumberAndRetryCounter txnNumberAndRetryCounter{1, 0};

    txn::AsyncWorkScheduler aws(getServiceContext());
    TransactionCoordinatorCatalog catalog;
    catalog.exitStepUp(Status::OK());

    auto coordinator = std::make_shared<TransactionCoordinator>(operationContext(),
                                                                lsid,
                                                                txnNumberAndRetryCounter,
                                                                aws.makeChildScheduler(),
                                                                network()->now() + Seconds{5});

    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Test step down"});
    catalog.onStepDown();

    advanceClockAndExecuteScheduledTasks();

    catalog.insert(operationContext(), lsid, txnNumberAndRetryCounter, coordinator);
    catalog.join();

    coordinator->onCompletion().wait();
}

}  // namespace
}  // namespace mongo
