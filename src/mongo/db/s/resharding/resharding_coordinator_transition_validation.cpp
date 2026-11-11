// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_coordinator_transition_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/resharding_transition_validation_common.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mongo {
namespace {

using enum CoordinatorStateEnum;

using CoordinatorPreConstraint = TransitionConstraint<CoordinatorPreTransitionInput>;

template <typename Entry>
Status checkIdsUnchanged(const char* field,
                         const std::vector<Entry>& before,
                         const std::vector<Entry>& proposed) {
    if (before.size() != proposed.size()) {
        return transitionViolation(str::stream() << field << " size changed from " << before.size()
                                                 << " to " << proposed.size());
    }
    if (proposed.empty()) {
        return transitionViolation(str::stream() << field << " is empty");
    }
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i].getId() != proposed[i].getId()) {
            return transitionViolation(str::stream()
                                       << field << "[" << i << "] changed from "
                                       << before[i].getId() << " to " << proposed[i].getId());
        }
    }
    return Status::OK();
}

template <typename Entry>
Status checkNoEntries(const char* field, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        return Status::OK();
    }
    return transitionViolation(str::stream()
                               << field << " has " << entries.size() << " unexpected entries");
}

template <typename Entry>
Status checkUniqueEntries(const char* field, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        return transitionViolation(str::stream() << field << " is empty");
    }
    std::set<ShardId> ids;
    for (const auto& entry : entries) {
        if (!ids.insert(entry.getId()).second) {
            return transitionViolation(str::stream()
                                       << field << " has duplicate shard " << entry.getId());
        }
    }
    return Status::OK();
}

template <typename Entry, typename StateEnum>
Status checkParticipantsAtLeast(const char* role,
                                const std::vector<Entry>& entries,
                                StateEnum minimum) {
    if (entries.empty()) {
        return transitionViolation(str::stream() << role << " list is empty");
    }
    for (const auto& entry : entries) {
        const auto state = entry.getMutableState().getState();
        if (state < minimum) {
            return transitionViolation(str::stream()
                                       << role << " " << entry.getId() << " is in state "
                                       << idl::serialize(state) << ", expected at least "
                                       << idl::serialize(minimum));
        }
    }
    return Status::OK();
}

template <typename Entry, typename StateEnum>
Status checkParticipantsEqual(const char* role,
                              const std::vector<Entry>& entries,
                              StateEnum expected) {
    if (entries.empty()) {
        return transitionViolation(str::stream() << role << " list is empty");
    }
    for (const auto& entry : entries) {
        const auto state = entry.getMutableState().getState();
        if (state != expected) {
            return transitionViolation(str::stream() << role << " " << entry.getId()
                                                     << " is in state " << idl::serialize(state)
                                                     << ", expected " << idl::serialize(expected));
        }
    }
    return Status::OK();
}

template <typename Entry>
Status checkNoParticipantAbortReason(const char* role, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        return transitionViolation(str::stream() << role << " list is empty");
    }
    for (const auto& entry : entries) {
        if (const auto& abortReason = entry.getMutableState().getAbortReason()) {
            return transitionViolation(str::stream() << "unexpected abort reason on " << role << " "
                                                     << entry.getId() << ": " << *abortReason);
        }
    }
    return Status::OK();
}

Status checkAllParticipantsDone(const ReshardingCoordinatorDocument& doc) {
    Status status = checkParticipantsEqual("donor", doc.getDonorShards(), DonorStateEnum::kDone);
    if (!status.isOK()) {
        return status;
    }
    return checkParticipantsEqual("recipient", doc.getRecipientShards(), RecipientStateEnum::kDone);
}

/**
 * Participant progress rules are skipped once any participant has aborted: the operation is
 * already aborting, and participant invariants belong to the participant validators.
 */
bool anyParticipantAborted(const ReshardingCoordinatorDocument& doc) {
    const auto aborted = [](const auto& entry) {
        return entry.getMutableState().getAbortReason().has_value();
    };
    return std::any_of(doc.getDonorShards().begin(), doc.getDonorShards().end(), aborted) ||
        std::any_of(doc.getRecipientShards().begin(), doc.getRecipientShards().end(), aborted);
}

const CoordinatorPreConstraint kMetadataUnchanged{
    "metadataUnchanged", [](const CoordinatorPreTransitionInput& in) -> Status {
        const auto before = in.before.getCommonReshardingMetadata().toBSON();
        const auto proposed = in.proposed.getCommonReshardingMetadata().toBSON();
        if (before.binaryEqual(proposed)) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "metadata changed from " << before << " to " << proposed);
    }};

const CoordinatorPreConstraint kExistingAbortReasonUnchanged{
    "existingAbortReasonUnchanged", [](const CoordinatorPreTransitionInput& in) -> Status {
        return checkAbortReasonUnchanged(in.before.getAbortReason(), in.proposed.getAbortReason());
    }};

const CoordinatorPreConstraint kParticipantIdsUnchanged{
    "participantIdsUnchanged", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (in.before.getState() < kPreparingToDonate) {
            return Status::OK();
        }
        Status status = checkIdsUnchanged(
            "donorShards", in.before.getDonorShards(), in.proposed.getDonorShards());
        if (!status.isOK()) {
            return status;
        }
        return checkIdsUnchanged(
            "recipientShards", in.before.getRecipientShards(), in.proposed.getRecipientShards());
    }};

const CoordinatorPreConstraint kNoShardEntries{
    "noShardEntries", [](const CoordinatorPreTransitionInput& in) -> Status {
        Status status = checkNoEntries("donorShards", in.proposed.getDonorShards());
        if (!status.isOK()) {
            return status;
        }
        return checkNoEntries("recipientShards", in.proposed.getRecipientShards());
    }};

const CoordinatorPreConstraint kNoCloneTimestamp{
    "noCloneTimestamp", [](const CoordinatorPreTransitionInput& in) -> Status {
        const auto& cloneTimestamp = in.proposed.getCloneTimestamp();
        if (!cloneTimestamp) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "unexpected cloneTimestamp: " << *cloneTimestamp);
    }};

const CoordinatorPreConstraint kNoAbortReason{
    "noAbortReason", [](const CoordinatorPreTransitionInput& in) -> Status {
        return checkNoAbortReason(in.proposed.getAbortReason());
    }};

const CoordinatorPreConstraint kHasAbortReason{
    "hasAbortReason", [](const CoordinatorPreTransitionInput& in) -> Status {
        return checkHasAbortReason(in.proposed.getAbortReason());
    }};

const CoordinatorPreConstraint kHasParticipants{
    "hasParticipants", [](const CoordinatorPreTransitionInput& in) -> Status {
        Status status = checkUniqueEntries("donorShards", in.proposed.getDonorShards());
        if (!status.isOK()) {
            return status;
        }
        return checkUniqueEntries("recipientShards", in.proposed.getRecipientShards());
    }};

const CoordinatorPreConstraint kParticipantsUnused{
    "participantsUnused", [](const CoordinatorPreTransitionInput& in) -> Status {
        Status status =
            checkParticipantsEqual("donor", in.proposed.getDonorShards(), DonorStateEnum::kUnused);
        if (!status.isOK()) {
            return status;
        }
        return checkParticipantsEqual(
            "recipient", in.proposed.getRecipientShards(), RecipientStateEnum::kUnused);
    }};

const CoordinatorPreConstraint kPresetChunksAndZonesCleared{
    "presetChunksAndZonesCleared", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (in.proposed.getPresetReshardedChunks()) {
            return transitionViolation("presetReshardedChunks is set");
        }
        if (in.proposed.getZones()) {
            return transitionViolation("zones is set");
        }
        return Status::OK();
    }};

const CoordinatorPreConstraint kAllDonorsReportedMinFetchTimestamp{
    "allDonorsReportedMinFetchTimestamp", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (anyParticipantAborted(in.proposed)) {
            return Status::OK();
        }
        const auto& donors = in.proposed.getDonorShards();
        if (donors.empty()) {
            return transitionViolation("donorShards is empty");
        }
        Status status =
            checkParticipantsAtLeast("donor", donors, DonorStateEnum::kDonatingInitialData);
        if (!status.isOK()) {
            return status;
        }
        for (const auto& donor : donors) {
            if (!donor.getMutableState().getMinFetchTimestamp()) {
                return transitionViolation(str::stream() << "donor " << donor.getId()
                                                         << " minFetchTimestamp is unset");
            }
        }
        return Status::OK();
    }};

const CoordinatorPreConstraint kCloneTimestampIsHighestMinFetch{
    "cloneTimestampIsHighestMinFetch", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (anyParticipantAborted(in.proposed)) {
            return Status::OK();
        }
        const auto& cloneTimestamp = in.proposed.getCloneTimestamp();
        if (!cloneTimestamp) {
            return transitionViolation("cloneTimestamp is unset");
        }
        // Donors without a minFetchTimestamp are reported by allDonorsReportedMinFetchTimestamp.
        boost::optional<Timestamp> highest;
        for (const auto& donor : in.proposed.getDonorShards()) {
            const auto& minFetchTimestamp = donor.getMutableState().getMinFetchTimestamp();
            if (minFetchTimestamp && (!highest || *minFetchTimestamp > *highest)) {
                highest = *minFetchTimestamp;
            }
        }
        if (!highest) {
            return transitionViolation("no donor has a minFetchTimestamp");
        }
        if (*cloneTimestamp == *highest) {
            return Status::OK();
        }
        return transitionViolation(str::stream() << "cloneTimestamp " << *cloneTimestamp
                                                 << " != highest minFetchTimestamp " << *highest);
    }};

const CoordinatorPreConstraint kHasApproxCopySize{
    "hasApproxCopySize", [](const CoordinatorPreTransitionInput& in) -> Status {
        Status status = checkNonNegative("approxBytesToCopy", in.proposed.getApproxBytesToCopy());
        if (!status.isOK()) {
            return status;
        }
        return checkNonNegative("approxDocumentsToCopy", in.proposed.getApproxDocumentsToCopy());
    }};

const CoordinatorPreConstraint kAllRecipientsApplying{
    "allRecipientsApplying", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (anyParticipantAborted(in.proposed)) {
            return Status::OK();
        }
        return checkParticipantsAtLeast(
            "recipient", in.proposed.getRecipientShards(), RecipientStateEnum::kApplying);
    }};

const CoordinatorPreConstraint kRecipientsUnused{
    "recipientsUnused", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (anyParticipantAborted(in.proposed)) {
            return Status::OK();
        }
        return checkParticipantsEqual(
            "recipient", in.proposed.getRecipientShards(), RecipientStateEnum::kUnused);
    }};

const CoordinatorPreConstraint kCopyProgressUnchanged{
    "copyProgressUnchanged", [](const CoordinatorPreTransitionInput& in) -> Status {
        Status status = checkUnchanged(
            "cloneTimestamp", in.before.getCloneTimestamp(), in.proposed.getCloneTimestamp());
        if (!status.isOK()) {
            return status;
        }
        status = checkUnchanged("approxBytesToCopy",
                                in.before.getApproxBytesToCopy(),
                                in.proposed.getApproxBytesToCopy());
        if (!status.isOK()) {
            return status;
        }
        return checkUnchanged("approxDocumentsToCopy",
                              in.before.getApproxDocumentsToCopy(),
                              in.proposed.getApproxDocumentsToCopy());
    }};

const CoordinatorPreConstraint kDocumentsToCopyAllOrNone{
    "documentsToCopyAllOrNone", [](const CoordinatorPreTransitionInput& in) -> Status {
        // documentsToCopy is written only when performVerification is enabled and the best-effort
        // donor fetch succeeded, so it may be absent from every donor.
        const auto& donors = in.proposed.getDonorShards();
        const auto withDocumentsToCopy =
            std::count_if(donors.begin(), donors.end(), [](const auto& donor) {
                return donor.getDocumentsToCopy().has_value();
            });
        if (withDocumentsToCopy == 0 || withDocumentsToCopy == std::ssize(donors)) {
            return Status::OK();
        }
        return transitionViolation(str::stream()
                                   << "documentsToCopy is set on " << withDocumentsToCopy << " of "
                                   << donors.size() << " donor shards");
    }};

const CoordinatorPreConstraint kCanEnterCritical{
    "canEnterCritical", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (in.facts.canEnterCritical) {
            return Status::OK();
        }
        return transitionViolation("coordinator is not ready to enter the critical section");
    }};

const CoordinatorPreConstraint kHasCriticalSectionExpiresAt{
    "hasCriticalSectionExpiresAt", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (in.proposed.getCriticalSectionExpiresAt()) {
            return Status::OK();
        }
        return transitionViolation("criticalSectionExpiresAt is unset");
    }};

const CoordinatorPreConstraint kAllRecipientsStrictlyConsistent{
    "allRecipientsStrictlyConsistent", [](const CoordinatorPreTransitionInput& in) -> Status {
        return checkParticipantsAtLeast(
            "recipient", in.proposed.getRecipientShards(), RecipientStateEnum::kStrictConsistency);
    }};

const CoordinatorPreConstraint kNoParticipantAbortReason{
    "noParticipantAbortReason", [](const CoordinatorPreTransitionInput& in) -> Status {
        Status status = checkNoParticipantAbortReason("donor", in.proposed.getDonorShards());
        if (!status.isOK()) {
            return status;
        }
        return checkNoParticipantAbortReason("recipient", in.proposed.getRecipientShards());
    }};

const CoordinatorPreConstraint kHasCloneTimestamp{
    "hasCloneTimestamp", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (in.proposed.getCloneTimestamp()) {
            return Status::OK();
        }
        return transitionViolation("cloneTimestamp is unset");
    }};

const CoordinatorPreConstraint kCriticalSectionExpiresAtUnchanged{
    "criticalSectionExpiresAtUnchanged", [](const CoordinatorPreTransitionInput& in) -> Status {
        return checkUnchanged("criticalSectionExpiresAt",
                              in.before.getCriticalSectionExpiresAt(),
                              in.proposed.getCriticalSectionExpiresAt());
    }};

const CoordinatorPreConstraint kAllParticipantsDone{
    "allParticipantsDone", [](const CoordinatorPreTransitionInput& in) -> Status {
        return checkAllParticipantsDone(in.proposed);
    }};

const CoordinatorPreConstraint kExitStateMatchesUserReshardingUUID{
    "exitStateMatchesUserReshardingUUID", [](const CoordinatorPreTransitionInput& in) -> Status {
        if (in.proposed.getState() == kQuiesced) {
            if (!in.proposed.getUserReshardingUUID()) {
                return transitionViolation("userReshardingUUID is unset");
            }
            if (!in.proposed.getQuiescePeriodEnd()) {
                return transitionViolation("quiescePeriodEnd is unset");
            }
            return Status::OK();
        }
        // Quiesced -> done keeps the user resharding UUID.
        if (in.before.getState() == kQuiesced) {
            return Status::OK();
        }
        if (const auto& userReshardingUUID = in.proposed.getUserReshardingUUID()) {
            return transitionViolation(str::stream()
                                       << "unexpected userReshardingUUID: " << *userReshardingUUID);
        }
        return Status::OK();
    }};

/**
 * Every edge requires the immutable metadata, an existing abort reason, and the participant list
 * to be unchanged.
 */
TransitionCheck<CoordinatorPreTransitionInput> preCheck(const CoordinatorPreTransitionInput& in) {
    auto check = makePreTransitionCheck(in, in.before.getState(), in.proposed.getState());
    check.require(kMetadataUnchanged)
        .require(kExistingAbortReasonUnchanged)
        .require(kParticipantIdsUnchanged);
    return check;
}

ConstraintResult validateEnteringInitializing(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kNoShardEntries)
        .require(kNoCloneTimestamp)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringPreparingToDonate(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasParticipants)
        .require(kParticipantsUnused)
        .require(kPresetChunksAndZonesCleared)
        .require(kNoCloneTimestamp)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringCloning(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kAllDonorsReportedMinFetchTimestamp)
        .require(kCloneTimestampIsHighestMinFetch)
        .require(kRecipientsUnused)
        .require(kHasApproxCopySize)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringApplying(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasCloneTimestamp)
        .require(kAllRecipientsApplying)
        .require(kCopyProgressUnchanged)
        .require(kDocumentsToCopyAllOrNone)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringBlockingWrites(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kCanEnterCritical)
        .require(kHasCloneTimestamp)
        .require(kHasCriticalSectionExpiresAt)
        .require(kCopyProgressUnchanged)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringCommitting(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kAllRecipientsStrictlyConsistent)
        .require(kNoParticipantAbortReason)
        .require(kHasCloneTimestamp)
        .require(kCopyProgressUnchanged)
        .require(kCriticalSectionExpiresAtUnchanged)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateEnteringAborting(const CoordinatorPreTransitionInput& in) {
    return preCheck(in).require(kHasAbortReason).require(kCopyProgressUnchanged).result();
}

ConstraintResult validateEnteringAbortingAfterCloning(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasAbortReason)
        .require(kHasCloneTimestamp)
        .require(kCopyProgressUnchanged)
        .result();
}

ConstraintResult validateSuccessExit(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kNoAbortReason)
        .require(kAllParticipantsDone)
        .require(kCopyProgressUnchanged)
        .require(kExitStateMatchesUserReshardingUUID)
        .result();
}

ConstraintResult validateAbortExit(const CoordinatorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasAbortReason)
        .require(kCopyProgressUnchanged)
        .require(kExitStateMatchesUserReshardingUUID)
        .result();
}

ConstraintResult validateLeavingQuiesced(const CoordinatorPreTransitionInput& in) {
    return preCheck(in).require(kCopyProgressUnchanged).result();
}

using CoordinatorTransitionEdge = std::pair<CoordinatorStateEnum, CoordinatorStateEnum>;
using CoordinatorPreTransitionValidator =
    ConstraintResult (*)(const CoordinatorPreTransitionInput&);

const std::map<CoordinatorTransitionEdge, CoordinatorPreTransitionValidator>&
coordinatorTransitionValidators() {
    static StaticImmortal validators =
        std::map<CoordinatorTransitionEdge, CoordinatorPreTransitionValidator>{
            // Forward edges.
            {{kUnused, kInitializing}, &validateEnteringInitializing},
            {{kInitializing, kPreparingToDonate}, &validateEnteringPreparingToDonate},
            {{kPreparingToDonate, kCloning}, &validateEnteringCloning},
            {{kCloning, kApplying}, &validateEnteringApplying},
            {{kApplying, kBlockingWrites}, &validateEnteringBlockingWrites},
            {{kBlockingWrites, kCommitting}, &validateEnteringCommitting},
            // kAborting edges.
            {{kPreparingToDonate, kAborting}, &validateEnteringAborting},
            {{kCloning, kAborting}, &validateEnteringAbortingAfterCloning},
            {{kApplying, kAborting}, &validateEnteringAbortingAfterCloning},
            {{kBlockingWrites, kAborting}, &validateEnteringAbortingAfterCloning},
            // Success exit edges.
            {{kCommitting, kQuiesced}, &validateSuccessExit},
            {{kCommitting, kDone}, &validateSuccessExit},
            // Abort exit edges.
            {{kInitializing, kQuiesced}, &validateAbortExit},
            {{kInitializing, kDone}, &validateAbortExit},
            {{kAborting, kQuiesced}, &validateAbortExit},
            {{kAborting, kDone}, &validateAbortExit},
            // kQuiesced edges.
            {{kQuiesced, kDone}, &validateLeavingQuiesced},
        };
    return *validators;
}

}  // namespace

ConstraintResult validateCoordinatorPreTransition(const CoordinatorPreTransitionInput& input) {
    const auto& validators = coordinatorTransitionValidators();
    const auto it = validators.find({input.before.getState(), input.proposed.getState()});
    if (it == validators.end()) {
        return notAllowedTransition(input.before.getState(), input.proposed.getState());
    }
    return it->second(input);
}

}  // namespace mongo
