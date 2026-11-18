// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/resharding_transition_check.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <string>
#include <utility>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Helpers shared by the donor, recipient and coordinator transition validators. Constraints stay
 * next to the document type they validate; only checks on plain field values live here.
 */

inline Status transitionViolation(std::string reason) {
    return {ErrorCodes::InternalError, std::move(reason)};
}

template <typename T>
std::string describeOptionalValue(const boost::optional<T>& value) {
    if (!value) {
        return "unset";
    }
    return str::stream() << *value;
}

/**
 * Date_t is only streamable to std::ostream.
 */
inline std::string describeOptionalValue(const boost::optional<Date_t>& value) {
    return value ? value->toString() : "unset";
}

template <typename T>
Status checkUnchanged(const char* field,
                      const boost::optional<T>& before,
                      const boost::optional<T>& proposed) {
    if (before == proposed) {
        return Status::OK();
    }
    return transitionViolation(str::stream()
                               << field << " changed from " << describeOptionalValue(before)
                               << " to " << describeOptionalValue(proposed));
}

/**
 * A null timestamp is treated as unset.
 */
inline bool isSetAndNonNull(const boost::optional<Timestamp>& timestamp) {
    return timestamp && !timestamp->isNull();
}

inline Status checkSetAndNonNull(const std::string& field,
                                 const boost::optional<Timestamp>& value) {
    if (!value) {
        return transitionViolation(field + " is unset");
    }
    if (value->isNull()) {
        return transitionViolation(field + " is null");
    }
    return Status::OK();
}

inline Status checkNonNegative(const char* field, const boost::optional<int64_t>& value) {
    if (!value) {
        return transitionViolation(str::stream() << field << " is unset");
    }
    if (*value < 0) {
        return transitionViolation(str::stream() << field << " is negative: " << *value);
    }
    return Status::OK();
}

inline Status checkNoAbortReason(const boost::optional<BSONObj>& proposed) {
    if (!proposed) {
        return Status::OK();
    }
    return transitionViolation(str::stream() << "unexpected abort reason: " << *proposed);
}

inline Status checkHasAbortReason(const boost::optional<BSONObj>& proposed) {
    if (proposed) {
        return Status::OK();
    }
    return transitionViolation("abort reason is unset");
}

inline Status checkAbortReasonUnchanged(const boost::optional<BSONObj>& before,
                                        const boost::optional<BSONObj>& proposed) {
    if (!before) {
        return Status::OK();
    }
    if (proposed && before->binaryEqual(*proposed)) {
        return Status::OK();
    }
    return transitionViolation(str::stream() << "abort reason changed from " << *before << " to "
                                             << describeOptionalValue(proposed));
}

inline Status checkBsonMatches(const char* actualName,
                               const BSONObj& actual,
                               const BSONObj& proposed) {
    if (actual.binaryEqual(proposed)) {
        return Status::OK();
    }
    return transitionViolation(str::stream()
                               << actualName << " " << actual << " != proposed " << proposed);
}

template <typename Input, typename StateEnum>
TransitionCheck<Input> makePreTransitionCheck(const Input& in, StateEnum from, StateEnum to) {
    return {describeTransitionEdge(from, to), in, ErrorCodes::ReshardingPreconditionFailure};
}

template <typename Input, typename StateEnum>
TransitionCheck<Input> makePostTransitionCheck(const Input& in, StateEnum from, StateEnum to) {
    return {describeTransitionEdge(from, to), in, ErrorCodes::ReshardingPostconditionFailure};
}

template <typename StateEnum>
ConstraintResult notAllowedTransition(
    StateEnum from,
    StateEnum to,
    ErrorCodes::Error code = ErrorCodes::ReshardingPreconditionFailure) {
    return {Status{code, describeTransitionEdge(from, to) + " is not allowed"}, {"isAllowedEdge"}};
}

}  // namespace mongo
