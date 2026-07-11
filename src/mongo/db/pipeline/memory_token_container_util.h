// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/util/modules.h"

namespace mongo {

class MemoryTokenValueComparator {
public:
    using is_transparent = std::true_type;

    explicit MemoryTokenValueComparator(const ValueComparator* comparator)
        : _comparator(comparator) {}

    template <typename LHS, typename RHS>
    bool operator()(const LHS& lhs, const RHS& rhs) const {
        return _comparator->compare(_getValue(lhs), _getValue(rhs)) < 0;
    }

private:
    const Value& _getValue(const Value& v) const {
        return v;
    }

    template <typename Tracker>
    const Value& _getValue(const MemoryUsageTokenWithImpl<Tracker, Value>& v) const {
        return v.value();
    }

    const ValueComparator* _comparator;
};

/**
 * Helper function to convert container of MemoryUsageTokenWith<Value> to a Value, containing an
 * array of Values.
 */
template <typename Iterator>
Value convertToValueFromMemoryTokenWithValue(Iterator begin, Iterator end, size_t size) {
    std::vector<Value> result;
    result.reserve(size);
    while (begin != end) {
        result.emplace_back(begin->value());
        ++begin;
    }
    return Value{std::move(result)};
}

}  // namespace mongo
