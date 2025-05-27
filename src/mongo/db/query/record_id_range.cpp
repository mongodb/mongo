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

#include "mongo/db/query/record_id_range.h"

#include <compare>

#include <boost/optional.hpp>

namespace mongo {
void RecordIdRange::maybeNarrowMin(const BSONObj& newMin, bool inclusive) {
    maybeNarrowMin(RecordIdBound(record_id_helpers::keyForObj(newMin), newMin), inclusive);
}

void RecordIdRange::maybeNarrowMin(const RecordIdBound& newMin, bool inclusive) {
    std::partial_ordering cmp = _min ? *_min <=> newMin : std::partial_ordering::unordered;
    // The range only needs updating if:
    // * There's no existing _min
    // * The provided value is greater than the current _min
    // * The value == _min, but is _not_ inclusive, but the existing value is

    if (std::is_gt(cmp)) {
        // Current min is strictly greater than the provided value (and existing value has been
        // initialised), nothing to do.
        return;
    }

    if (std::is_eq(cmp)) {
        // Inclusivity moving true -> false narrows the range.
        _minInclusive = _minInclusive && inclusive;
        return;
    }
    _min = newMin;
    // The bound value changed, so the previous value of _minInclusive is irrelevant.
    _minInclusive = inclusive;
}

void RecordIdRange::maybeNarrowMax(const BSONObj& newMax, bool inclusive) {
    maybeNarrowMax(RecordIdBound(record_id_helpers::keyForObj(newMax), newMax), inclusive);
}

void RecordIdRange::maybeNarrowMax(const RecordIdBound& newMax, bool inclusive) {
    std::partial_ordering cmp = _max ? *_max <=> newMax : std::partial_ordering::unordered;
    // The range only needs updating if:
    // * There's no existing _max
    // * The provided value is less than the current _max
    // * The value == _max, but is _not_ inclusive, but the existing value is

    if (std::is_lt(cmp)) {
        // Current max is strictly less than the provided value (and existing value has been
        // initialised), nothing to do.
        return;
    }

    if (std::is_eq(cmp)) {
        // Inclusivity moving true -> false narrows the range.
        _maxInclusive = _maxInclusive && inclusive;
        return;
    }
    _max = newMax;
    // The bound value changed, so the previous value of _maxInclusive is irrelevant.
    _maxInclusive = inclusive;
}

void RecordIdRange::intersectRange(const RecordIdRange& other) {
    intersectRange(other._min, other._max, other._minInclusive, other._maxInclusive);
}

void RecordIdRange::intersectRange(const boost::optional<RecordIdBound>& min,
                                   const boost::optional<RecordIdBound>& max,
                                   bool minInclusive,
                                   bool maxInclusive) {
    if (min) {
        maybeNarrowMin(*min, minInclusive);
    }
    if (max) {
        maybeNarrowMax(*max, maxInclusive);
    }
}

}  // namespace mongo
