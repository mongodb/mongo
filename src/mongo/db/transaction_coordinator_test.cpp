/**
 *    Copyright (C) 2018 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator.h"

#include <future>

#include "mongo/unittest/unittest.h"

namespace mongo {

using State = TransactionCoordinator::StateMachine::State;

const Timestamp dummyTimestamp = Timestamp::min();

TEST(Coordinator, SomeParticipantVotesAbortLeadsToAbort) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteAbort(ShardId("shard0000"));
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    ASSERT_EQ(State::kAborted, coordinator.state());
}

TEST(Coordinator, SomeParticipantsVoteAbortBeforeCoordinatorReceivesParticipantListLeadsToAbort) {
    TransactionCoordinator coordinator;
    coordinator.recvVoteAbort(ShardId("shard0000"));
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    ASSERT_EQ(State::kAborted, coordinator.state());
}

TEST(Coordinator, AllParticipantsVoteCommitLeadsToCommit) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    coordinator.recvCommitAck(ShardId("shard0000"));
    coordinator.recvCommitAck(ShardId("shard0001"));
    ASSERT_EQ(State::kCommitted, coordinator.state());
}

TEST(
    Coordinator,
    AllParticipantsVoteCommitSomeParticipantsVoteBeforeCoordinatorReceivesParticipantListLeadsToCommit) {
    TransactionCoordinator coordinator;
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    coordinator.recvCommitAck(ShardId("shard0000"));
    coordinator.recvCommitAck(ShardId("shard0001"));
    ASSERT_EQ(State::kCommitted, coordinator.state());
}

TEST(Coordinator, NotHearingSomeParticipantsVoteOtherParticipantsVotedCommitLeadsToStillWaiting) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    ASSERT_EQ(State::kWaitingForVotes, coordinator.state());
}

TEST(Coordinator, NotHearingSomeParticipantsVoteAnotherParticipantVotedAbortLeadsToAbort) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteAbort(ShardId("shard0000"));
    ASSERT_EQ(State::kAborted, coordinator.state());
}

TEST(Coordinator, NotHearingSomeParticipantsCommitAckLeadsToStillWaiting) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    coordinator.recvCommitAck(ShardId("shard0000"));
    ASSERT_EQ(State::kWaitingForCommitAcks, coordinator.state());
}

TEST(Coordinator, TryAbortWhileWaitingForParticipantListSuccessfullyAborts) {
    TransactionCoordinator coordinator;
    coordinator.recvTryAbort();
    ASSERT_EQ(State::kAborted, coordinator.state());
}

TEST(Coordinator, TryAbortWhileWaitingForVotesSuccessfullyAborts) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvTryAbort();
    ASSERT_EQ(State::kAborted, coordinator.state());
}

TEST(Coordinator, TryAbortWhileWaitingForCommitAcksDoesNotCancelCommit) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    ASSERT_EQ(State::kWaitingForCommitAcks, coordinator.state());
    coordinator.recvTryAbort();
    ASSERT_EQ(State::kWaitingForCommitAcks, coordinator.state());
    coordinator.recvCommitAck(ShardId("shard0000"));
    coordinator.recvCommitAck(ShardId("shard0001"));
    ASSERT_EQ(State::kCommitted, coordinator.state());
}

TEST(Coordinator, WaitForCompletionReturnsOnChangeToCommitted) {
    TransactionCoordinator coordinator;
    auto future = coordinator.waitForCompletion();
    coordinator.recvCoordinateCommit({ShardId("shard0000")});
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvCommitAck(ShardId("shard0000"));
    auto finalState = future.get();
    ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kCommitted);
}

TEST(Coordinator, WaitForCompletionReturnsOnChangeToAborted) {
    TransactionCoordinator coordinator;
    auto future = coordinator.waitForCompletion();
    coordinator.recvVoteAbort(ShardId("shard0000"));
    auto finalState = future.get();
    ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kAborted);
}

TEST(Coordinator, RepeatedCallsToWaitForCompletionAllReturn) {
    TransactionCoordinator coordinator;
    auto futures = {coordinator.waitForCompletion(),
                    coordinator.waitForCompletion(),
                    coordinator.waitForCompletion()};
    coordinator.recvVoteAbort(ShardId("shard0000"));

    for (auto& future : futures) {
        auto finalState = future.get();
        ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kAborted);
    }
}

TEST(Coordinator, CallingWaitForCompletionAfterAlreadyCompleteReturns) {
    TransactionCoordinator coordinator;
    coordinator.recvVoteAbort(ShardId("shard0000"));

    auto future = coordinator.waitForCompletion();
    auto finalState = future.get();

    ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kAborted);
}

}  // namespace mongo
