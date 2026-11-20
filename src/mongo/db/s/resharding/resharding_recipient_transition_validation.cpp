// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_recipient_transition_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/s/resharding/resharding_transition_validation_common.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mongo {
namespace {

using enum RecipientStateEnum;

using RecipientPreConstraint = TransitionConstraint<RecipientPreTransitionInput>;
using RecipientPostConstraint = TransitionConstraint<RecipientPostTransitionInput>;
using RecipientPreCheck = TransitionCheck<RecipientPreTransitionInput>;

bool allDonorsHaveMinFetchTimestamp(const RecipientTransitionSnapshot& snapshot) {
    const auto& donors = snapshot.options.getDonorShards();
    return std::all_of(donors.begin(), donors.end(), [](const auto& donor) {
        return isSetAndNonNull(donor.getMinFetchTimestamp());
    });
}

Status checkDonorsHaveMinFetchTimestamp(const std::vector<DonorShardFetchTimestamp>& donors) {
    for (const auto& donor : donors) {
        Status status =
            checkSetAndNonNull(str::stream() << "minFetchTimestamp of donor " << donor.getShardId(),
                               donor.getMinFetchTimestamp());
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

// The donors' minFetchTimestamps. The donors are matched by position: the immutable-fields rule
// rejects a change to the donor list before any edge validator runs.
Status checkDonorMinFetchTimestampsUnchanged(const RecipientTransitionSnapshot& before,
                                             const RecipientTransitionSnapshot& proposed) {
    const auto& beforeDonors = before.options.getDonorShards();
    const auto& proposedDonors = proposed.options.getDonorShards();
    if (beforeDonors.size() != proposedDonors.size()) {
        return transitionViolation(str::stream()
                                   << "donor count changed from " << beforeDonors.size() << " to "
                                   << proposedDonors.size());
    }
    for (size_t i = 0; i < beforeDonors.size(); ++i) {
        const std::string field = str::stream()
            << "minFetchTimestamp of donor " << beforeDonors[i].getShardId();
        Status status = checkUnchanged(field.c_str(),
                                       beforeDonors[i].getMinFetchTimestamp(),
                                       proposedDonors[i].getMinFetchTimestamp());
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

// Every clone detail: the scalars and the donors' minFetchTimestamps.
Status checkCloneDetailsUnchanged(const RecipientTransitionSnapshot& before,
                                  const RecipientTransitionSnapshot& proposed) {
    Status status =
        checkUnchanged("cloneTimestamp", before.cloneTimestamp, proposed.cloneTimestamp);
    if (!status.isOK()) {
        return status;
    }
    status = checkUnchanged(
        "approxDocumentsToCopy", before.approxDocumentsToCopy, proposed.approxDocumentsToCopy);
    if (!status.isOK()) {
        return status;
    }
    status =
        checkUnchanged("approxBytesToCopy", before.approxBytesToCopy, proposed.approxBytesToCopy);
    if (!status.isOK()) {
        return status;
    }
    status = checkUnchanged("startConfigTxnCloneTime",
                            before.startConfigTxnCloneTime,
                            proposed.startConfigTxnCloneTime);
    if (!status.isOK()) {
        return status;
    }
    return checkDonorMinFetchTimestampsUnchanged(before, proposed);
}

// Only the start is compared, as the monitor persists its own progress.
Status checkChangeStreamsMonitorStartUnchanged(const RecipientTransitionSnapshot& before,
                                               const RecipientTransitionSnapshot& proposed) {
    const auto start = [](const RecipientTransitionSnapshot& s) -> boost::optional<Timestamp> {
        if (!s.changeStreamsMonitor) {
            return boost::none;
        }
        return s.changeStreamsMonitor->getStartAtOperationTime();
    };
    return checkUnchanged("changeStreamsMonitor start", start(before), start(proposed));
}

bool donorShardIdsUnchanged(const std::vector<DonorShardFetchTimestamp>& before,
                            const std::vector<DonorShardFetchTimestamp>& proposed) {
    auto sameShard = [](const auto& a, const auto& b) {
        return a.getShardId() == b.getShardId();
    };
    return std::equal(before.begin(), before.end(), proposed.begin(), proposed.end(), sameShard);
}

// The options and performVerification are fixed for the operation, and the conditional rules read
// them from the proposed snapshot, so a change would let a transition waive a rule. The donors'
// minFetchTimestamps are not covered: the edge validators decide when they are set and that they
// then stay.
Status checkImmutableFields(const RecipientTransitionSnapshot& before,
                            const RecipientTransitionSnapshot& proposed) {
    auto withoutDonorShards = [](ReshardingRecipientOptions options) {
        options.setDonorShards({});
        return options.toBSON();
    };
    if (before.performVerification == proposed.performVerification &&
        withoutDonorShards(before.options).binaryEqual(withoutDonorShards(proposed.options)) &&
        donorShardIdsUnchanged(before.options.getDonorShards(),
                               proposed.options.getDonorShards())) {
        return Status::OK();
    }
    return transitionViolation(str::stream()
                               << "immutable fields changed: performVerification "
                               << before.performVerification << " -> "
                               << proposed.performVerification << ", options "
                               << before.options.toBSON() << " -> " << proposed.options.toBSON());
}

const RecipientPreConstraint kImmutableFieldsUnchanged{
    "immutableFieldsUnchanged", [](const RecipientPreTransitionInput& in) -> Status {
        return checkImmutableFields(in.before, in.proposed);
    }};

// The edges that advance the operation, from kAwaitingFetchTimestamp through kStrictConsistency,
// carry no abort reason. An abort is recorded by the edges into kError and kDone.
const RecipientPreConstraint kNoAbortReason{
    "noAbortReason", [](const RecipientPreTransitionInput& in) -> Status {
        Status status = checkNoAbortReason(in.before.ctx.getAbortReason());
        if (!status.isOK()) {
            return status;
        }
        return checkNoAbortReason(in.proposed.ctx.getAbortReason());
    }};

// The clone details are written once, by the transition into kCreatingCollection. A null clone
// timestamp counts as unset. The donors' minFetchTimestamps are not examined: the recipient
// document may already hold some when it is created. Every later edge must keep all of the clone
// details unchanged (checkCloneDetailsUnchanged).
const RecipientPreConstraint kCloneDetailsNotYetSet{
    "cloneDetailsNotYetSet", [](const RecipientPreTransitionInput& in) -> Status {
        const auto& before = in.before;
        std::string alreadySet;
        const auto note = [&](const char* field, bool isSet) {
            if (isSet) {
                alreadySet += (alreadySet.empty() ? "" : ", ") + std::string{field};
            }
        };
        note("cloneTimestamp", isSetAndNonNull(before.cloneTimestamp));
        note("approxDocumentsToCopy", before.approxDocumentsToCopy.has_value());
        note("approxBytesToCopy", before.approxBytesToCopy.has_value());
        note("startConfigTxnCloneTime", before.startConfigTxnCloneTime.has_value());
        if (alreadySet.empty()) {
            return Status::OK();
        }
        return transitionViolation("clone details are already set: " + alreadySet);
    }};

// Written by kAwaitingFetchTimestamp -> kCreatingCollection.
const RecipientPreConstraint kHasCloneTimestamp{
    "hasCloneTimestamp", [](const RecipientPreTransitionInput& in) -> Status {
        return checkSetAndNonNull("cloneTimestamp", in.proposed.cloneTimestamp);
    }};

const RecipientPreConstraint kHasDonorShards{
    "hasDonorShards", [](const RecipientPreTransitionInput& in) -> Status {
        if (!in.proposed.options.getDonorShards().empty()) {
            return Status::OK();
        }
        return transitionViolation("there are no donor shards");
    }};

const RecipientPreConstraint kDonorsHaveMinFetchTimestamp{
    "donorsHaveMinFetchTimestamp", [](const RecipientPreTransitionInput& in) -> Status {
        return checkDonorsHaveMinFetchTimestamp(in.proposed.options.getDonorShards());
    }};

// The coordinator derives the clone timestamp as the highest minFetchTimestamp. Not evaluated
// unless every value is set; the rules for missing values report those cases.
const RecipientPreConstraint kCloneTimestampIsHighestMinFetchTimestamp{
    "cloneTimestampIsHighestMinFetchTimestamp",
    [](const RecipientPreTransitionInput& in) -> Status {
        const auto& snapshot = in.proposed;
        const auto& donors = snapshot.options.getDonorShards();
        if (donors.empty() || !isSetAndNonNull(snapshot.cloneTimestamp) ||
            !allDonorsHaveMinFetchTimestamp(snapshot)) {
            return Status::OK();
        }
        const auto highest =
            std::max_element(donors.begin(), donors.end(), [](const auto& a, const auto& b) {
                return *a.getMinFetchTimestamp() < *b.getMinFetchTimestamp();
            });
        const auto& highestMinFetchTimestamp = *highest->getMinFetchTimestamp();
        if (*snapshot.cloneTimestamp == highestMinFetchTimestamp) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "cloneTimestamp " << *snapshot.cloneTimestamp
                                   << " != highest minFetchTimestamp " << highestMinFetchTimestamp
                                   << " of donor " << highest->getShardId());
    }};

const RecipientPreConstraint kHasValidApproxCopyCounts{
    "hasValidApproxCopyCounts", [](const RecipientPreTransitionInput& in) -> Status {
        Status status =
            checkNonNegative("approxDocumentsToCopy", in.proposed.approxDocumentsToCopy);
        if (!status.isOK()) {
            return status;
        }
        return checkNonNegative("approxBytesToCopy", in.proposed.approxBytesToCopy);
    }};

const RecipientPreConstraint kHasStartConfigTxnCloneTime{
    "hasStartConfigTxnCloneTime", [](const RecipientPreTransitionInput& in) -> Status {
        if (in.proposed.startConfigTxnCloneTime) {
            return Status::OK();
        }
        return transitionViolation("startConfigTxnCloneTime is unset");
    }};

const RecipientPreConstraint kCloneDetailsUnchanged{
    "cloneDetailsUnchanged", [](const RecipientPreTransitionInput& in) -> Status {
        return checkCloneDetailsUnchanged(in.before, in.proposed);
    }};

// A recipient reaches kDone without passing through kError when the coordinator aborts, and then
// records the coordinator's abort as the reason.
const RecipientPreConstraint kNewAbortReasonIsCoordinatorAbort{
    "newAbortReasonIsCoordinatorAbort", [](const RecipientPreTransitionInput& in) -> Status {
        const auto& proposed = in.proposed.ctx.getAbortReason();
        if (in.before.ctx.getAbortReason() || !proposed ||
            proposed->getIntField("code") ==
                static_cast<int>(ErrorCodes::ReshardCollectionAborted)) {
            return Status::OK();
        }
        return transitionViolation(str::stream() << "abort reason " << *proposed
                                                 << " is not the coordinator's abort");
    }};

const RecipientPreConstraint kChangeStreamsMonitorStartUnchanged{
    "changeStreamsMonitorStartUnchanged", [](const RecipientPreTransitionInput& in) -> Status {
        return checkChangeStreamsMonitorStartUnchanged(in.before, in.proposed);
    }};

const RecipientPreConstraint kCloningTotalsUnchanged{
    "cloningTotalsUnchanged", [](const RecipientPreTransitionInput& in) -> Status {
        Status status = checkUnchanged("numDocumentsCloned",
                                       in.before.ctx.getNumDocumentsCloned(),
                                       in.proposed.ctx.getNumDocumentsCloned());
        if (!status.isOK()) {
            return status;
        }
        return checkUnchanged("totalDocumentSize",
                              in.before.ctx.getTotalDocumentSize(),
                              in.proposed.ctx.getTotalDocumentSize());
    }};

// Recorded on entering kApplying and kept afterwards.
const RecipientPreConstraint kHasCloningTotalsWhenVerifying{
    "hasCloningTotalsWhenVerifying", [](const RecipientPreTransitionInput& in) -> Status {
        const auto& proposed = in.proposed;
        if (!proposed.performVerification) {
            return Status::OK();
        }
        Status status =
            checkNonNegative("numDocumentsCloned", proposed.ctx.getNumDocumentsCloned());
        if (!status.isOK()) {
            return status;
        }
        return checkNonNegative("totalDocumentSize", proposed.ctx.getTotalDocumentSize());
    }};

// Started on entering kApplying if and only if the operation verifies and applies oplog entries,
// as in _updateRecipientDocument().
const RecipientPreConstraint kChangeStreamsMonitorMatchesOptions{
    "changeStreamsMonitorMatchesOptions", [](const RecipientPreTransitionInput& in) -> Status {
        const auto& proposed = in.proposed;
        const bool skipCloningAndApplying = proposed.options.getSkipCloningAndApplying();
        const bool expected = proposed.performVerification && !skipCloningAndApplying;
        if (proposed.changeStreamsMonitor.has_value() == expected) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "change streams monitor is "
                                   << (proposed.changeStreamsMonitor ? "present" : "absent")
                                   << " but performVerification is " << proposed.performVerification
                                   << " and skipCloningAndApplying is " << skipCloningAndApplying);
    }};

// A null start would begin the monitor at the start of the oplog.
const RecipientPreConstraint kChangeStreamsMonitorHasStart{
    "changeStreamsMonitorHasStart", [](const RecipientPreTransitionInput& in) -> Status {
        const auto& monitor = in.proposed.changeStreamsMonitor;
        if (!monitor) {
            return Status::OK();
        }
        return checkSetAndNonNull("change streams monitor start",
                                  monitor->getStartAtOperationTime());
    }};

const RecipientPreConstraint kNoChangeStreamsMonitorYet{
    "noChangeStreamsMonitorYet", [](const RecipientPreTransitionInput& in) -> Status {
        if (!in.before.changeStreamsMonitor) {
            return Status::OK();
        }
        return transitionViolation("change streams monitor is already set before applying");
    }};

// A new monitor starts empty, with no resume token: a token would skip events that the initial
// delta does not account for.
const RecipientPreConstraint kChangeStreamsMonitorStartsEmpty{
    "changeStreamsMonitorStartsEmpty", [](const RecipientPreTransitionInput& in) -> Status {
        const auto& monitor = in.proposed.changeStreamsMonitor;
        if (!monitor ||
            (monitor->getDocumentsDelta() == 0 && !monitor->getCompleted() &&
             !monitor->getResumeToken())) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "change streams monitor starts with documentsDelta "
                                   << monitor->getDocumentsDelta() << ", completed "
                                   << monitor->getCompleted() << " and resume token "
                                   << (monitor->getResumeToken() ? "set" : "unset"));
    }};

// A mismatch means the edge would be checked against the wrong source state.
const RecipientPreConstraint kPersistedStateMatchesInMemory{
    "persistedStateMatchesInMemory", [](const RecipientPreTransitionInput& in) -> Status {
        if (!in.outgoingPhase || *in.outgoingPhase == in.before.ctx.getState()) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "persisted state " << idl::serialize(in.before.ctx.getState())
                                   << " != in-memory state " << idl::serialize(*in.outgoingPhase));
    }};

const RecipientPostConstraint kDurableMatchesProposed{
    "durableMatchesProposed", [](const RecipientPostTransitionInput& in) -> Status {
        Status status =
            checkBsonMatches("durable context", in.durable.ctx.toBSON(), in.proposed.ctx.toBSON());
        if (!status.isOK()) {
            return status;
        }
        status = checkCloneDetailsUnchanged(in.durable, in.proposed);
        if (!status.isOK()) {
            return transitionViolation(
                str::stream() << "durable clone details differ from proposed: " << status.reason());
        }
        status = checkBsonMatches(
            "durable options", in.durable.options.toBSON(), in.proposed.options.toBSON());
        if (!status.isOK()) {
            return status;
        }
        if (in.durable.performVerification != in.proposed.performVerification) {
            return transitionViolation(
                str::stream() << "durable performVerification " << in.durable.performVerification
                              << " != proposed " << in.proposed.performVerification);
        }
        status = checkChangeStreamsMonitorStartUnchanged(in.durable, in.proposed);
        if (!status.isOK()) {
            return transitionViolation(str::stream() << "durable change streams monitor differs "
                                                        "from proposed: "
                                                     << status.reason());
        }
        return Status::OK();
    }};

const RecipientPostConstraint kInMemoryMatchesProposed{
    "inMemoryMatchesProposed", [](const RecipientPostTransitionInput& in) -> Status {
        return checkBsonMatches(
            "in-memory context", in.inMemory.toBSON(), in.proposed.ctx.toBSON());
    }};

RecipientPreCheck preCheck(const RecipientPreTransitionInput& in) {
    return makePreTransitionCheck(in, in.before.ctx.getState(), in.proposed.ctx.getState());
}

// A change to the donor list is rejected by the immutable-fields rule before this validator runs.
ConstraintResult validateEnteringCreatingCollection(const RecipientPreTransitionInput& in) {
    return preCheck(in)
        .require(kNoAbortReason)
        .require(kCloneDetailsNotYetSet)
        .require(kHasCloneTimestamp)
        .require(kHasDonorShards)
        .require(kDonorsHaveMinFetchTimestamp)
        .require(kCloneTimestampIsHighestMinFetchTimestamp)
        .require(kHasValidApproxCopyCounts)
        .require(kHasStartConfigTxnCloneTime)
        .result();
}

// Writes nothing new: the clone details must be present and unchanged.
ConstraintResult validateCarryingCloneDetails(const RecipientPreTransitionInput& in) {
    return preCheck(in)
        .require(kNoAbortReason)
        .require(kHasCloneTimestamp)
        .require(kHasDonorShards)
        .require(kDonorsHaveMinFetchTimestamp)
        .require(kCloneTimestampIsHighestMinFetchTimestamp)
        .require(kHasValidApproxCopyCounts)
        .require(kHasStartConfigTxnCloneTime)
        .require(kCloneDetailsUnchanged)
        .result();
}

ConstraintResult validateEnteringApplying(const RecipientPreTransitionInput& in) {
    return preCheck(in)
        .require(kNoAbortReason)
        .require(kHasCloneTimestamp)
        .require(kHasDonorShards)
        .require(kDonorsHaveMinFetchTimestamp)
        .require(kCloneTimestampIsHighestMinFetchTimestamp)
        .require(kHasValidApproxCopyCounts)
        .require(kHasStartConfigTxnCloneTime)
        .require(kCloneDetailsUnchanged)
        .require(kNoChangeStreamsMonitorYet)
        .require(kChangeStreamsMonitorStartsEmpty)
        .require(kHasCloningTotalsWhenVerifying)
        .require(kChangeStreamsMonitorMatchesOptions)
        .require(kChangeStreamsMonitorHasStart)
        .result();
}

// Nothing new is written. Only the monitor's start is compared, not its progress.
ConstraintResult validateEnteringStrictConsistency(const RecipientPreTransitionInput& in) {
    return preCheck(in)
        .require(kNoAbortReason)
        .require(kHasCloneTimestamp)
        .require(kHasDonorShards)
        .require(kDonorsHaveMinFetchTimestamp)
        .require(kCloneTimestampIsHighestMinFetchTimestamp)
        .require(kHasValidApproxCopyCounts)
        .require(kHasStartConfigTxnCloneTime)
        .require(kHasCloningTotalsWhenVerifying)
        .require(kChangeStreamsMonitorMatchesOptions)
        .require(kChangeStreamsMonitorHasStart)
        .require(kCloneDetailsUnchanged)
        .require(kChangeStreamsMonitorStartUnchanged)
        .require(kCloningTotalsUnchanged)
        .result();
}

// Neither the cloning totals nor the monitor's completion is compared. The service refreshes the
// totals from the temporary collection before this write, and the coordinator commits without
// waiting for a monitor once its verification deadline passes.
ConstraintResult validateEnteringDone(const RecipientPreTransitionInput& in) {
    return preCheck(in)
        .require(kCloneDetailsUnchanged)
        .require(kNewAbortReasonIsCoordinatorAbort)
        .require(kChangeStreamsMonitorStartUnchanged)
        .result();
}

using RecipientTransitionEdge = std::pair<RecipientStateEnum, RecipientStateEnum>;
using RecipientPreTransitionValidator = ConstraintResult (*)(const RecipientPreTransitionInput&);

// Legal transitions and their validators. Documents start in kAwaitingFetchTimestamp, so kUnused is
// not a source. kStrictConsistency -> kError is absent: the error path asserts it is never reached.
const std::map<RecipientTransitionEdge, RecipientPreTransitionValidator>&
recipientTransitionValidators() {
    static StaticImmortal validators =
        std::map<RecipientTransitionEdge, RecipientPreTransitionValidator>{
            // Forward edges.
            {{kAwaitingFetchTimestamp, kCreatingCollection}, &validateEnteringCreatingCollection},
            {{kCreatingCollection, kCloning}, &validateCarryingCloneDetails},
            {{kCloning, kBuildingIndex}, &validateCarryingCloneDetails},
            {{kBuildingIndex, kApplying}, &validateEnteringApplying},
            {{kApplying, kStrictConsistency}, &validateEnteringStrictConsistency},
            // Whether the coordinator decided to commit is not examined: that needs a coordinator
            // read.
            {{kStrictConsistency, kDone}, &validateEnteringDone},
        };
    return *validators;
}

}  // namespace

// A persisted/in-memory mismatch, or a change to the immutable fields, is reported alone: the edge
// validators would check the wrong source state, or evaluate their conditional rules against the
// wrong options.
ConstraintResult validateRecipientPreTransition(const RecipientPreTransitionInput& input) {
    const auto from = input.outgoingPhase.value_or(input.before.ctx.getState());
    const auto to = input.proposed.ctx.getState();
    const auto checkAlone = [&](const RecipientPreConstraint& constraint) {
        return makePreTransitionCheck(input, from, to).require(constraint).result();
    };
    if (from != input.before.ctx.getState()) {
        return checkAlone(kPersistedStateMatchesInMemory);
    }
    if (auto immutableFields = checkAlone(kImmutableFieldsUnchanged);
        !immutableFields.status.isOK()) {
        return immutableFields;
    }

    const auto& validators = recipientTransitionValidators();
    const auto it = validators.find({from, to});
    if (it == validators.end()) {
        return notAllowedTransition(from, to);
    }
    return it->second(input);
}

ConstraintResult validateRecipientPostTransition(const RecipientPostTransitionInput& input) {
    return TransitionCheck<RecipientPostTransitionInput>(
               std::string{str::stream()
                           << "transition to " << idl::serialize(input.proposed.ctx.getState())},
               input,
               ErrorCodes::ReshardingPostconditionFailure)
        .require(kDurableMatchesProposed)
        .require(kInMemoryMatchesProposed)
        .result();
}

}  // namespace mongo
