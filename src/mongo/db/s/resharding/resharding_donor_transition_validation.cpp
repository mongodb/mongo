// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_donor_transition_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/resharding_transition_validation_common.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mongo {
namespace {

using enum DonorStateEnum;

using DonorPreConstraint = TransitionConstraint<DonorPreTransitionInput>;
using DonorPostConstraint = TransitionConstraint<DonorPostTransitionInput>;

const DonorPreConstraint kHasMinFetchTimestamp{
    "hasMinFetchTimestamp", [](const DonorPreTransitionInput& in) -> Status {
        return checkSetAndNonNull("minFetchTimestamp", in.proposed.getMinFetchTimestamp());
    }};

const DonorPreConstraint kHasValidCloneStats{
    "hasValidCloneStats", [](const DonorPreTransitionInput& in) -> Status {
        Status status = checkNonNegative("bytesToClone", in.proposed.getBytesToClone());
        if (!status.isOK()) {
            return status;
        }
        status = checkNonNegative("documentsToClone", in.proposed.getDocumentsToClone());
        if (!status.isOK()) {
            return status;
        }
        return checkNonNegative("indexCount", in.proposed.getIndexCount());
    }};

const DonorPreConstraint kNoAbortReason{"noAbortReason",
                                        [](const DonorPreTransitionInput& in) -> Status {
                                            return checkNoAbortReason(in.proposed.getAbortReason());
                                        }};

const DonorPreConstraint kHasAbortReason{
    "hasAbortReason", [](const DonorPreTransitionInput& in) -> Status {
        return checkHasAbortReason(in.proposed.getAbortReason());
    }};

const DonorPreConstraint kCloneDetailsUnchanged{
    "cloneDetailsUnchanged", [](const DonorPreTransitionInput& in) -> Status {
        Status status = checkUnchanged("minFetchTimestamp",
                                       in.before.getMinFetchTimestamp(),
                                       in.proposed.getMinFetchTimestamp());
        if (!status.isOK()) {
            return status;
        }
        status = checkUnchanged(
            "bytesToClone", in.before.getBytesToClone(), in.proposed.getBytesToClone());
        if (!status.isOK()) {
            return status;
        }
        status = checkUnchanged(
            "documentsToClone", in.before.getDocumentsToClone(), in.proposed.getDocumentsToClone());
        if (!status.isOK()) {
            return status;
        }
        return checkUnchanged("indexCount", in.before.getIndexCount(), in.proposed.getIndexCount());
    }};

const DonorPreConstraint kExistingAbortReasonUnchanged{
    "existingAbortReasonUnchanged", [](const DonorPreTransitionInput& in) -> Status {
        return checkAbortReasonUnchanged(in.before.getAbortReason(), in.proposed.getAbortReason());
    }};

const DonorPreConstraint kAllRecipientsDoneCloning{
    "allRecipientsDoneCloning", [](const DonorPreTransitionInput& in) -> Status {
        if (in.facts.allRecipientsDoneCloning) {
            return Status::OK();
        }
        return transitionViolation("recipients have not all reported done cloning");
    }};

const DonorPreConstraint kChangeStreamsMonitorStartedIfVerifying{
    "changeStreamsMonitorStartedIfVerifying", [](const DonorPreTransitionInput& in) -> Status {
        if (!in.facts.performVerification || in.facts.changeStreamsMonitorStarted) {
            return Status::OK();
        }
        return transitionViolation("change streams monitor has not started");
    }};

const DonorPreConstraint kAllRecipientsDoneApplying{
    "allRecipientsDoneApplying", [](const DonorPreTransitionInput& in) -> Status {
        if (in.facts.allRecipientsDoneApplying) {
            return Status::OK();
        }
        return transitionViolation("recipients have not all reported done applying");
    }};

const DonorPostConstraint kDurableMatchesProposed{
    "durableMatchesProposed", [](const DonorPostTransitionInput& in) -> Status {
        return checkBsonMatches("durable", in.durable.toBSON(), in.proposed.toBSON());
    }};

TransitionCheck<DonorPreTransitionInput> preCheck(const DonorPreTransitionInput& in) {
    return makePreTransitionCheck(in, in.before.getState(), in.proposed.getState());
}

ConstraintResult validateEnteringDonatingInitialData(const DonorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasMinFetchTimestamp)
        .require(kHasValidCloneStats)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringDonatingOplogEntries(const DonorPreTransitionInput& in) {
    return preCheck(in)
        .require(kCloneDetailsUnchanged)
        .require(kNoAbortReason)
        .require(kAllRecipientsDoneCloning)
        .require(kChangeStreamsMonitorStartedIfVerifying)
        .result();
}

ConstraintResult validateEnteringPreparingToBlockWrites(const DonorPreTransitionInput& in) {
    return preCheck(in)
        .require(kCloneDetailsUnchanged)
        .require(kNoAbortReason)
        .require(kAllRecipientsDoneApplying)
        .result();
}

ConstraintResult validateEnteringBlockingWrites(const DonorPreTransitionInput& in) {
    return preCheck(in).require(kCloneDetailsUnchanged).require(kNoAbortReason).result();
}

ConstraintResult validateEnteringError(const DonorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasAbortReason)
        .require(kExistingAbortReasonUnchanged)
        .require(kCloneDetailsUnchanged)
        .result();
}

ConstraintResult validateEnteringDone(const DonorPreTransitionInput& in) {
    return preCheck(in)
        .require(kCloneDetailsUnchanged)
        .require(kExistingAbortReasonUnchanged)
        .result();
}

const DonorPostConstraint kInDonatingOplogEntriesFulfilled{
    "inDonatingOplogEntriesFulfilled", [](const DonorPostTransitionInput& in) -> Status {
        if (in.facts.inDonatingOplogEntries) {
            return Status::OK();
        }
        return transitionViolation("inDonatingOplogEntries promise is not fulfilled");
    }};

const DonorPostConstraint kAllRecipientsDoneApplyingFulfilled{
    "allRecipientsDoneApplyingFulfilled", [](const DonorPostTransitionInput& in) -> Status {
        if (in.facts.allRecipientsDoneApplying) {
            return Status::OK();
        }
        return transitionViolation("allRecipientsDoneApplying promise is not fulfilled");
    }};

const DonorPostConstraint kInBlockingWritesOrErrorFulfilled{
    "inBlockingWritesOrErrorFulfilled", [](const DonorPostTransitionInput& in) -> Status {
        if (in.facts.inBlockingWritesOrError) {
            return Status::OK();
        }
        return transitionViolation("inBlockingWritesOrError promise is not fulfilled");
    }};

TransitionCheck<DonorPostTransitionInput> postCheck(const DonorPostTransitionInput& in) {
    return makePostTransitionCheck(in, in.before.getState(), in.proposed.getState());
}

ConstraintResult validatePostInstalled(const DonorPostTransitionInput& in) {
    return postCheck(in).require(kDurableMatchesProposed).result();
}

ConstraintResult validatePostDonatingOplogEntries(const DonorPostTransitionInput& in) {
    return postCheck(in)
        .require(kDurableMatchesProposed)
        .require(kInDonatingOplogEntriesFulfilled)
        .result();
}

ConstraintResult validatePostPreparingToBlockWrites(const DonorPostTransitionInput& in) {
    return postCheck(in)
        .require(kDurableMatchesProposed)
        .require(kAllRecipientsDoneApplyingFulfilled)
        .result();
}

ConstraintResult validatePostBlockingWritesOrError(const DonorPostTransitionInput& in) {
    return postCheck(in)
        .require(kDurableMatchesProposed)
        .require(kInBlockingWritesOrErrorFulfilled)
        .result();
}

using DonorTransitionEdge = std::pair<DonorStateEnum, DonorStateEnum>;
using DonorPreTransitionValidator = ConstraintResult (*)(const DonorPreTransitionInput&);
using DonorPostTransitionValidator = ConstraintResult (*)(const DonorPostTransitionInput&);

struct DonorTransitionValidators {
    DonorPreTransitionValidator pre;
    DonorPostTransitionValidator post = &validatePostInstalled;
};

const std::map<DonorTransitionEdge, DonorTransitionValidators>& donorTransitionValidators() {
    static StaticImmortal validators = std::map<DonorTransitionEdge, DonorTransitionValidators>{
        // Forward edges.
        {{kPreparingToDonate, kDonatingInitialData}, {&validateEnteringDonatingInitialData}},
        {{kDonatingInitialData, kDonatingOplogEntries},
         {&validateEnteringDonatingOplogEntries, &validatePostDonatingOplogEntries}},
        {{kDonatingOplogEntries, kPreparingToBlockWrites},
         {&validateEnteringPreparingToBlockWrites, &validatePostPreparingToBlockWrites}},
        {{kPreparingToBlockWrites, kBlockingWrites},
         {&validateEnteringBlockingWrites, &validatePostBlockingWritesOrError}},
        // kError edges.
        {{kPreparingToDonate, kError},
         {&validateEnteringError, &validatePostBlockingWritesOrError}},
        {{kDonatingInitialData, kError},
         {&validateEnteringError, &validatePostBlockingWritesOrError}},
        {{kDonatingOplogEntries, kError},
         {&validateEnteringError, &validatePostBlockingWritesOrError}},
        {{kPreparingToBlockWrites, kError},
         {&validateEnteringError, &validatePostBlockingWritesOrError}},
        // kDone edges.
        {{kPreparingToDonate, kDone}, {&validateEnteringDone}},
        {{kDonatingInitialData, kDone}, {&validateEnteringDone}},
        {{kDonatingOplogEntries, kDone}, {&validateEnteringDone}},
        {{kPreparingToBlockWrites, kDone}, {&validateEnteringDone}},
        {{kError, kDone}, {&validateEnteringDone}},
        {{kBlockingWrites, kDone}, {&validateEnteringDone}},
    };
    return *validators;
}

}  // namespace

ConstraintResult validateDonorPreTransition(const DonorPreTransitionInput& input) {
    const auto& validators = donorTransitionValidators();
    const auto it = validators.find({input.before.getState(), input.proposed.getState()});
    if (it == validators.end()) {
        return notAllowedTransition(input.before.getState(), input.proposed.getState());
    }
    return it->second.pre(input);
}

ConstraintResult validateDonorPostTransition(const DonorPostTransitionInput& input) {
    const auto& validators = donorTransitionValidators();
    const auto it = validators.find({input.before.getState(), input.proposed.getState()});
    if (it == validators.end()) {
        return notAllowedTransition(input.before.getState(),
                                    input.proposed.getState(),
                                    ErrorCodes::ReshardingPostconditionFailure);
    }
    return it->second.post(input);
}

}  // namespace mongo
