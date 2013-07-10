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

    using std::make_pair;
    using std::pair;
    using std::string;
    using std::stringstream;

    bool rangeContains( const BSONObj& inclusiveLower,
                        const BSONObj& exclusiveUpper,
                        const BSONObj& point )
    {
        return point.woCompare( inclusiveLower ) >= 0 && point.woCompare( exclusiveUpper ) < 0;
    }

    bool rangeOverlaps( const BSONObj& inclusiveLower1,
                        const BSONObj& exclusiveUpper1,
                        const BSONObj& inclusiveLower2,
                        const BSONObj& exclusiveUpper2 )
    {
        return ( exclusiveUpper1.woCompare( inclusiveLower2 ) > 0 )
            && ( exclusiveUpper2.woCompare( inclusiveLower1 ) > 0 );
    }

    int compareRanges( const BSONObj& rangeMin1,
                       const BSONObj& rangeMax1,
                       const BSONObj& rangeMin2,
                       const BSONObj& rangeMax2 )
    {
        const int minCmp = rangeMin1.woCompare( rangeMin2 );
        if ( minCmp != 0 ) return minCmp;
        return rangeMax1.woCompare( rangeMax2 );
    }

    // Represents the start and end of an overlap of a tested range
    typedef pair<RangeMap::const_iterator, RangeMap::const_iterator> OverlapBounds;

    // Internal-only, shared functionality
    OverlapBounds rangeMapOverlapBounds( const RangeMap& ranges,
                                         const BSONObj& inclusiveLower,
                                         const BSONObj& exclusiveUpper ) {

        // Returns the first chunk with a min key that is >= lower bound - the previous chunk
        // might overlap.
        RangeMap::const_iterator low = ranges.lower_bound( inclusiveLower );

        // See if the previous chunk overlaps our range, not clear from just min key
        if ( low != ranges.begin() ) {

            RangeMap::const_iterator next = low;
            --low;

            // If the previous range's max value is lte our min value
            if ( low->second.woCompare( inclusiveLower ) < 1 ) {
                low = next;
            }
        }

        // Returns the first chunk with a max key that is >= upper bound - implies the
        // chunk does not overlap upper bound
        RangeMap::const_iterator high = ranges.lower_bound( exclusiveUpper );

        return OverlapBounds( low, high );
    }

    void getRangeMapOverlap( const RangeMap& ranges,
                             const BSONObj& inclusiveLower,
                             const BSONObj& exclusiveUpper,
                             RangeVector* overlap ) {
        overlap->clear();
        OverlapBounds bounds = rangeMapOverlapBounds( ranges, inclusiveLower, exclusiveUpper );
        for ( RangeMap::const_iterator it = bounds.first; it != bounds.second; ++it ) {
            overlap->push_back( make_pair( it->first, it->second ) );
        }
    }

    bool rangeMapOverlaps( const RangeMap& ranges,
                           const BSONObj& inclusiveLower,
                           const BSONObj& exclusiveUpper ) {
        OverlapBounds bounds = rangeMapOverlapBounds( ranges, inclusiveLower, exclusiveUpper );
        return bounds.first != bounds.second;
    }

    bool rangeMapContains( const RangeMap& ranges,
                           const BSONObj& inclusiveLower,
                           const BSONObj& exclusiveUpper ) {
        OverlapBounds bounds = rangeMapOverlapBounds( ranges, inclusiveLower, exclusiveUpper );
        if ( bounds.first == ranges.end() ) return false;

        return bounds.first->first.woCompare( inclusiveLower ) == 0
            && bounds.first->second.woCompare( exclusiveUpper ) == 0;
    }

}
