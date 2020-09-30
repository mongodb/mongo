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
        std::vector<RecipientShardEntry>& recipients, std::vector<DonorShardEntry>& donors) {
        auto coordinatorDoc = ReshardingCoordinatorDocument();
        coordinatorDoc.setRecipientShards(std::move(recipients));
        coordinatorDoc.setDonorShards(std::move(donors));
        return coordinatorDoc;
    }

    std::vector<DonorShardEntry> makeMockDonorsInState(
        DonorStateEnum donorState, boost::optional<Timestamp> timestamp = boost::none) {
        return {makeDonorShard(ShardId{"s1"}, donorState, timestamp),
                makeDonorShard(ShardId{"s2"}, donorState, timestamp),
                makeDonorShard(ShardId{"s3"}, donorState, timestamp)};
    }

    std::vector<RecipientShardEntry> makeMockRecipientsInState(
        RecipientStateEnum recipientState, boost::optional<Timestamp> timestamp = boost::none) {
        return {makeRecipientShard(ShardId{"s1"}, recipientState, timestamp),
                makeRecipientShard(ShardId{"s2"}, recipientState, timestamp),
                makeRecipientShard(ShardId{"s3"}, recipientState, timestamp)};
    }
};

TEST_F(ReshardingCoordinatorObserverTest, onReshardingParticipantTransitionSucceeds) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsCreatedCollection();
    ASSERT_FALSE(fut.isReady());

    auto donorShards = makeMockDonorsInState(DonorStateEnum::kUnused);
    std::vector<RecipientShardEntry> recipientShards0{
        makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kInitializing),
        makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kInitialized)};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards0, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards1{
        makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kInitialized),
        makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kInitialized)};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards1, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, onReshardingParticipantTransitionTwoOutOfOrder) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsCreatedCollection();
    ASSERT_FALSE(fut.isReady());

    // By default, all donors should be kUnused at this stage.
    auto donorShards = makeMockDonorsInState(DonorStateEnum::kUnused);

    std::vector<RecipientShardEntry> recipientShards0{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kInitializing)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kInitialized)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kInitialized)}};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards0, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards1{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kInitializing)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kInitialized)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kInitializing)}};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards1, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_FALSE(fut.isReady());

    std::vector<RecipientShardEntry> recipientShards2{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kInitialized)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kInitialized)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kInitialized)}};
    auto coordinatorDoc2 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards2, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc2);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, participantReportsError) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllRecipientsCreatedCollection();
    ASSERT_FALSE(fut.isReady());

    // By default, all donors should be kUnused at this stage.
    auto donorShards = makeMockDonorsInState(DonorStateEnum::kUnused);

    std::vector<RecipientShardEntry> recipientShards0{
        {makeRecipientShard(ShardId{"s1"}, RecipientStateEnum::kInitializing)},
        {makeRecipientShard(ShardId{"s2"}, RecipientStateEnum::kError)},
        {makeRecipientShard(ShardId{"s3"}, RecipientStateEnum::kInitialized)}};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards0, donorShards);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    auto resp = fut.getNoThrow();

    // If any participant is in state kError, regardless of other participants' states, an error
    // should be set.
    ASSERT_EQ(resp.getStatus(), ErrorCodes::InternalError);

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}

TEST_F(ReshardingCoordinatorObserverTest, onDonorsReportedMinFetchTimestamp) {
    auto reshardingObserver = std::make_shared<ReshardingCoordinatorObserver>();
    auto fut = reshardingObserver->awaitAllDonorsReadyToDonate();
    ASSERT_FALSE(fut.isReady());

    // By default, all recipients should be kInitialized at this stage.
    auto recipientShards = makeMockRecipientsInState(RecipientStateEnum::kInitialized);

    std::vector<DonorShardEntry> donorShards0{
        {makeDonorShard(ShardId{"s1"}, DonorStateEnum::kDonating, Timestamp())},
        {makeDonorShard(ShardId{"s2"}, DonorStateEnum::kPreparingToDonate)}};
    auto coordinatorDoc0 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards0);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc0);
    ASSERT_FALSE(fut.isReady());
    ASSERT_TRUE(reshardingObserver->awaitAllRecipientsCreatedCollection().isReady());

    std::vector<DonorShardEntry> donorShards1{
        {makeDonorShard(ShardId{"s1"}, DonorStateEnum::kDonating, Timestamp())},
        {makeDonorShard(ShardId{"s2"}, DonorStateEnum::kDonating, Timestamp())}};
    auto coordinatorDoc1 = makeCoordinatorDocWithRecipientsAndDonors(recipientShards, donorShards1);
    reshardingObserver->onReshardingParticipantTransition(coordinatorDoc1);
    ASSERT_TRUE(fut.isReady());

    reshardingObserver->interrupt(Status{ErrorCodes::Interrupted, "interrupted"});
}
}  // namespace
}  // namespace mongo
