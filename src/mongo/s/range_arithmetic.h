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

#pragma once

#include <string>
#include <map>
#include <vector>

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

        KeyRange( const std::string& ns,
                  const BSONObj& minKey,
                  const BSONObj& maxKey,
                  const BSONObj& keyPattern ) :
                ns( ns ), minKey( minKey ), maxKey( maxKey ), keyPattern( keyPattern )
        {
        }

        KeyRange() {}

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

    /**
     * A RangeMap is a mapping of a BSON range from lower->upper (lower maps to upper), using
     * standard BSON woCompare.  Upper bound is exclusive.
     *
     * NOTE: For overlap testing to work correctly, there may be no overlaps present in the map
     * itself.
     */
    typedef map<BSONObj, BSONObj, BSONObjCmp> RangeMap;

    /**
     * A RangeVector is a list of [lower,upper) ranges.
     */
    typedef vector<pair<BSONObj,BSONObj> > RangeVector;

    /**
     * Returns the overlap of a range [inclusiveLower, exclusiveUpper) with the provided range map
     * as a vector of ranges from the map.
     */
    void getRangeMapOverlap( const RangeMap& ranges,
                             const BSONObj& inclusiveLower,
                             const BSONObj& exclusiveUpper,
                             RangeVector* vector );

    /**
     * Returns true if the provided range map has ranges which overlap the provided range
     * [inclusiveLower, exclusiveUpper).
     */
    bool rangeMapOverlaps( const RangeMap& ranges,
                           const BSONObj& inclusiveLower,
                           const BSONObj& exclusiveUpper );

    /**
     * Returns true if the provided range map exactly contains the provided range
     * [inclusiveLower, exclusiveUpper).
     */
    bool rangeMapContains( const RangeMap& ranges,
                           const BSONObj& inclusiveLower,
                           const BSONObj& exclusiveUpper );

    /**
     * String representation of [inclusiveLower, exclusiveUpper)
     */
    std::string rangeToString( const BSONObj& inclusiveLower,
                               const BSONObj& exclusiveUpper );

    /**
     * String representation of overlapping ranges as a list "[range1),[range2),..."
     */
    std::string overlapToString( RangeVector overlap );

}
