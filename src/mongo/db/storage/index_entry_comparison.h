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
#include "mongo/bson/ordering.h"
#include "mongo/db/record_id.h"

namespace mongo {

    /**
     * Represents a single item in an index. An index item simply consists of a key
     * and a disk location.
     */
    struct IndexKeyEntry {
        IndexKeyEntry(const BSONObj& key, RecordId loc) :key(key), loc(loc) {}

        BSONObj key;
        RecordId loc;
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
         * Compares two IndexKeyEntries and returns -1 if lhs < rhs, 1 if lhs > rhs, and 0
         * otherwise.
         *
         * IndexKeyEntries are compared lexicographically field by field in the BSONObj, followed by
         * the RecordId. Either lhs or rhs (but not both) can be a query object returned by
         * makeQueryObject(). See makeQueryObject() for a description of how its arguments affect
         * the outcome of the comparison.
         */
        int compare(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;

        /**
         * Encodes the arguments into a query object suitable to pass in to compare().
         *
         * A query object is used for seeking an iterator to a position in a sorted index.  The
         * difference between a query object and the keys inserted into indexes is that query
         * objects can be exclusive. This means that the first matching entry in the index is the
         * first key in the index after the query. The meaning of "after" depends on
         * cursorDirection.
         *
         * The fields of the key are the combination of keyPrefix and keySuffix. The first prefixLen
         * keys of keyPrefix are used, as well as the keys starting at the prefixLen index of
         * keySuffix.  The first prefixLen elements of keySuffix are ignored.
         *
         * If a field is marked as exclusive, then comparisons stop after that field and return
         * either higher or lower, even if that field compares equal. If prefixExclusive is true and
         * prefixLen is greater than 0, then the last field in the prefix is marked as exclusive. It
         * is illegal to specify prefixExclusive as true with a prefixLen of 0. Each bool in
         * suffixInclusive, starting at index prefixLen, indicates whether the corresponding element
         * in keySuffix is inclusive or exclusive.
         *
         * Returned objects are for use in lookups only and should never be inserted into the
         * database, as their format may change. The only reason this is the same type as the
         * entries in an index is to support storage engines that require comparators that take
         * arguments of the same type.
         *
         * A cursurDirection of 1 indicates a forward cursor, and -1 indicates a reverse cursor.
         * This effects the result when the exclusive field compares equal.
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
