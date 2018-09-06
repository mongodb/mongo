/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/transaction_coordinator_catalog.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/db/transaction_coordinator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class TransactionCoordinatorCatalogTest : public unittest::Test {
public:
    void setUp() override {
        _coordinatorCatalog = std::make_unique<TransactionCoordinatorCatalog>();
    }
    void tearDown() override {}

    TransactionCoordinatorCatalog& coordinatorCatalog() {
        return *_coordinatorCatalog;
    }

private:
    std::unique_ptr<TransactionCoordinatorCatalog> _coordinatorCatalog;
};

TEST_F(TransactionCoordinatorCatalogTest, GetOnSessionThatDoesNotExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;

    auto coordinator = coordinatorCatalog().get(lsid, txnNumber);
    ASSERT_EQ(coordinator, boost::none);
}

TEST_F(TransactionCoordinatorCatalogTest,
       GetOnSessionThatExistsButTxnNumberThatDoesntExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    coordinatorCatalog().create(lsid, txnNumber);
    auto coordinatorInCatalog = coordinatorCatalog().get(lsid, txnNumber + 1);
    ASSERT_EQ(coordinatorInCatalog, boost::none);
}


TEST_F(TransactionCoordinatorCatalogTest, CreateFollowedByGetReturnsCoordinator) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    coordinatorCatalog().create(lsid, txnNumber);
    auto coordinatorInCatalog = coordinatorCatalog().get(lsid, txnNumber);
    ASSERT_NOT_EQUALS(coordinatorInCatalog, boost::none);
}

TEST_F(TransactionCoordinatorCatalogTest, SecondCreateForSessionDoesNotOverwriteFirstCreate) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber1 = 1;
    TxnNumber txnNumber2 = 2;
    auto coordinator1 = coordinatorCatalog().create(lsid, txnNumber1);
    auto coordinator2 = coordinatorCatalog().create(lsid, txnNumber2);

    auto coordinator1InCatalog = coordinatorCatalog().get(lsid, txnNumber1);
    ASSERT_NOT_EQUALS(coordinator1InCatalog, boost::none);
}

DEATH_TEST_F(TransactionCoordinatorCatalogTest,
             CreatingACoordinatorWithASessionIdAndTxnNumberThatAlreadyExistFails,
             "Invariant failure") {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    coordinatorCatalog().create(lsid, txnNumber);
    // Re-creating w/ same session id and txn number should cause invariant failure
    coordinatorCatalog().create(lsid, txnNumber);
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
    coordinatorCatalog().create(lsid, txnNumber);
    auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);

    ASSERT_TRUE(latestTxnNumAndCoordinator);
    ASSERT_EQ(latestTxnNumAndCoordinator->first, txnNumber);
}

TEST_F(TransactionCoordinatorCatalogTest,
       TwoCreatesFollowedByGetLatestOnSessionReturnsCoordinatorWithHighestTxnNumber) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber1 = 1;
    TxnNumber txnNumber2 = 2;
    coordinatorCatalog().create(lsid, txnNumber1);
    coordinatorCatalog().create(lsid, txnNumber2);
    auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);

    ASSERT_EQ(latestTxnNumAndCoordinator->first, txnNumber2);
}

// Basically checks to make sure we clear out entries in the catalog for
// sessions with no remaining coordinators.
TEST_F(TransactionCoordinatorCatalogTest,
       CreatingAndThenRemovingACoordinatorFollowedByGetLatestOnSessionReturnsNone) {
    using CoordinatorState = TransactionCoordinator::StateMachine::State;

    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    auto coordinator = coordinatorCatalog().create(lsid, txnNumber);

    coordinator->recvCoordinateCommit({ShardId("shard0000")});
    coordinator->recvVoteCommit(ShardId("shard0000"), 0);
    coordinator->recvCommitAck(ShardId("shard0000"));
    ASSERT_EQ(coordinator->state(), CoordinatorState::kCommitted);

    coordinatorCatalog().remove(lsid, txnNumber);

    auto latestTxnNumAndCoordinator = coordinatorCatalog().getLatestOnSession(lsid);
    ASSERT_FALSE(latestTxnNumAndCoordinator);
}

TEST_F(TransactionCoordinatorCatalogTest, RemovingACommittedCoordinatorSucceeds) {
    using CoordinatorState = TransactionCoordinator::StateMachine::State;

    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    auto coordinator = coordinatorCatalog().create(lsid, txnNumber);

    coordinator->recvCoordinateCommit({ShardId("shard0000")});
    coordinator->recvVoteCommit(ShardId("shard0000"), 0);
    coordinator->recvCommitAck(ShardId("shard0000"));
    ASSERT_EQ(coordinator->state(), CoordinatorState::kCommitted);

    coordinatorCatalog().remove(lsid, txnNumber);
    auto coordinatorInCatalog = coordinatorCatalog().get(lsid, txnNumber);
    ASSERT_EQ(coordinatorInCatalog, boost::none);
}

TEST_F(TransactionCoordinatorCatalogTest, RemovingAnAbortedCoordinatorSucceeds) {
    using CoordinatorState = TransactionCoordinator::StateMachine::State;

    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    auto coordinator = coordinatorCatalog().create(lsid, txnNumber);

    coordinator->recvCoordinateCommit({ShardId("shard0000")});
    coordinator->recvVoteAbort(ShardId("shard0000"));
    coordinator->recvAbortAck(ShardId("shard0000"));
    ASSERT_EQ(coordinator->state(), CoordinatorState::kAborted);

    coordinatorCatalog().remove(lsid, txnNumber);
    auto coordinatorInCatalog = coordinatorCatalog().get(lsid, txnNumber);
    ASSERT_EQ(coordinatorInCatalog, boost::none);
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
