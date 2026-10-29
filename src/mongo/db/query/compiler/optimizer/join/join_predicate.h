// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * The physical representation of a join predicate between two relations. This struct is used by
 * QuerySolutionNodes that implement binary joins to indicate which fields are being joined.
 */
struct QSNJoinPredicate {
    enum class ComparisonOp {
        // Regular equality (null == missing).
        Eq,
        // "Strict" $expr equality (null != missing).
        ExprEq
    };

    ComparisonOp op;
    // The left and right fields of the equality predicate. The order of left and right fields is
    // important as it corresponds to the children of a join node. The field may correspond directly
    // to a collection, in which case the namespace of the field is implicit in the structure of the
    // QSN, or to a stream of documents which themselves represent the result of a join, in which
    // case the field will have a prefix representing the "as" field of a $lookup.
    FieldPath leftField;
    FieldPath rightField;

    bool isEquality() const {
        return op == ComparisonOp::Eq || op == ComparisonOp::ExprEq;
    }

    std::string toString() const;

    template <typename H>
    friend H AbslHashValue(H h, const QSNJoinPredicate& pred) {
        return H::combine(std::move(h), pred.op, pred.leftField, pred.rightField);
    }
};

}  // namespace mongo
