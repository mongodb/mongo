// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/range_arithmetic.h"

#include <map>
#include <utility>

namespace mongo {
namespace {

// Represents the start and end of an overlap of a tested range
typedef std::pair<RangeMap::const_iterator, RangeMap::const_iterator> OverlapBounds;

// Internal-only, shared functionality
OverlapBounds rangeMapOverlapBounds(const RangeMap& ranges,
                                    const BSONObj& inclusiveLower,
                                    const BSONObj& exclusiveUpper) {
    // Returns the first chunk with a min key that is >= lower bound - the previous chunk
    // might overlap.
    RangeMap::const_iterator low = ranges.lower_bound(inclusiveLower);

    // See if the previous chunk overlaps our range, not clear from just min key
    if (low != ranges.begin()) {
        RangeMap::const_iterator next = low;
        --low;

        // If the previous range's max value is lte our min value
        if (low->second.woCompare(inclusiveLower) < 1) {
            low = next;
        }
    }

    // Returns the first chunk with a max key that is >= upper bound - implies the
    // chunk does not overlap upper bound
    RangeMap::const_iterator high = ranges.lower_bound(exclusiveUpper);

    return OverlapBounds(low, high);
}

}  // namespace

bool rangeContains(const BSONObj& inclusiveLower,
                   const BSONObj& exclusiveUpper,
                   const BSONObj& point) {
    return point.woCompare(inclusiveLower) >= 0 && point.woCompare(exclusiveUpper) < 0;
}

bool rangeOverlaps(const BSONObj& inclusiveLower1,
                   const BSONObj& exclusiveUpper1,
                   const BSONObj& inclusiveLower2,
                   const BSONObj& exclusiveUpper2) {
    return (exclusiveUpper1.woCompare(inclusiveLower2) > 0) &&
        (exclusiveUpper2.woCompare(inclusiveLower1) > 0);
}

bool rangeMapOverlaps(const RangeMap& ranges,
                      const BSONObj& inclusiveLower,
                      const BSONObj& exclusiveUpper) {
    OverlapBounds bounds = rangeMapOverlapBounds(ranges, inclusiveLower, exclusiveUpper);
    return bounds.first != bounds.second;
}

}  // namespace mongo
