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

#include <iosfwd>
#include <tuple>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"

namespace mongo {

/**
 * Represents a single item in an index. An index item simply consists of a key
 * and a disk location.
 */
struct IndexKeyEntry {
    IndexKeyEntry(BSONObj key, RecordId loc) : key(std::move(key)), loc(std::move(loc)) {}

    BSONObj key;
    RecordId loc;
};

std::ostream& operator<<(std::ostream& stream, const IndexKeyEntry& entry);

inline bool operator==(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) {
    return std::tie(lhs.key, lhs.loc) == std::tie(rhs.key, rhs.loc);
}

inline bool operator!=(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) {
    return std::tie(lhs.key, lhs.loc) != std::tie(rhs.key, rhs.loc);
}

/**
 * Describes a query that can be compared against an IndexKeyEntry in a way that allows
 * expressing exclusiveness on a prefix of the key. This is mostly used to express a location to
 * seek to in an index that may not be representable as a valid key.
 *
 * The "key" used for comparison is the concatenation of the first 'prefixLen' elements of
 * 'keyPrefix' followed by the last 'keySuffix.size() - prefixLen' elements of
 * 'keySuffix'.
 *
 * The comparison is exclusive if either 'prefixExclusive' is true or if there are any false
 * values in 'suffixInclusive' that are false at index >= 'prefixLen'.
 *
 * Portions of the key following the first exclusive part may be ignored.
 *
 * e.g.
 *
 *  Suppose that
 *
 *      keyPrefix = { "" : 1, "" : 2 }
 *      prefixLen = 1
 *      prefixExclusive = false
 *      keySuffix = [ IGNORED, { "" : 5 } ]
 *      suffixInclusive = [ IGNORED, false ]
 *
 *      ==> key is { "" : 1, "" : 5 }
 *          with the comparison being done exclusively
 *
 *  Suppose that
 *
 *      keyPrefix = { "" : 1, "" : 2 }
 *      prefixLen = 1
 *      prefixExclusive = true
 *      keySuffix = IGNORED
 *      suffixInclusive = IGNORED
 *
 *      ==> represented key is { "" : 1 }
 *          with the comparison being done exclusively
 *
 * 'prefixLen = 0' and 'prefixExclusive = true' are mutually incompatible.
 *
 * @see IndexEntryComparison::makeQueryObject
 */
struct IndexSeekPoint {
    BSONObj keyPrefix;

    /**
     * Use this many fields in 'keyPrefix'.
     */
    int prefixLen = 0;

    /**
     * If true, compare exclusively on just the fields on keyPrefix and ignore the suffix.
     */
    bool prefixExclusive = false;

    /**
     * Elements starting at index 'prefixLen' are logically appended to the prefix.
     * The elements before index 'prefixLen' should be ignored.
     */
    std::vector<const BSONElement*> keySuffix;

    /**
     * If the ith element is false, ignore indexes > i in keySuffix and treat the
     * concatenated key as exclusive.
     * The elements before index 'prefixLen' should be ignored.
     *
     * Must have identical size as keySuffix.
     */
    std::vector<bool> suffixInclusive;
};

/**
 * Compares two different IndexKeyEntry instances.
 * The existence of compound indexes necessitates some complicated logic. This is meant to
 * support the comparisons of IndexKeyEntries (that are stored in an index) with IndexSeekPoints
 * (that were encoded with makeQueryObject) to support fine-grained control over whether the
 * ranges of various keys comprising a compound index are inclusive or exclusive.
 */
class IndexEntryComparison {
public:
    IndexEntryComparison(Ordering order) : _order(order) {}

    bool operator()(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;

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
                                   const std::vector<const BSONElement*>& keySuffix,
                                   const std::vector<bool>& suffixInclusive,
                                   const int cursorDirection);

    static BSONObj makeQueryObject(const IndexSeekPoint& seekPoint, bool isForward) {
        return makeQueryObject(seekPoint.keyPrefix,
                               seekPoint.prefixLen,
                               seekPoint.prefixExclusive,
                               seekPoint.keySuffix,
                               seekPoint.suffixInclusive,
                               isForward ? 1 : -1);
    }

private:
    // Ordering is used in comparison() to compare BSONElements
    const Ordering _order;

};  // struct IndexEntryComparison

}  // namespace mongo
