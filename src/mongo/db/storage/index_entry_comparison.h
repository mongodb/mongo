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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

    void serialize(BSONObjBuilder* builder) const {
        builder->append("key"_sd, key);
        loc.serializeToken("RecordId", builder);
    }

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
 * Represents KeyString struct containing a key_string::Value and its RecordId
 */
struct KeyStringEntry {
    KeyStringEntry(key_string::Value ks, RecordId id) : keyString(ks), loc(std::move(id)) {
        if (!kDebugBuild) {
            return;
        }
        invariant(loc == key_string::decodeRecordIdAtEnd(ks.getView(), loc.keyFormat()));
    }

    key_string::Value keyString;
    RecordId loc;
};

/**
 * Describes a query that can be compared against an IndexKeyEntry in a way that allows
 * expressing exclusiveness on a prefix of the key. This is mostly used to express a location to
 * seek to in an index that may not be representable as a valid key.
 *
 * If 'firstExclusive' is negative, the "key" used for comparison is the concatenation of the first
 * 'prefixLen' elements of 'keyPrefix' followed by the last 'keySuffix.size() - prefixLen' elements
 * of 'keySuffix'.
 *
 * The comparison is exclusive if 'firstExclusive' is non-negative, and any portion of the key after
 * that index will be omitted. The value of 'firstExclusive' must be at least 'prefixLen' - 1.
 *
 * e.g.
 *
 *  Suppose that
 *
 *      keyPrefix = { "" : 1, "" : 2 }
 *      prefixLen = 1
 *      keySuffix = [ IGNORED, { "" : 5 }, { "" : 9 } ]
 *      firstExclusive = 1
 *
 *      ==> key is { "" : 1, "" : 5 }
 *          with the comparison being done exclusively
 *
 *  Suppose that
 *
 *      keyPrefix = { "" : 1, "" : 2 }
 *      prefixLen = 1
 *      keySuffix = IGNORED
 *      firstExclusive = 0
 *
 *      ==> represented key is { "" : 1 }
 *          with the comparison being done exclusively
 *
 *  Suppose that
 *
 *      keyPrefix = { "" : 1, "" : 2 }
 *      prefixLen = 1
 *      keySuffix = [ IGNORED, { "" : 5 }, { "" : 9 } ]
 *      firstExclusive = -1
 *
 *      ==> key is { "" : 1, "" : 5, "" : 9 }
 *          with the comparison being done inclusively
 */
struct IndexSeekPoint {
    BSONObj keyPrefix;

    /**
     * Use this many fields in 'keyPrefix'.
     */
    int prefixLen = 0;

    /**
     * Elements starting at index 'prefixLen' are logically appended to the prefix.
     * The elements before index 'prefixLen' should be ignored.
     */
    std::vector<BSONElement> keySuffix;

    /**
     * If non-negative, then the comparison will be exclusive and any elements after index
     * 'firstExclusive' are ignored. Otherwise all elements are considered and the comparison will
     * be inclusive.
     */
    int firstExclusive = -1;
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
     * Given a KeyString Builder reference, encodes the SeekPoint into the builder and returns its
     * owned buffer suitable to pass in to seek().
     *
     * A KeyString is used for seeking an iterator to a position in a sorted index. The difference
     * between a query KeyString and the KeyStrings inserted into indexes is that a query KeyString
     * can have an exclusive discriminator, which forces the key to compare less than or greater to
     * a matching key in the index. This means that the first matching key is not returned, but
     * rather the one immediately after. The meaning of "after" depends on the cursor directory,
     * isForward.
     *
     * If a field is marked as exclusive, then comparisons stop after that field and return
     * either higher or lower, even if that field compares equal. If firstExclusive is non-negative,
     * then the field at the corresponding index is marked as exclusive and any subsequent fields
     * are ignored.
     *
     * Returned buffers are for use in lookups only and should never be inserted into the
     * database, as their format may change.
     */
    static std::span<const char> makeKeyStringFromSeekPointForSeek(const IndexSeekPoint& seekPoint,
                                                                   bool isForward,
                                                                   key_string::Builder& builder);

    /**
     * Given a KeyString Builder reference, encodes the BSON Key into the builder and returns its
     * owned buffer to pass in to SortedDataInterface::seek().
     *
     * `isForward` and `inclusive` together decide which discriminator we will put into the
     * KeyString. This logic is closely related to how WiredTiger uses its API
     * (prev/next) to do the seek. Other storage engines' SortedDataInterface should use
     * the discriminator to deduce the `inclusive` and the use their own ways to seek to the right
     * position.
     *
     * 1. When isForward == true, inclusive == true, bsonKey will be encoded with kExclusiveBefore
     * (which is less than bsonKey).
     *
     * 2. When isForward == true, inclusive == false, bsonKey will be encoded with kExclusiveAfter
     * (which is greater than bsonKey).
     *
     * 3. When isForward == false, inclusive == true, bsonKey will be encoded with kExclusiveAfter
     * (which is greater than bsonKey).
     *
     * 4. When isForward == false, inclusive == false, bsonKey will be encoded with kExclusiveBefore
     * (which is less than bsonKey).
     */
    static std::span<const char> makeKeyStringFromBSONKeyForSeek(const BSONObj& bsonKey,
                                                                 Ordering ord,
                                                                 bool isForward,
                                                                 bool inclusive,
                                                                 key_string::Builder& builder);

    /**
     * Given a KeyString Builder reference, encodes the BSON Key into the builder and returns its
     * owned buffer to pass in to SortedDataInterface::seek().
     *
     * This function is similar to IndexEntryComparison::makeKeyStringFromBSONKeyForSeek()
     * but allows you to pick your own key_string::Discriminator based on wether or not the
     * resulting KeyString is for the start key or end key of a seek.
     */
    static std::span<const char> makeKeyStringFromBSONKey(const BSONObj& bsonKey,
                                                          Ordering ord,
                                                          key_string::Discriminator discrim,
                                                          key_string::Builder& builder);

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
                              const BSONObj& keyPattern,
                              const BSONObj& indexCollation,
                              DuplicateKeyErrorInfo::FoundValue&& foundValue = std::monostate{},
                              boost::optional<RecordId> duplicateRid = boost::none);

Status buildDupKeyErrorStatus(const key_string::Value& keyString,
                              const NamespaceString& collectionNamespace,
                              const std::string& indexName,
                              const BSONObj& keyPattern,
                              const BSONObj& indexCollation,
                              const Ordering& ordering);

}  // namespace mongo
