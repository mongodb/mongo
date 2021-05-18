/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/shard_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ReshardingCoordinatorObserverTest : public unittest::Test {
protected:
    ReshardingCoordinatorDocument makeCoordinatorDocWithRecipientsAndDonors(
        std::vector<RecipientShardEntry>& recipients,
        std::vector<DonorShardEntry>& donors,
        boost::optional<Status> abortReason = boost::none) {
        auto coordinatorDoc = ReshardingCoordinatorDocument();
        coordinatorDoc.setRecipientShards(std::move(recipients));
        coordinatorDoc.setDonorShards(std::move(donors));
        emplaceTruncatedAbortReasonIfExists(coordinatorDoc, abortReason);
        return coordinatorDoc;
    }

    std::vector<DonorShardEntry> makeMockDonorsInState(
        DonorStateEnum donorState,
        boost::optional<Timestamp> timestamp = boost::none,
        boost::optional<Status> abortReason = boost::none) {
        // The mock state here is simulating only one donor shard having errored locally.
        return {makeDonorShard(ShardId{"s1"}, donorState, timestamp),
                makeDonorShard(ShardId{"s2"}, donorState, timestamp, abortReason),
                makeDonorShard(ShardId{"s3"}, donorState, timestamp)};
    }

    std::vector<RecipientShardEntry> makeMockRecipientsInState(
        RecipientStateEnum recipientState,
        boost::optional<Timestamp> timestamp = boost::none,
        boost::optional<Status> abortReason = boost::none) {
        // The mock state here is simulating only one donor shard having errored locally.
        return {makeRecipientShard(ShardId{"s1"}, recipientState),
                makeRecipientShard(ShardId{"s2"}, recipientState, abortReason),
                makeRecipientShard(ShardId{"s3"}, recipientState)};
    }
};

TEST_F(ReshardingCoordinatorObserverTest, onReshardingParticipantTransitionSucceeds) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsFinishedCloning();
    ASSERT_FALSE(fut.isReady());

    auto donorShards = makeMockDonorsInState(DonorStateEnum::kDonatingInitialData, Timestamp(1, 1));
    std::vector<RecipientShardEntry> recipientShards0{
        makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kCloning),
        makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kApplying)};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards0, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards1{
        makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kApplying),
        makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kApplying)};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards1, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, onReshardingParticipantTransitionTwoOutOfOrder) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsFinishedCloning();
    ASSERT_FALSE(fut.isReady());

    // By default, all donors should be kDonatingInitialData at this stage.
    auto donorShards = makeMockDonorsInState(DonorStateEnum::kDonatingInitialData, Timestamp(1, 1));

    std::vector<RecipientShardEntry> recipientShards0{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kCloning)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kApplying)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kApplying)}};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards0, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards1{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kCloning)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kApplying)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kCloning)}};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards1, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards2{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kApplying)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kApplying)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kApplying)}};
    auto coordinatorDoc2 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards2, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc2);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, participantReportsError) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsFinishedCloning();
    ASSERT_FALSE(fut.isReady());

    // By default, all donors should be kDonatingInitialData at this stage.
    auto donorShards = makeMockDonorsInState(DonorStateEnum::kDonatingInitialData, Timestamp(1, 1));

    std::vector<RecipientShardEntry> recipientShards{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kCloning)},
        {makeRecipientShard(ShardId{"s2"},
                            RecipientStateEnum::kError,
                            Status{ErrorCodes::InternalError, "We gotta abort"})},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kApplying)}};
    auto coordinatorDoc = makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc);
    auto resp = fut.getNoThrow();

    // If any participant is in state kError, regardless of other participants' states, an error
    // should be set.
    ASSERT_EQ(resp.getStatus(), ErrorCodes::InternalError);

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, participantsDoneAborting) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();

    auto fut = reshardingObserver->awaitAllDonorsDone();
    ASSERT_FALSE(fut.isReady());

    auto abortReason = Status{ErrorCodes::InternalError, "We gotta abort"};

    // All recipients and donors are done (including the donor who caused the abort) except a single
    // donor who hasn't seen there was an error yet.
    auto recipientShards = makeMockRecipientsInState(RecipientStateEnum::kDone, Timestamp(1, 1));
    std::vector<DonorShardEntry> donorShards0{
        {makeDonorShard(ShardId{"s1"}, DonorStateEnum::kDone, Timestamp(1, 1), abortReason)},
        {makeDonorShard(ShardId{"s2"}, DonorStateEnum::kDonatingOplogEntries, Timestamp(1, 1))},
        {makeDonorShard(ShardId{"s3"}, DonorStateEnum::kDone, Timestamp(1, 1))}};
    auto coordinatorDoc0 =
        makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards0, abortReason);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    // All participants are done.
    std::vector<DonorShardEntry> donorShards1{
        {makeDonorShard(ShardId{"s1"}, DonorStateEnum::kDone, Timestamp(1, 1), abortReason)},
        {makeDonorShard(ShardId{"s2"}, DonorStateEnum::kDone, Timestamp(1, 1))},
        {makeDonorShard(ShardId{"s3"}, DonorStateEnum::kDone, Timestamp(1, 1))}};
    auto coordinatorDoc1 =
        makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards1, abortReason);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_TRUE(fut.isReady());

    // Since all other promises should be fulfilled with an error, no need to interrupt the observer
    // before finishing this test case.
}

/**
 * Confirms that if one recipient is more than one state behind the other, the promise does not get
 * fulfilled early.
 */
TEST_F(ReshardingCoordinatorObserverTest, onReshardingRecipientsOutOfSync) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsFinishedCloning();
    ASSERT_FALSE(fut.isReady());

    auto donorShards = makeMockDonorsInState(DonorStateEnum::kDonatingInitialData, Timestamp(1, 1));
    std::vector<RecipientShardEntry> recipientShards0{
        makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kUnused),
        makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kStrictConsistency)};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards0, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards1{
        makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kApplying),
        makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kStrictConsistency)};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards1, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, onDonorsReportedMinFetchTimestamp) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllDonorsReadyToDonate();
    ASSERT_FALSE(fut.isReady());

    // By default, all recipients should be kUnused at this stage.
    auto recipientShards = makeMockRecipientsInState(RecipientStateEnum::kUnused);

    std::vector<DonorShardEntry> donorShards0{
        {makeDonorShard(ShardId{"s1"}, DonorStateEnum::kDonatingInitialData, Timestamp(1, 1))},
        {makeDonorShard(ShardId{"s2"}, DonorStateEnum::kPreparingToDonate)}};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards0);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    std::vector<DonorShardEntry> donorShards1{
        {makeDonorShard(ShardId{"s1"}, DonorStateEnum::kDonatingInitialData, Timestamp(1, 1))},
        {makeDonorShard(ShardId{"s2"}, DonorStateEnum::kDonatingInitialData, Timestamp(1, 1))}};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards1);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}
}  // namespace
}  // namespace mongo
