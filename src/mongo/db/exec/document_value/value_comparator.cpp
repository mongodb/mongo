// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value_comparator.h"

#include "mongo/util/assert_util.h"

namespace mongo {

const ValueComparator ValueComparator::kInstance{};

bool ValueComparator::evaluate(Value::DeferredComparison deferredComparison) const {
    int cmp = Value::compare(deferredComparison.lhs, deferredComparison.rhs, _stringComparator);
    switch (deferredComparison.type) {
        case Value::DeferredComparison::Type::kLT:
            return cmp < 0;
        case Value::DeferredComparison::Type::kLTE:
            return cmp <= 0;
        case Value::DeferredComparison::Type::kEQ:
            return cmp == 0;
        case Value::DeferredComparison::Type::kGTE:
            return cmp >= 0;
        case Value::DeferredComparison::Type::kGT:
            return cmp > 0;
        case Value::DeferredComparison::Type::kNE:
            return cmp != 0;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
