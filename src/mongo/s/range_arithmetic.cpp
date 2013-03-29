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
 */

#include "mongo/s/range_arithmetic.h"

namespace mongo {
    bool rangeContains(const BSONObj& inclusiveLower,
                       const BSONObj& exclusiveUpper,
                       const BSONObj& point) {
        return point.woCompare(inclusiveLower) >= 0 &&
                point.woCompare(exclusiveUpper) < 0;
    }

    bool rangeOverlaps(const BSONObj& inclusiveLower1,
                       const BSONObj& exclusiveUpper1,
                       const BSONObj& inclusiveLower2,
                       const BSONObj& exclusiveUpper2) {
        return (exclusiveUpper1.woCompare(inclusiveLower2) > 0) &&
                (exclusiveUpper2.woCompare(inclusiveLower1) > 0);
    }

    int compareRanges(const BSONObj& rangeMin1,
                      const BSONObj& rangeMax1,
                      const BSONObj& rangeMin2,
                      const BSONObj& rangeMax2) {
        const int minCmp = rangeMin1.woCompare(rangeMin2);
        if (minCmp != 0) return minCmp;
        return rangeMax1.woCompare(rangeMax2);
    }
}
