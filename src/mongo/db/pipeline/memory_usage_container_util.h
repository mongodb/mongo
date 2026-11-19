// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/util/modules.h"

namespace mongo {

// A value paired with the bytes charged to the memory tracker when it was inserted into a
// container. Release exactly 'memoryUsage' when the entry is removed, since the value's size may
// not be recomputable later. Use for manual tracking where a MemoryUsageTokenWith per element would
// add too much overhead.
template <typename T>
struct ValueWithMemoryUsage {
    T value;
    int64_t memoryUsage{0};
};

class ValueWithMemoryUsageComparator {
public:
    using is_transparent = std::true_type;

    explicit ValueWithMemoryUsageComparator(const ValueComparator* comparator)
        : _comparator(comparator) {}

    template <typename LHS, typename RHS>
    bool operator()(const LHS& lhs, const RHS& rhs) const {
        return _comparator->compare(_getValue(lhs), _getValue(rhs)) < 0;
    }

private:
    static const Value& _getValue(const Value& v) {
        return v;
    }
    static const Value& _getValue(const ValueWithMemoryUsage<Value>& v) {
        return v.value;
    }

    const ValueComparator* _comparator;
};

/**
 * Helper function to convert container of ValueWithMemoryUsage<Value> to a Value, containing an
 * array of Values.
 */
template <typename Iterator>
Value convertToValueFromMemoryTokenWithValue(Iterator begin, Iterator end, size_t size) {
    std::vector<Value> result;
    result.reserve(size);
    while (begin != end) {
        result.emplace_back(begin->value);
        ++begin;
    }
    return Value{std::move(result)};
}

}  // namespace mongo
