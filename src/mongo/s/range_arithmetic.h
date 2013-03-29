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

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {
    /**
     * Returns true if the point is within the range [inclusiveLower, exclusiveUpper).
     */
    bool rangeContains(const BSONObj& inclusiveLower,
                       const BSONObj& exclusiveUpper,
                       const BSONObj& point);

    /**
     * Returns true if the bounds specified by [inclusiveLower1, exclusiveUpper1)
     * intersects with the bounds [inclusiveLower2, exclusiveUpper2).
     */
    bool rangeOverlaps(const BSONObj& inclusiveLower1,
                       const BSONObj& exclusiveUpper1,
                       const BSONObj& inclusiveLower2,
                       const BSONObj& exclusiveUpper2);

    /**
     * Returns -1 if first range is less than the second range, 0 if equal and 1 if
     * greater. The ordering is based on comparing both the min first and then uses
     * the max as the tie breaker.
     */
    int compareRanges(const BSONObj& rangeMin1,
                      const BSONObj& rangeMax1,
                      const BSONObj& rangeMin2,
                      const BSONObj& rangeMax2);
}
