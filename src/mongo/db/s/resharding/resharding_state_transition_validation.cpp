// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_state_transition_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
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

Status violation(std::string reason) {
    return {ErrorCodes::InternalError, std::move(reason)};
}

template <typename T>
std::string describeValue(const boost::optional<T>& value) {
    if (!value) {
        return "unset";
    }
    return str::stream() << *value;
}

Status checkCloneStat(const char* field, const boost::optional<int64_t>& value) {
    if (!value) {
        return violation(str::stream() << field << " is unset");
    }
    if (*value < 0) {
        return violation(str::stream() << field << " is negative: " << *value);
    }
    return Status::OK();
}

template <typename T>
Status checkUnchanged(const char* field,
                      const boost::optional<T>& before,
                      const boost::optional<T>& proposed) {
    if (before == proposed) {
        return Status::OK();
    }
    return violation(str::stream() << field << " changed from " << describeValue(before) << " to "
                                   << describeValue(proposed));
}

const DonorPreConstraint kHasMinFetchTimestamp{
    "hasMinFetchTimestamp", [](const DonorPreTransitionInput& in) -> Status {
        const auto& minFetchTimestamp = in.proposed.getMinFetchTimestamp();
        if (!minFetchTimestamp) {
            return violation("minFetchTimestamp is unset");
        }
        if (minFetchTimestamp->isNull()) {
            return violation("minFetchTimestamp is null");
        }
        return Status::OK();
    }};

const DonorPreConstraint kHasValidCloneStats{
    "hasValidCloneStats", [](const DonorPreTransitionInput& in) -> Status {
        Status status = checkCloneStat("bytesToClone", in.proposed.getBytesToClone());
        if (!status.isOK()) {
            return status;
        }
        status = checkCloneStat("documentsToClone", in.proposed.getDocumentsToClone());
        if (!status.isOK()) {
            return status;
        }
        return checkCloneStat("indexCount", in.proposed.getIndexCount());
    }};

const DonorPreConstraint kNoAbortReason{
    "noAbortReason", [](const DonorPreTransitionInput& in) -> Status {
        const auto& abortReason = in.proposed.getAbortReason();
        if (!abortReason) {
            return Status::OK();
        }
        return violation(str::stream() << "unexpected abort reason: " << *abortReason);
    }};

const DonorPreConstraint kHasAbortReason{"hasAbortReason",
                                         [](const DonorPreTransitionInput& in) -> Status {
                                             if (in.proposed.getAbortReason()) {
                                                 return Status::OK();
                                             }
                                             return violation("abort reason is unset");
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
        const auto& before = in.before.getAbortReason();
        if (!before) {
            return Status::OK();
        }
        const auto& proposed = in.proposed.getAbortReason();
        if (proposed && before->binaryEqual(*proposed)) {
            return Status::OK();
        }
        return violation(str::stream() << "abort reason changed from " << *before << " to "
                                       << describeValue(proposed));
    }};

const DonorPostConstraint kDurableMatchesProposed{
    "durableMatchesProposed", [](const DonorPostTransitionInput& in) -> Status {
        if (in.durable.toBSON().binaryEqual(in.proposed.toBSON())) {
            return Status::OK();
        }
        return violation(str::stream() << "durable " << in.durable.toBSON() << " != proposed "
                                       << in.proposed.toBSON());
    }};

const DonorPostConstraint kInMemoryMatchesProposed{
    "inMemoryMatchesProposed", [](const DonorPostTransitionInput& in) -> Status {
        if (in.inMemory.toBSON().binaryEqual(in.proposed.toBSON())) {
            return Status::OK();
        }
        return violation(str::stream() << "in-memory " << in.inMemory.toBSON() << " != proposed "
                                       << in.proposed.toBSON());
    }};

TransitionCheck<DonorPreTransitionInput> preCheck(const DonorPreTransitionInput& in) {
    return {describeTransitionEdge(in.before.getState(), in.proposed.getState()),
            in,
            ErrorCodes::ReshardingPreconditionFailure};
}

ConstraintResult validateEnteringDonatingInitialData(const DonorPreTransitionInput& in) {
    return preCheck(in)
        .require(kHasMinFetchTimestamp)
        .require(kHasValidCloneStats)
        .require(kNoAbortReason)
        .result();
}

ConstraintResult validateForward(const DonorPreTransitionInput& in) {
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

using DonorTransitionEdge = std::pair<DonorStateEnum, DonorStateEnum>;
using DonorPreTransitionValidator = ConstraintResult (*)(const DonorPreTransitionInput&);

const std::map<DonorTransitionEdge, DonorPreTransitionValidator>& donorTransitionValidators() {
    static StaticImmortal validators = std::map<DonorTransitionEdge, DonorPreTransitionValidator>{
        // Forward edges.
        {{kPreparingToDonate, kDonatingInitialData}, &validateEnteringDonatingInitialData},
        {{kDonatingInitialData, kDonatingOplogEntries}, &validateForward},
        {{kDonatingOplogEntries, kPreparingToBlockWrites}, &validateForward},
        {{kPreparingToBlockWrites, kBlockingWrites}, &validateForward},
        // kError edges.
        {{kPreparingToDonate, kError}, &validateEnteringError},
        {{kDonatingInitialData, kError}, &validateEnteringError},
        {{kDonatingOplogEntries, kError}, &validateEnteringError},
        {{kPreparingToBlockWrites, kError}, &validateEnteringError},
        // kDone edges.
        {{kPreparingToDonate, kDone}, &validateEnteringDone},
        {{kDonatingInitialData, kDone}, &validateEnteringDone},
        {{kDonatingOplogEntries, kDone}, &validateEnteringDone},
        {{kPreparingToBlockWrites, kDone}, &validateEnteringDone},
        {{kError, kDone}, &validateEnteringDone},
        {{kBlockingWrites, kDone}, &validateEnteringDone},
    };
    return *validators;
}

}  // namespace

ConstraintResult validateDonorPreTransition(const DonorPreTransitionInput& input) {
    const auto& validators = donorTransitionValidators();
    const auto it = validators.find({input.before.getState(), input.proposed.getState()});
    if (it == validators.end()) {
        return {Status{ErrorCodes::ReshardingPreconditionFailure,
                       describeTransitionEdge(input.before.getState(), input.proposed.getState()) +
                           " is not allowed"},
                {"isAllowedEdge"}};
    }
    return it->second(input);
}

ConstraintResult validateDonorPostTransition(const DonorPostTransitionInput& input) {
    return TransitionCheck<DonorPostTransitionInput>(
               std::string{str::stream()
                           << "transition to " << idl::serialize(input.proposed.getState())},
               input,
               ErrorCodes::ReshardingPostconditionFailure)
        .require(kDurableMatchesProposed)
        .require(kInMemoryMatchesProposed)
        .result();
}

}  // namespace mongo
