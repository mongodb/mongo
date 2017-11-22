/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/range_arithmetic.h"

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
