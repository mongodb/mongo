/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string.h"

namespace mongo {

/**
 * Represents a single item in an index. An index item simply consists of a key
 * and a disk location.
 */
struct IndexKeyEntry {
    /**
     * Given an index key 'dehyratedKey' with no field names, returns a new BSONObj representing the
     * index key after adding field names according to 'keyPattern'.
     */
    static BSONObj rehydrateKey(const BSONObj& keyPattern, const BSONObj& dehydratedKey) {
        BSONObjBuilder bob;
        BSONObjIterator keyIter(keyPattern);
        BSONObjIterator valueIter(dehydratedKey);

        while (keyIter.more() && valueIter.more()) {
            bob.appendAs(valueIter.next(), keyIter.next().fieldNameStringData());
        }

        invariant(!keyIter.more());
        invariant(!valueIter.more());

        return bob.obj();
    }

    IndexKeyEntry(BSONObj key, RecordId loc) : key(std::move(key)), loc(std::move(loc)) {}

    BSONObj key;
    RecordId loc;
};

std::ostream& operator<<(std::ostream& stream, const IndexKeyEntry& entry);

inline bool operator==(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.key == rhs.key) && (lhs.loc == rhs.loc);
}

inline bool operator!=(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) {
    return !(lhs == rhs);
}

/**
 * Represents KeyString struct containing a KeyString::Value and its RecordId
 */
struct KeyStringEntry {
    KeyStringEntry(KeyString::Value ks, RecordId loc) : keyString(ks), loc(loc) {
        invariant(loc == KeyString::decodeRecordIdAtEnd(ks.getBuffer(), ks.getSize()));
    }

    KeyString::Value keyString;
    RecordId loc;
};

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
 * Compares two different IndexKeyEntry instances. The existence of compound indexes necessitates
 * some complicated logic. This is meant to support the comparisons of IndexKeyEntries (that are
 * stored in an index) with IndexSeekPoints to support fine-grained control over whether the ranges
 * of various keys comprising a compound index are inclusive or exclusive.
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
     * the RecordId.
     */
    int compare(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;

    /**
     * Encodes the SeekPoint into a KeyString object suitable to pass in to seek().
     *
     * A KeyString is used for seeking an iterator to a position in a sorted index. The difference
     * between a query KeyString and the KeyStrings inserted into indexes is that a query KeyString
     * can have an exclusive discriminator, which forces the key to compare less than or greater to
     * a matching key in the index. This means that the first matching key is not returned, but
     * rather the one immediately after. The meaning of "after" depends on the cursor directory,
     * isForward.
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
     */
    static KeyString::Value makeKeyStringFromSeekPointForSeek(const IndexSeekPoint& seekPoint,
                                                              KeyString::Version version,
                                                              Ordering ord,
                                                              bool isForward);

    /**
     * Encodes the BSON Key into a KeyString object to pass in to SortedDataInterface::seek().
     *
     * `isForward` and `inclusive` together decide which discriminator we will put into the
     * KeyString. This logic is closely related to how WiredTiger uses its API
     * (search_near/prev/next) to do the seek. Other storage engines' SortedDataInterface should use
     * the discriminator to deduce the `inclusive` and the use their own ways to seek to the right
     * position.
     *
     * 1. When isForward == true, inclusive == true, bsonKey will be encoded with kExclusiveBefore
     * (which is less than bsonKey). WT's search_near() could land either on the previous key or
     * bsonKey. WT will selectively call next() if it's on the previous key.
     *
     * 2. When isForward == true, inclusive == false, bsonKey will be encoded with kExclusiveAfter
     * (which is greater than bsonKey). WT's search_near() could land either on bsonKey or the next
     * key. WT will selectively call next() if it's on bsonKey.
     *
     * 3. When isForward == false, inclusive == true, bsonKey will be encoded with kExclusiveAfter
     * (which is greater than bsonKey). WT's search_near() could land either on bsonKey or the next
     * key. WT will selectively call prev() if it's on the next key.
     *
     * 4. When isForward == false, inclusive == false, bsonKey will be encoded with kExclusiveBefore
     * (which is less than bsonKey). WT's search_near() could land either on the previous key or the
     * bsonKey. WT will selectively call prev() if it's on bsonKey.
     */
    static KeyString::Value makeKeyStringFromBSONKeyForSeek(const BSONObj& bsonKey,
                                                            KeyString::Version version,
                                                            Ordering ord,
                                                            bool isForward,
                                                            bool inclusive);

private:
    // Ordering is used in comparison() to compare BSONElements
    const Ordering _order;

};  // struct IndexEntryComparison

/**
 * Returns the formatted error status about the duplicate key.
 */
Status buildDupKeyErrorStatus(const BSONObj& key,
                              const NamespaceString& collectionNamespace,
                              const std::string& indexName,
                              const BSONObj& keyPattern);

/**
 * Returns the formatted error status about the duplicate KeyString.
 */
Status buildDupKeyErrorStatus(const KeyString::Value& keyString,
                              const NamespaceString& collectionNamespace,
                              const std::string& indexName,
                              const BSONObj& keyPattern,
                              const Ordering& ordering);

}  // namespace mongo
