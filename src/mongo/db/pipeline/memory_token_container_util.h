/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"

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
