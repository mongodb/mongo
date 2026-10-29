// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

bool RecordIdRange::isEmpty() const {
    if (!_min || !_max) {
        return false;  // at least one side is unbounded → the range is not empty
    }
    auto cmp = *_min <=> *_max;
    if (std::is_gt(cmp)) {
        return true;  // min > max → no records can satisfy the range
    }
    if (std::is_eq(cmp)) {
        // Same bound value: empty unless both sides are inclusive ([x,x] contains x)
        return !_minInclusive || !_maxInclusive;
    }
    return false;
}

int RecordIdRange::compare(const RecordId& rid) const {
    if (_min) {
        int cmp = _min->recordId().compare(rid);
        // cmp > 0: _min > rid → rid is before the range start
        // cmp == 0 with exclusive bound: rid is at an excluded start → also before range
        if (cmp > 0 || (cmp == 0 && !_minInclusive)) {
            return -1;
        }
    }
    if (_max) {
        int cmp = _max->recordId().compare(rid);
        // cmp < 0: _max < rid → rid is past the range end
        // cmp == 0 with exclusive bound: rid is at an excluded end → also past range
        if (cmp < 0 || (cmp == 0 && !_maxInclusive)) {
            return 1;
        }
    }
    return 0;
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

auto RecordIdRange::makeSeekParams(bool forward) const
    -> boost::optional<std::tuple<const RecordId&, SeekableRecordCursor::BoundInclusion>> {
    const auto& seekTarget = forward ? _min : _max;
    if (seekTarget) {
        bool seekTargetInclusive = forward ? _minInclusive : _maxInclusive;
        auto inclusivity = seekTargetInclusive ? SeekableRecordCursor::BoundInclusion::kInclude
                                               : SeekableRecordCursor::BoundInclusion::kExclude;
        return std::make_tuple(std::cref(seekTarget->recordId()), inclusivity);
    }

    return boost::none;
}

}  // namespace mongo
