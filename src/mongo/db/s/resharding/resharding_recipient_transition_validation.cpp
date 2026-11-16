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
        return Status::OK();
    }};

const RecipientPostConstraint kInMemoryMatchesProposed{
    "inMemoryMatchesProposed", [](const RecipientPostTransitionInput& in) -> Status {
        return checkBsonMatches(
            "in-memory context", in.inMemory.toBSON(), in.proposed.ctx.toBSON());
    }};

using RecipientTransitionEdge = std::pair<RecipientStateEnum, RecipientStateEnum>;
using RecipientPreTransitionValidator = ConstraintResult (*)(const RecipientPreTransitionInput&);

// Legal transitions and their validators. Documents start in kAwaitingFetchTimestamp, so kUnused is
// not a source. kStrictConsistency -> kError is absent: the error path asserts it is never reached.
const std::map<RecipientTransitionEdge, RecipientPreTransitionValidator>&
recipientTransitionValidators() {
    static StaticImmortal validators =
        std::map<RecipientTransitionEdge, RecipientPreTransitionValidator>{};
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
