/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/diskloc.h"
#include "mongo/bson/ordering.h"

namespace mongo {

    /**
     * This represents a single item in an index. This is intended to (possibly) be used by
     * implementations of the SortedDataInterface interface. An index item simply consists of a key
     * and a disk location, but this could be subclassed to perform more complex tasks.
     */
    struct IndexKeyEntry {
        IndexKeyEntry(const BSONObj& key, DiskLoc loc) :key(key), loc(loc) {}

        BSONObj key;
        DiskLoc loc;
    };

    /**
     * Compares two different IndexKeyEntry instances.
     * The existense of compound indexes necessitates some complicated logic. This is meant to
     * support the implementation of the SortedDataInterface::customLocate() and
     * SortedDataInterface::advanceTo() methods, which require fine-grained control over whether the
     * ranges of various keys comprising a compound index are inclusive or exclusive.
     */
    class IndexEntryComparison {
    public:
        IndexEntryComparison(Ordering order) : _order(order) {}

        bool operator() (const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;

        /**
         * Returns -1 if lhs < rhs, 1 if lhs > rhs, and 0 otherwise.
         *
         * This function compares two IndexKeyEntry objects which have been stripped of their field
         * names. Either lhs or rhs can represent the lower bound of a query, meaning that either
         * lhs or rhs can be the result of a call to makeQueryObject(). At most one can be a query
         * object. The comparison function compares the BSONObjects in each IndexKeyEntry, and uses
         * the DiskLoc's as a tiebreaker.
         *
         * Ex: {"": 5, "": "foo"} > {"": 4, "": "foo"}
         *     {"": 5, "": "foo"} < {"": 5, "": "zzz"}
         *
         * It may be necessary to specify the first element of a query object as an inclusive range,
         * the second one as an exclusive range, etc. Always returning 0 when the two BSONObjects
         * have identical values would only allow inclusive ranges across all elements of the query
         * object.
         *
         * To get around this, a query object uses its field names to describe what a given
         * field's behavior should be if it is being compared to a field with an equal value. An 'l'
         * indicates that the query should be considered less than the other object, a 'g' indicates
         * that the query should be considered greater than the other object,and a null byte
         * indicates that the query should be considered equal to the other object.
         *
         * Ex: {"": 5, "": "foo"} == {"": 5, "": "foo"}
         *     {"g": 5, "": "foo"} > {"": 5, "": "foo"}
         *     {"": 5, "l": "foo"} < {"": 5, "": "foo"}
         */
        int compare(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;

        /**
         * See the comment above comparison() for some important details.
         * Preps a query for compare(). Strips fieldNames if there are any.
         *
         * @param keyPrefix a BSONObj representing the beginning of a query
         *
         * @param prefixLen the number of fields, beginning with the first and ending with the
         * prefixLen'th, in keyPrefix to use as part of the query. Must be >= 0 and < the number of
         * elements in keyPrefix.
         *
         * @param prefixExclusive true if the first prefixLen elements in the query are exclusive,
         * and false otherwise
         *
         * @param keySuffix a vector of BSONElements. The first prefixLen elements in keySuffix are
         * ignored, while the remaining elements make up the remainder of the query (following the
         * first prefixLen elements of keyPrefix).
         *
         * @param suffixInclusive a vector of booleans, of the same length as keySuffix. Elements
         * less than prefixLen are ignored, while for all other indexes i, suffixInclusive[i] is
         * true iff keySuffix[i] is an inclusive part of the range.
         *
         * @param cursorDirection an int which indicates the cursor direction. 1 indicates a forward
         * cursor, and -1 indicates a reverse cursor.
         */
        static BSONObj makeQueryObject(const BSONObj& keyPrefix,
                                       int prefixLen,
                                       bool prefixExclusive,
                                       const vector<const BSONElement*>& keySuffix,
                                       const vector<bool>& suffixInclusive,
                                       const int cursorDirection);

    private:
        // Ordering is used in comparison() to compare BSONElements
        const Ordering _order;

    }; // struct IndexEntryComparison

} // namespace mongo
