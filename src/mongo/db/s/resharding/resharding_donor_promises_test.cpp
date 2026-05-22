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

#include "mongo/db/s/resharding/resharding_donor_promises.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

ReshardingDonorDocument makeDonorDoc(DonorStateEnum state,
                                     boost::optional<BSONObj> abortReason = boost::none) {
    DonorShardContext donorCtx;
    donorCtx.setState(state);
    if (abortReason) {
        AbortReason abortReasonStruct;
        abortReasonStruct.setAbortReason(*abortReason);
        donorCtx.setAbortReasonStruct(std::move(abortReasonStruct));
    }

    ReshardingDonorDocument doc(std::move(donorCtx), {ShardId{"recipient1"}});

    auto sourceNss = NamespaceString::createNamespaceString_forTest("db.coll");
    auto sourceUUID = UUID::gen();
    CommonReshardingMetadata metadata(UUID::gen(),
                                      sourceNss,
                                      sourceUUID,
                                      NamespaceString::createNamespaceString_forTest(
                                          "db.system.resharding." + sourceUUID.toString()),
                                      BSON("newKey" << 1));
    doc.setCommonReshardingMetadata(std::move(metadata));
    return doc;
}

class ReshardingDonorPromisesTest : public unittest::Test {};

// ---- recover(): persisted-state replay -------------------------------------------------------

TEST_F(ReshardingDonorPromisesTest, RecoverEarlyStateLeavesAllPromisesPending) {
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kPreparingToDonate);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_FALSE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_FALSE(promises.getAllRecipientsDoneApplyingFuture().isReady());
    ASSERT_FALSE(promises.getInDonatingOplogEntriesFuture().isReady());
    ASSERT_FALSE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverDonatingInitialDataLeavesEveryPromisePending) {
    // kDonatingInitialData is the last state at which none of the donor's workflow promises
    // should be considered already-fulfilled on stepup.
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kDonatingInitialData);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_FALSE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_FALSE(promises.getAllRecipientsDoneApplyingFuture().isReady());
    ASSERT_FALSE(promises.getInDonatingOplogEntriesFuture().isReady());
    ASSERT_FALSE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverDonatingOplogEntriesFulfillsThroughCloning) {
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kDonatingOplogEntries);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getInDonatingOplogEntriesFuture().isReady());

    ASSERT_FALSE(promises.getAllRecipientsDoneApplyingFuture().isReady());
    ASSERT_FALSE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverPreparingToBlockWritesFulfillsThroughApplying) {
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kPreparingToBlockWrites);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getInDonatingOplogEntriesFuture().isReady());
    ASSERT_TRUE(promises.getAllRecipientsDoneApplyingFuture().isReady());

    // kPreparingToBlockWrites alone is not enough to conclude the critical section acquisition
    // completed (the donor crashed between the transition and the in-process acquire). Only
    // strictly-past states (>= kBlockingWrites) prove acquisition.
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverBlockingWritesFulfillsCritSecAcquiredAndOrError) {
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kBlockingWrites);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_TRUE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_TRUE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverErrorFulfillsOnlyInBlockingWritesOrError) {
    // kError can be reached from any donor state earlier than kBlockingWrites without the
    // critical section ever being acquired, so only _inBlockingWritesOrError fires on recovery.
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kError);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_TRUE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverDoneSuccessFulfillsEveryWorkflowPromise) {
    ReshardingDonorPromises promises;
    auto doc = makeDonorDoc(DonorStateEnum::kDone);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getAllRecipientsDoneApplyingFuture().isReady());
    ASSERT_TRUE(promises.getInDonatingOplogEntriesFuture().isReady());
    ASSERT_TRUE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_TRUE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_TRUE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, RecoverDoneWithAbortReasonLeavesCritSecPromotedPending) {
    // kDone reached via abort means the critical section was never promoted. The donor's
    // terminal cleanup is expected to broadcast setError() so consumers see an error rather
    // than a spurious success; the recovery itself does not infer success.
    ReshardingDonorPromises promises;
    auto abortReason = BSON("code" << ErrorCodes::ReshardCollectionAborted << "errmsg"
                                   << "user-cancelled");
    auto doc = makeDonorDoc(DonorStateEnum::kDone, abortReason);
    promises.recover(WithLock::withoutLock(), doc);

    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

// ---- onCoordinatorStateAdvanced() ------------------------------------------------------------

TEST_F(ReshardingDonorPromisesTest, CoordinatorApplyingFulfillsAllRecipientsDoneCloning) {
    ReshardingDonorPromises promises;
    promises.onCoordinatorStateAdvanced(WithLock::withoutLock(), CoordinatorStateEnum::kApplying);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_FALSE(promises.getAllRecipientsDoneApplyingFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, CoordinatorBlockingWritesFulfillsAllRecipientsDoneApplying) {
    ReshardingDonorPromises promises;
    promises.onCoordinatorStateAdvanced(WithLock::withoutLock(),
                                        CoordinatorStateEnum::kBlockingWrites);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getAllRecipientsDoneApplyingFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, CoordinatorStateAdvanceIsIdempotent) {
    // Re-issuing an already-passed state must not crash by double-emplacing.
    ReshardingDonorPromises promises;
    promises.onCoordinatorStateAdvanced(WithLock::withoutLock(),
                                        CoordinatorStateEnum::kBlockingWrites);
    promises.onCoordinatorStateAdvanced(WithLock::withoutLock(), CoordinatorStateEnum::kApplying);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getAllRecipientsDoneApplyingFuture().isReady());
}

// ---- onDonorStateAdvanced() ------------------------------------------------------------------

TEST_F(ReshardingDonorPromisesTest, DonorDonatingOplogEntriesFulfillsCloneCascade) {
    ReshardingDonorPromises promises;
    promises.onDonorStateAdvanced(WithLock::withoutLock(), DonorStateEnum::kDonatingOplogEntries);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getInDonatingOplogEntriesFuture().isReady());
    ASSERT_FALSE(promises.getAllRecipientsDoneApplyingFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, DonorPreparingToBlockWritesFulfillsApplyingCascade) {
    ReshardingDonorPromises promises;
    promises.onDonorStateAdvanced(WithLock::withoutLock(), DonorStateEnum::kPreparingToBlockWrites);

    ASSERT_TRUE(promises.getAllRecipientsDoneCloningFuture().isReady());
    ASSERT_TRUE(promises.getInDonatingOplogEntriesFuture().isReady());
    ASSERT_TRUE(promises.getAllRecipientsDoneApplyingFuture().isReady());

    // The critical section is still being acquired; not yet fulfilled.
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getInBlockingWritesOrErrorFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, DonorBlockingWritesFulfillsOnlyInBlockingWritesOrError) {
    // _critSecWasAcquired is NOT fulfilled here: it tracks the in-memory critical-section
    // acquisition, which the donor's algorithm signals via emplaceCritSecWasAcquired(). State
    // advance alone is not enough (tests pause the on-disk transition without invalidating the
    // earlier in-memory acquisition).
    ReshardingDonorPromises promises;
    promises.onDonorStateAdvanced(WithLock::withoutLock(), DonorStateEnum::kBlockingWrites);

    ASSERT_TRUE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, DonorErrorFulfillsOnlyInBlockingWritesOrError) {
    ReshardingDonorPromises promises;
    promises.onDonorStateAdvanced(WithLock::withoutLock(), DonorStateEnum::kError);

    ASSERT_TRUE(promises.getInBlockingWritesOrErrorFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

TEST_F(ReshardingDonorPromisesTest, DonorDoneAloneDoesNotFulfillCritSecPromoted) {
    // Same as above for _critSecWasPromoted: it is driven by emplaceCritSecWasPromoted() at the
    // moment the promotion happens, not by state-advance.
    ReshardingDonorPromises promises;
    promises.onDonorStateAdvanced(WithLock::withoutLock(), DonorStateEnum::kDone);

    ASSERT_FALSE(promises.getCritSecWasPromotedFuture().isReady());
}

// ---- Explicit critical-section emplace hooks -------------------------------------------------

TEST_F(ReshardingDonorPromisesTest, EmplaceCritSecWasAcquiredFulfillsWithSuccess) {
    ReshardingDonorPromises promises;
    promises.emplaceCritSecWasAcquired(WithLock::withoutLock());

    ASSERT_TRUE(promises.getCritSecWasAcquiredFuture().isReady());
    ASSERT_OK(promises.getCritSecWasAcquiredFuture().getNoThrow());
}

TEST_F(ReshardingDonorPromisesTest, EmplaceCritSecWasPromotedFulfillsWithSuccess) {
    ReshardingDonorPromises promises;
    promises.emplaceCritSecWasPromoted(WithLock::withoutLock());

    ASSERT_TRUE(promises.getCritSecWasPromotedFuture().isReady());
    ASSERT_OK(promises.getCritSecWasPromotedFuture().getNoThrow());
}

TEST_F(ReshardingDonorPromisesTest, EmplaceCritSecWasPromotedIsIdempotentAfterError) {
    // Encodes the abort-path contract: setError() runs before the donor finishes promoting on
    // the abort branch, so a subsequent (e.g. recovery) emplaceCritSecWasPromoted() must not
    // throw and must not overwrite the error.
    ReshardingDonorPromises promises;
    Status err{ErrorCodes::ReshardCollectionAborted, "aborted"};
    promises.setError(WithLock::withoutLock(), err);
    promises.emplaceCritSecWasPromoted(WithLock::withoutLock());

    ASSERT_EQ(promises.getCritSecWasPromotedFuture().getNoThrow().code(),
              ErrorCodes::ReshardCollectionAborted);
}

// ---- setError(): broadcast -------------------------------------------------------------------

TEST_F(ReshardingDonorPromisesTest, SetErrorBroadcastsToEveryWorkflowPromise) {
    // Encodes the contract that DonorStateMachine::_runMandatoryCleanup relies on: a terminal
    // broadcast must unblock every consumer awaiting a workflow promise.
    ReshardingDonorPromises promises;
    Status err{ErrorCodes::InternalError, "error"};
    promises.setError(WithLock::withoutLock(), err);

    ASSERT_EQ(promises.getAllRecipientsDoneCloningFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
    ASSERT_EQ(promises.getAllRecipientsDoneApplyingFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
    ASSERT_EQ(promises.getInDonatingOplogEntriesFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
    ASSERT_EQ(promises.getInBlockingWritesOrErrorFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
    ASSERT_EQ(promises.getCritSecWasAcquiredFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
    ASSERT_EQ(promises.getCritSecWasPromotedFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
}

TEST_F(ReshardingDonorPromisesTest, SetErrorDoesNotOverwriteAlreadyFulfilledPromise) {
    ReshardingDonorPromises promises;
    promises.onDonorStateAdvanced(WithLock::withoutLock(), DonorStateEnum::kDonatingOplogEntries);
    promises.setError(WithLock::withoutLock(), Status{ErrorCodes::InternalError, "error"});

    ASSERT_OK(promises.getAllRecipientsDoneCloningFuture().getNoThrow());
    ASSERT_OK(promises.getInDonatingOplogEntriesFuture().getNoThrow());
    ASSERT_EQ(promises.getAllRecipientsDoneApplyingFuture().getNoThrow().code(),
              ErrorCodes::InternalError);
}

}  // namespace
}  // namespace mongo
