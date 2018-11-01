
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

#include "mongo/db/transaction_coordinator.h"

#include <future>

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using State = TransactionCoordinator::StateMachine::State;
using Coordinator = ServiceContextMongoDTest;

const Timestamp dummyTimestamp = Timestamp::min();

void doCommit(TransactionCoordinator& coordinator) {
    coordinator.recvCoordinateCommit({ShardId("shard0000")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.madeCommitDecisionDurable();
    coordinator.recvCommitAck(ShardId("shard0000"));
}

void doAbort(TransactionCoordinator& coordinator) {
    coordinator.recvCoordinateCommit({ShardId("shard0000")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteAbort(ShardId("shard0000"));
    coordinator.madeAbortDecisionDurable();
    coordinator.recvAbortAck(ShardId("shard0000"));
}
}

TEST_F(Coordinator, SomeParticipantVotesAbortLeadsToAbort) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteAbort(ShardId("shard0000"));
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    coordinator.madeAbortDecisionDurable();
    coordinator.recvAbortAck(ShardId("shard0001"));
    ASSERT_EQ(State::kAborted, coordinator.state());
}

TEST_F(Coordinator, AllParticipantsVoteCommitLeadsToCommit) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    coordinator.madeCommitDecisionDurable();
    coordinator.recvCommitAck(ShardId("shard0000"));
    coordinator.recvCommitAck(ShardId("shard0001"));
    ASSERT_EQ(State::kCommitted, coordinator.state());
}

TEST_F(Coordinator, NotHearingSomeParticipantsVoteOtherParticipantsVotedCommitLeadsToStillWaiting) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    ASSERT_EQ(State::kWaitingForVotes, coordinator.state());
}

TEST_F(Coordinator, NotHearingSomeParticipantsVoteAnotherParticipantVotedAbortLeadsToAbort) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteAbort(ShardId("shard0000"));
    ASSERT_EQ(State::kMakingAbortDecisionDurable, coordinator.state());
}

TEST_F(Coordinator, NotHearingSomeParticipantsCommitAckLeadsToStillWaiting) {
    TransactionCoordinator coordinator;
    coordinator.recvCoordinateCommit({ShardId("shard0000"), ShardId("shard0001")});
    coordinator.madeParticipantListDurable();
    coordinator.recvVoteCommit(ShardId("shard0000"), dummyTimestamp);
    coordinator.recvVoteCommit(ShardId("shard0001"), dummyTimestamp);
    coordinator.madeCommitDecisionDurable();
    coordinator.recvCommitAck(ShardId("shard0000"));
    ASSERT_EQ(State::kWaitingForCommitAcks, coordinator.state());
}

TEST_F(Coordinator, WaitForCompletionReturnsOnChangeToCommitted) {
    TransactionCoordinator coordinator;
    auto future = coordinator.waitForCompletion();
    doCommit(coordinator);
    auto finalState = future.get();
    ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kCommitted);
}

TEST_F(Coordinator, WaitForCompletionReturnsOnChangeToAborted) {
    TransactionCoordinator coordinator;
    auto future = coordinator.waitForCompletion();
    doAbort(coordinator);
    auto finalState = future.get();
    ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kAborted);
}

TEST_F(Coordinator, RepeatedCallsToWaitForCompletionAllReturn) {
    TransactionCoordinator coordinator;
    auto futures = {coordinator.waitForCompletion(),
                    coordinator.waitForCompletion(),
                    coordinator.waitForCompletion()};
    doAbort(coordinator);

    for (auto& future : futures) {
        auto finalState = future.get();
        ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kAborted);
    }
}

TEST_F(Coordinator, CallingWaitForCompletionAfterAlreadyCompleteReturns) {
    TransactionCoordinator coordinator;
    doAbort(coordinator);

    auto future = coordinator.waitForCompletion();
    auto finalState = future.get();

    ASSERT_EQ(finalState, TransactionCoordinator::StateMachine::State::kAborted);
}

}  // namespace mongo
