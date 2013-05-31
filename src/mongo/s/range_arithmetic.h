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

#include <string>

#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * A KeyRange represents a range over keys of documents in a namespace, qualified by a
     * key pattern which defines the documents that are in the key range.
     *
     * There may be many different expressions to generate the same key fields from a document - the
     * keyPattern tells us these expressions.
     *
     * Ex:
     * DocA : { field : "aaaa" }
     * DocB : { field : "bbb" }
     * DocC : { field : "ccccc" }
     *
     * keyPattern : { field : 1 }
     * minKey : { field : "aaaa" } : Id(DocA)
     * maxKey : { field : "ccccc" } : Id(DocB)
     *
     * contains Id(DocB)
     *
     * keyPattern : { field : "numberofletters" }
     * minKey : { field : 4 } : numberofletters(DocA)
     * maxKey : { field : 5 } : numberofletters(DocC)
     *
     * does not contain numberofletters(DocB)
     */
    struct KeyRange {

        KeyRange( const std::string& ns_,
                  const BSONObj& minKey_,
                  const BSONObj& maxKey_,
                  const BSONObj& keyPattern_ ) :
                ns( ns_ ), minKey( minKey_ ), maxKey( maxKey_ ), keyPattern( keyPattern_ )
        {
        }

        std::string ns;
        BSONObj minKey;
        BSONObj maxKey;
        BSONObj keyPattern;
    };

    /**
     * Returns true if the point is within the range [inclusiveLower, exclusiveUpper).
     */
    bool rangeContains( const BSONObj& inclusiveLower,
                        const BSONObj& exclusiveUpper,
                        const BSONObj& point );

    /**
     * Returns true if the bounds specified by [inclusiveLower1, exclusiveUpper1)
     * intersects with the bounds [inclusiveLower2, exclusiveUpper2).
     */
    bool rangeOverlaps( const BSONObj& inclusiveLower1,
                        const BSONObj& exclusiveUpper1,
                        const BSONObj& inclusiveLower2,
                        const BSONObj& exclusiveUpper2 );

    /**
     * Returns -1 if first range is less than the second range, 0 if equal and 1 if
     * greater. The ordering is based on comparing both the min first and then uses
     * the max as the tie breaker.
     */
    int compareRanges( const BSONObj& rangeMin1,
                       const BSONObj& rangeMax1,
                       const BSONObj& rangeMin2,
                       const BSONObj& rangeMax2 );

}
