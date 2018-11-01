
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

#include "mongo/db/transaction_coordinator_catalog.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/db/transaction_coordinator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const Timestamp dummyTimestamp = Timestamp::min();

class TransactionCoordinatorCatalogTest : public unittest::Test {
public:
    void setUp() override {
        _coordinatorCatalog = std::make_shared<TransactionCoordinatorCatalog>();
    }
    void tearDown() override {
        _coordinatorCatalog.reset();
        // Make sure all of the coordinators are in a committed/aborted state before they are
        // destroyed. Otherwise, the coordinator's destructor will invariant because it will still
        // have outstanding futures that have not been completed (the one to remove itself from the
        // catalog). This has the added benefit of testing whether it's okay to destroy
        // the catalog while there are outstanding coordinators.
        for (auto& coordinator : _coordinatorsForTest) {
            coordinator->recvTryAbort();
        }
        _coordinatorsForTest.clear();
    }

    TransactionCoordinatorCatalog& coordinatorCatalog() {
        return *_coordinatorCatalog;
    }

    std::shared_ptr<TransactionCoordinator> createCoordinatorInCatalog(LogicalSessionId lsid,
                                                                       TxnNumber txnNumber) {
        auto coordinator = coordinatorCatalog().create(lsid, txnNumber);
        _coordinatorsForTest.push_back(coordinator);
        return coordinator;
    }

private:
    // Note: MUST be shared_ptr due to use of std::enable_shared_from_this
    std::shared_ptr<TransactionCoordinatorCatalog> _coordinatorCatalog;
    std::vector<std::shared_ptr<TransactionCoordinator>> _coordinatorsForTest;
};

TEST_F(TransactionCoordinatorCatalogTest, GetOnSessionThatDoesNotExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;

    auto coordinator = coordinatorCatalog().get(lsid, txnNumber);
    ASSERT(coordinator == nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest,
       GetOnSessionThatExistsButTxnNumberThatDoesntExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(lsid, txnNumber);
    auto coordinatorInCatalog = coordinatorCatalog().get(lsid, txnNumber + 1);
    ASSERT(coordinatorInCatalog == nullptr);
}


TEST_F(TransactionCoordinatorCatalogTest, CreateFollowedByGetReturnsCoordinator) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(lsid, txnNumber);
    auto coordinatorInCatalog = coordinatorCatalog().get(lsid, txnNumber);
    ASSERT(coordinatorInCatalog != nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest, SecondCreateForSessionDoesNotOverwriteFirstCreate) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber1 = 1;
    TxnNumber txnNumber2 = 2;
    auto coordinator1 = createCoordinatorInCatalog(lsid, txnNumber1);
    auto coordinator2 = createCoordinatorInCatalog(lsid, txnNumber2);

    auto coordinator1InCatalog = coordinatorCatalog().get(lsid, txnNumber1);
    ASSERT(coordinator1InCatalog != nullptr);
}

DEATH_TEST_F(TransactionCoordinatorCatalogTest,
             CreatingACoordinatorWithASessionIdAndTxnNumberThatAlreadyExistFails,
             "Invariant failure") {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(lsid, txnNumber);
    // Re-creating w/ same session id and txn number should cause invariant failure
    createCoordinatorInCatalog(lsid, txnNumber);
}

TEST_F(TransactionCoordinatorCatalogTest, GetLatestOnSessionWithNoCoordinatorsReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);
    ASSERT_FALSE(latestTxnNumAndCoordinator);
}

TEST_F(TransactionCoordinatorCatalogTest,
       CreateFollowedByGetLatestOnSessionReturnsOnlyCoordinator) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(lsid, txnNumber);
    auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);

    ASSERT_TRUE(latestTxnNumAndCoordinator);
    ASSERT_EQ(latestTxnNumAndCoordinator->first, txnNumber);
}

// TODO (SERVER-XXXX): Re-enable once coordinators are also participants and decision recovery
// works correctly.
// TEST_F(TransactionCoordinatorCatalogTest,
//        CoordinatorsRemoveThemselvesFromCatalogWhenTheyReachCommittedState) {
//     using CoordinatorState = TransactionCoordinator::StateMachine::State;
//
//     LogicalSessionId lsid = makeLogicalSessionIdForTest();
//     TxnNumber txnNumber = 1;
//     auto coordinator = createCoordinatorInCatalog(lsid, txnNumber);
//
//     coordinator->recvCoordinateCommit({ShardId("shard0000")});
//     coordinator->recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
//     coordinator->recvCommitAck(ShardId("shard0000"));
//     ASSERT_EQ(coordinator->state(), CoordinatorState::kCommitted);
//
//     auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);
//     ASSERT_FALSE(latestTxnNumAndCoordinator);
// }

// TODO (SERVER-XXXX): Re-enable once coordinators are also participants and decision recovery
// works correctly.
// TEST_F(TransactionCoordinatorCatalogTest,
//        CoordinatorsRemoveThemselvesFromCatalogWhenTheyReachAbortedState) {
//     using CoordinatorState = TransactionCoordinator::StateMachine::State;
//
//     LogicalSessionId lsid = makeLogicalSessionIdForTest();
//     TxnNumber txnNumber = 1;
//     auto coordinator = createCoordinatorInCatalog(lsid, txnNumber);
//
//     coordinator->recvVoteAbort(ShardId("shard0000"));
//     ASSERT_EQ(coordinator->state(), CoordinatorState::kAborted);
//
//     auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);
//     ASSERT_FALSE(latestTxnNumAndCoordinator);
// }

TEST_F(TransactionCoordinatorCatalogTest,
       TwoCreatesFollowedByGetLatestOnSessionReturnsCoordinatorWithHighestTxnNumber) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber1 = 1;
    TxnNumber txnNumber2 = 2;
    createCoordinatorInCatalog(lsid, txnNumber1);
    createCoordinatorInCatalog(lsid, txnNumber2);
    auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);

    ASSERT_EQ(latestTxnNumAndCoordinator->first, txnNumber2);
}

// TODO (SERVER-36304/37021): Reenable once transaction participants are able to send
// votes and once we validate the state of the coordinator when a new transaction comes
// in for an existing session. For now, we're not validating the state of the
// coordinator which means it is possible that if we hit invalid behavior in testing
// that this will result in hidden incorrect behavior.
// DEATH_TEST_F(TransactionCoordinatorCatalogTest,
//              RemovingACoordinatorNotInCommittedOrAbortedStateFails,
//              "Invariant failure") {
//     LogicalSessionId lsid = makeLogicalSessionIdForTest();
//     TxnNumber txnNumber = 1;
//     coordinatorCatalog().create(lsid, txnNumber);
//     coordinatorCatalog().remove(lsid, txnNumber);
// }

}  // namespace
}  // namespace mongo
