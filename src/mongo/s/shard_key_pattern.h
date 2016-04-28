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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/query/index_bounds.h"

namespace mongo {

class CanonicalQuery;
class FieldRef;
class OperationContext;

/**
 * Helper struct when generating flattened bounds below
 *
 * A BoundList contains intervals specified by inclusive start
 * and end bounds.  The intervals should be nonoverlapping and occur in
 * the specified direction of traversal.  For example, given a simple index {i:1}
 * and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
 * would be valid for index {i:-1} with direction -1.
 */
typedef std::vector<std::pair<BSONObj, BSONObj>> BoundList;

/**
 * A ShardKeyPattern represents the key pattern used to partition data in a collection between
 * shards.  Shard keys are extracted from documents, simple queries, or Matchable objects based
 * on the paths within the key pattern.
 *
 * Shard key pattern paths may be nested, but are not traversable through arrays - this means
 * a shard key pattern path always yields a single value.
 */
class ShardKeyPattern {
public:
    // Maximum size of shard key
    static const int kMaxShardKeySizeBytes;

    // Maximum number of intervals produced by $in queries.
    static const unsigned int kMaxFlattenedInCombinations;

    /**
     * Helper to check shard key size and generate an appropriate error message.
     */
    static Status checkShardKeySize(const BSONObj& shardKey);

    /**
     * Constructs a shard key pattern from a BSON pattern document.  If the document is not a
     * valid shard key pattern, !isValid() will be true and key extraction will fail.
     */
    explicit ShardKeyPattern(const BSONObj& keyPattern);

    /**
     * Constructs a shard key pattern from a key pattern, see above.
     */
    explicit ShardKeyPattern(const KeyPattern& keyPattern);

    bool isValid() const;

    bool isHashedPattern() const;

    const KeyPattern& getKeyPattern() const;

    const BSONObj& toBSON() const;

    std::string toString() const;

    /**
     * Returns true if the provided document is a shard key - i.e. has the same fields as the
     * shard key pattern and valid shard key values.
     */
    bool isShardKey(const BSONObj& shardKey) const;

    /**
     * Given a shard key, return it in normal form where the fields are in the same order as
     * the shard key pattern fields.
     *
     * If the shard key is invalid, returns BSONObj()
     */
    BSONObj normalizeShardKey(const BSONObj& shardKey) const;

    /**
     * Given a MatchableDocument, extracts the shard key corresponding to the key pattern.
     * For each path in the shard key pattern, extracts a value from the matchable document.
     *
     * Paths to shard key fields must not contain arrays at any level, and shard keys may not
     * be array fields, undefined, or non-storable sub-documents.  If the shard key pattern is
     * a hashed key pattern, this method performs the hashing.
     *
     * If a shard key cannot be extracted, returns an empty BSONObj().
     *
     * Examples:
     *  If 'this' KeyPattern is { a  : 1 }
     *   { a: "hi" , b : 4} --> returns { a : "hi" }
     *   { c : 4 , a : 2 } -->  returns { a : 2 }
     *   { b : 2 }  -> returns {}
     *   { a : [1,2] } -> returns {}
     *  If 'this' KeyPattern is { a  : "hashed" }
     *   { a: 1 } --> returns { a : NumberLong("5902408780260971510")  }
     *  If 'this' KeyPattern is { 'a.b' : 1 }
     *   { a : { b : "hi" } } --> returns { 'a.b' : "hi" }
     *   { a : [{ b : "hi" }] } --> returns {}
     */
    BSONObj extractShardKeyFromMatchable(const MatchableDocument& matchable) const;

    /**
     * Given a document, extracts the shard key corresponding to the key pattern.
     * See above.
     */
    BSONObj extractShardKeyFromDoc(const BSONObj& doc) const;

    /**
     * Given a simple BSON query, extracts the shard key corresponding to the key pattern
     * from equality matches in the query.  The query expression *must not* be a complex query
     * with sorts or other attributes.
     *
     * Logically, the equalities in the BSON query can be serialized into a BSON document and
     * then a shard key is extracted from this equality document.
     *
     * NOTE: BSON queries and BSON documents look similar but are different languages.  Use the
     * correct shard key extraction function.
     *
     * Returns !OK status if the query cannot be parsed.  Returns an empty BSONObj() if there is
     * no shard key found in the query equalities.
     *
     * Examples:
     *  If the key pattern is { a : 1 }
     *   { a : "hi", b : 4 } --> returns { a : "hi" }
     *   { a : { $eq : "hi" }, b : 4 } --> returns { a : "hi" }
     *   { $and : [{a : { $eq : "hi" }}, { b : 4 }] } --> returns { a : "hi" }
     *  If the key pattern is { 'a.b' : 1 }
     *   { a : { b : "hi" } } --> returns { 'a.b' : "hi" }
     *   { 'a.b' : "hi" } --> returns { 'a.b' : "hi" }
     *   { a : { b : { $eq : "hi" } } } --> returns {} because the query language treats this as
     *                                                 a : { $eq : { b : ... } }
     */
    StatusWith<BSONObj> extractShardKeyFromQuery(OperationContext* txn,
                                                 const BSONObj& basicQuery) const;
    StatusWith<BSONObj> extractShardKeyFromQuery(const CanonicalQuery& query) const;

    /**
     * Returns true if the shard key pattern can ensure that the unique index pattern is
     * respected across all shards.
     *
     * Primarily this just checks whether the shard key pattern field names are equal to or a
     * prefix of the unique index pattern field names.  Since documents with the same fields in
     * the shard key pattern are guaranteed to go to the same shard, and all documents must
     * contain the full shard key, a unique index with a shard key pattern prefix can be sure
     * when resolving duplicates that documents on other shards will have different shard keys,
     * and so are not duplicates.
     *
     * Hashed shard key patterns are similar to ordinary patterns in that they guarantee similar
     * shard keys go to the same shard.
     *
     * Examples:
     *     shard key {a : 1} is compatible with a unique index on {_id : 1}
     *     shard key {a : 1} is compatible with a unique index on {a : 1 , b : 1}
     *     shard key {a : 1} is compatible with a unique index on {a : -1 , b : 1 }
     *     shard key {a : "hashed"} is compatible with a unique index on {a : 1}
     *     shard key {a : 1} is not compatible with a unique index on {b : 1}
     *     shard key {a : "hashed" , b : 1 } is not compatible with unique index on { b : 1 }
     *
     * All unique index patterns starting with _id are assumed to be enforceable by the fact
     * that _ids must be unique, and so all unique _id prefixed indexes are compatible with
     * any shard key pattern.
     *
     * NOTE: We assume 'uniqueIndexPattern' is a valid unique index pattern - a pattern like
     * { k : "hashed" } is not capable of being a unique index and is an invalid argument to
     * this method.
     */
    bool isUniqueIndexCompatible(const BSONObj& uniqueIndexPattern) const;

    /**
     * Return an ordered list of bounds generated using this KeyPattern and the
     * bounds from the IndexBounds.  This function is used in sharding to
     * determine where to route queries according to the shard key pattern.
     *
     * Examples:
     *
     * Key { a: 1 }, Bounds a: [0] => { a: 0 } -> { a: 0 }
     * Key { a: 1 }, Bounds a: [2, 3) => { a: 2 } -> { a: 3 }  // bound inclusion ignored.
     *
     * The bounds returned by this function may be a superset of those defined
     * by the constraints.  For instance, if this KeyPattern is {a : 1, b: 1}
     * Bounds: { a : {$in : [1,2]} , b : {$in : [3,4,5]} }
     *         => {a : 1 , b : 3} -> {a : 1 , b : 5}, {a : 2 , b : 3} -> {a : 2 , b : 5}
     *
     * If the IndexBounds are not defined for all the fields in this keypattern, which
     * means some fields are unsatisfied, an empty BoundList could return.
     *
     */
    BoundList flattenBounds(const IndexBounds& indexBounds) const;

private:
    // Ordered, parsed paths
    const OwnedPointerVector<FieldRef> _keyPatternPaths;

    const KeyPattern _keyPattern;
};
}
