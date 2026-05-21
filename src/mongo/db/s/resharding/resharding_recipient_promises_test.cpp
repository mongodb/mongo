/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_recipient_promises.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/with_lock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const Timestamp kCloneTimestamp{10, 1};
const int64_t kApproxDocs = 100;
const int64_t kApproxBytes = 200;

ReshardingRecipientMetrics makeMetrics() {
    ReshardingRecipientMetrics m;
    m.setApproxDocumentsToCopy(kApproxDocs);
    m.setApproxBytesToCopy(kApproxBytes);
    return m;
}

ReshardingRecipientDocument makeDocument(RecipientStateEnum state) {
    RecipientShardContext ctx;
    ctx.setState(state);
    ReshardingRecipientDocument document{std::move(ctx)};
    document.setDonorShards({});
    document.setMinimumOperationDurationMillis(0);
    document.setCloneTimestamp(kCloneTimestamp);
    document.setMetrics(makeMetrics());
    return document;
}

class ReshardingRecipientPromisesTest : public unittest::Test {};

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryFulfillsAllDonorsPreparedWhenStateIsBeyondAwaitingFetchTimestamp) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCloning);

    promises.recover(WithLock::withoutLock(), document);

    auto details = promises.getAllDonorsPreparedToDonateFuture().getNoThrow().getValue();
    ASSERT_EQ(details.cloneTimestamp, kCloneTimestamp);
    ASSERT_EQ(details.approxDocumentsToCopy, kApproxDocs);
    ASSERT_EQ(details.approxBytesToCopy, kApproxBytes);
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillAllDonorsPreparedWhenStateIsAtAwaitingFetchTimestamp) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kAwaitingFetchTimestamp);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillAllDonorsPreparedWhenMetricsAreMissing) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCloning);
    document.setMetrics(boost::none);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillAllDonorsPreparedWhenCloneTimestampIsMissing) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCloning);
    document.setCloneTimestamp(boost::none);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillAllDonorsPreparedWhenMetricsAreIncomplete) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCloning);
    ReshardingRecipientMetrics incomplete;
    incomplete.setApproxDocumentsToCopy(kApproxDocs);
    document.setMetrics(incomplete);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnCoordinatorStateAdvancedFulfillsAllDonorsPreparedWhenCloningWithDetails) {
    ReshardingRecipientPromises promises;

    ReshardingRecipientPromises::CloneDetails details{
        kCloneTimestamp, kApproxDocs, kApproxBytes, {}};
    promises.onCoordinatorStateAdvanced(
        WithLock::withoutLock(), CoordinatorStateEnum::kCloning, details);

    auto result = promises.getAllDonorsPreparedToDonateFuture().getNoThrow().getValue();
    ASSERT_EQ(result.cloneTimestamp, kCloneTimestamp);
    ASSERT_EQ(result.approxDocumentsToCopy, kApproxDocs);
    ASSERT_EQ(result.approxBytesToCopy, kApproxBytes);
}

TEST_F(ReshardingRecipientPromisesTest,
       OnCoordinatorStateAdvancedDoesNotFulfillAllDonorsPreparedAfterCloning) {
    ReshardingRecipientPromises promises;

    promises.onCoordinatorStateAdvanced(
        WithLock::withoutLock(), CoordinatorStateEnum::kApplying, boost::none);

    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnCoordinatorStateAdvancedDoesNotFulfillAllDonorsPreparedWhenCloningWithNoCloneDetails) {
    ReshardingRecipientPromises promises;
    promises.onCoordinatorStateAdvanced(
        WithLock::withoutLock(), CoordinatorStateEnum::kCloning, boost::none);
    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnCoordinatorStateAdvancedDoesNotFulfillAllDonorsPreparedBeforeCloning) {
    ReshardingRecipientPromises promises;

    ReshardingRecipientPromises::CloneDetails details{
        kCloneTimestamp, kApproxDocs, kApproxBytes, {}};
    promises.onCoordinatorStateAdvanced(
        WithLock::withoutLock(), CoordinatorStateEnum::kInitializing, details);

    ASSERT_FALSE(promises.getAllDonorsPreparedToDonateFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryFulfillsInApplyingOrErrorWhenStateIsAtOrBeyondApplying) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kApplying);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_TRUE(promises.getInApplyingOrErrorFuture().isReady());
    ASSERT_OK(promises.getInApplyingOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest, RecoveryFulfillsInApplyingOrErrorWhenStateIsError) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kError);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_TRUE(promises.getInApplyingOrErrorFuture().isReady());
    ASSERT_OK(promises.getInApplyingOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillInApplyingOrErrorWhenStateIsBeforeApplying) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCloning);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getInApplyingOrErrorFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnRecipientStateAdvancedFulfillsInApplyingOrErrorAtApplying) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(), RecipientStateEnum::kApplying);

    ASSERT_TRUE(promises.getInApplyingOrErrorFuture().isReady());
    ASSERT_OK(promises.getInApplyingOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest, OnRecipientStateAdvancedFulfillsInApplyingOrErrorAtError) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(), RecipientStateEnum::kError);

    ASSERT_TRUE(promises.getInApplyingOrErrorFuture().isReady());
    ASSERT_OK(promises.getInApplyingOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnRecipientStateAdvancedDoesNotFulfillInApplyingOrErrorBeforeApplying) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(), RecipientStateEnum::kCloning);

    ASSERT_FALSE(promises.getInApplyingOrErrorFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryFulfillsInStrictConsistencyOrErrorWhenStateIsAtOrBeyondStrictConsistency) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kStrictConsistency);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_TRUE(promises.getInStrictConsistencyOrErrorFuture().isReady());
    ASSERT_OK(promises.getInStrictConsistencyOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryFulfillsInStrictConsistencyOrErrorWhenStateIsError) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kError);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_TRUE(promises.getInStrictConsistencyOrErrorFuture().isReady());
    ASSERT_OK(promises.getInStrictConsistencyOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillInStrictConsistencyOrErrorWhenStateIsBeforeError) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kApplying);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getInStrictConsistencyOrErrorFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnRecipientStateAdvancedFulfillsInStrictConsistencyOrErrorAtStrictConsistency) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(),
                                      RecipientStateEnum::kStrictConsistency);

    ASSERT_TRUE(promises.getInStrictConsistencyOrErrorFuture().isReady());
    ASSERT_OK(promises.getInStrictConsistencyOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnRecipientStateAdvancedFulfillsInStrictConsistencyOrErrorAtError) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(), RecipientStateEnum::kError);

    ASSERT_TRUE(promises.getInStrictConsistencyOrErrorFuture().isReady());
    ASSERT_OK(promises.getInStrictConsistencyOrErrorFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnRecipientStateAdvancedDoesNotFulfillInStrictConsistencyOrErrorBeforeError) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(), RecipientStateEnum::kApplying);

    ASSERT_FALSE(promises.getInStrictConsistencyOrErrorFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryFulfillsTransitionedToCreateCollectionWhenStateIsAtOrBeyondCreatingCollection) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCreatingCollection);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_TRUE(promises.getTransitionedToCreateCollectionFuture().isReady());
    ASSERT_OK(promises.getTransitionedToCreateCollectionFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryFulfillsTransitionedToCreateCollectionWhenStateIsBeyondCreatingCollection) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kCloning);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_TRUE(promises.getTransitionedToCreateCollectionFuture().isReady());
    ASSERT_OK(promises.getTransitionedToCreateCollectionFuture().getNoThrow());
}

TEST_F(ReshardingRecipientPromisesTest,
       RecoveryDoesNotFulfillTransitionedToCreateCollectionWhenStateIsAwaitingFetchTimestamp) {
    ReshardingRecipientPromises promises;

    auto document = makeDocument(RecipientStateEnum::kAwaitingFetchTimestamp);

    promises.recover(WithLock::withoutLock(), document);

    ASSERT_FALSE(promises.getTransitionedToCreateCollectionFuture().isReady());
}

TEST_F(ReshardingRecipientPromisesTest,
       OnRecipientStateAdvancedFulfillsTransitionedToCreateCollectionAtCreatingCollection) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(),
                                      RecipientStateEnum::kCreatingCollection);

    ASSERT_TRUE(promises.getTransitionedToCreateCollectionFuture().isReady());
    ASSERT_OK(promises.getTransitionedToCreateCollectionFuture().getNoThrow());
}

TEST_F(
    ReshardingRecipientPromisesTest,
    OnRecipientStateAdvancedDoesNotFulfillTransitionedToCreateCollectionBeforeCreatingCollection) {
    ReshardingRecipientPromises promises;

    promises.onRecipientStateAdvanced(WithLock::withoutLock(),
                                      RecipientStateEnum::kAwaitingFetchTimestamp);

    ASSERT_FALSE(promises.getTransitionedToCreateCollectionFuture().isReady());
}

}  // namespace
}  // namespace mongo
