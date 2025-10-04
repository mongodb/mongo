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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/keypattern.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

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
    /**
     * A struct to represent the index key data. The 'data' field represents the actual key data and
     * the 'pattern' represents the index key pattern. For an index pattern {a: 1, b: 'hashed'} the
     * key data would look like {"": "value", "": NumberLong(12345)}.
     */
    struct IndexKeyData {
        BSONObj data;
        BSONObj pattern;
    };

    /**
     * Validates whether the specified shard key is valid to be written as part of the sharding
     * metadata.
     */
    static Status checkShardKeyIsValidForMetadataStorage(const BSONObj& shardKey);

    /**
     * Constructs a shard key pattern from a BSON pattern document.  If the document is not a
     * valid shard key pattern, !isValid() will be true and key extraction will fail.
     */
    explicit ShardKeyPattern(const BSONObj& keyPattern);

    /**
     * Constructs a shard key pattern from a key pattern, see above.
     */
    explicit ShardKeyPattern(const KeyPattern& keyPattern);

    /**
     * Returns whether the provided element is hashed.
     */
    static bool isHashedPatternEl(const BSONElement& el);

    /**
     * Returns the BSONElement pointing to the hashed field. Returns empty BSONElement if not found.
     */
    static BSONElement extractHashedField(BSONObj keyPattern);

    /**
     * Check if the given BSONElement is of type 'MinKey', 'MaxKey' or 'NumberLong', which are the
     * only acceptable values for hashed fields.
     */
    static bool isValidHashedValue(const BSONElement& el);

    bool isHashedPattern() const;

    bool isHashedOnField(StringData fieldName) const;

    bool hasHashedPrefix() const;

    BSONElement getHashedField() const {
        return _hashedField;
    }

    const KeyPattern& getKeyPattern() const {
        return _keyPattern;
    }

    const std::vector<std::unique_ptr<FieldRef>>& getKeyPatternFields() const {
        return _keyPatternPaths;
    }

    const BSONObj& toBSON() const;

    std::string toString() const;

    /**
     * Converts the passed in key pattern into a KeyString.
     * Note: this function strips the field names when creating the KeyString.
     */
    static std::string toKeyString(const BSONObj& shardKey);

    /**
     * Returns true if the provided document is a shard key - i.e. has the same fields as the
     * shard key pattern and valid shard key values.
     */
    bool isShardKey(const BSONObj& shardKey) const;

    /**
     * Returns true if the new shard key pattern extends this shard key pattern - i.e. contains this
     * shard key pattern as a prefix (begins with the same field names in the same order).
     */
    bool isExtendedBy(const ShardKeyPattern& newShardKeyPattern) const;

    /**
     * Given a shard key, return it in normal form where the fields are in the same order as
     * the shard key pattern fields.
     *
     * If the shard key is invalid, returns BSONObj()
     */
    BSONObj normalizeShardKey(const BSONObj& shardKey) const;

    /**
     * Given one or more index keys, potentially from more than one index, extracts the shard key
     * corresponding to the shard key pattern.
     *
     * All the shard key fields must be present in at least one of the index keys. A missing shard
     * key field will result in an invariant.
     */
    BSONObj extractShardKeyFromIndexKeyData(const std::vector<IndexKeyData>& indexKeyData) const;

    /**
     * Given a document key expressed in dotted notation, extracts its shard key, applying hashing
     * if necessary.
     * Note: For a shardKeyPattern {a.b: 1, c: 1}
     *  The documentKey for the document {a: {b: 10}, c: 20} is {a.b: 10, c: 20}
     *  The documentKey for the document {a: {b: 10, d: 20}, c: 30} is {a.b: 10, c: 30}
     *  The documentKey for the document {a: {b: {d: 10}}, c: 30} is {a.b: {d: 10}, c: 30}
     *
     * Examples:
     *  If 'this' KeyPattern is {a: 1}
     *   {a: 10, b: 20} --> returns {a: 10}
     *   {b: 20} --> returns {a: null}
     *   {a: {b: 10}} --> returns {a: {b: 10}}
     *   {a: [1,2]} --> returns {}
     *  If 'this' KeyPattern is {a.b: 1, c: 1}
     *   {a.b: 10, c: 20} --> returns {a.b: 10, c: 20}
     *   {a.b: 10} --> returns {a.b: 10, c: null}
     *   {a.b: {z: 10}, c: 20} --> returns {a.b: {z: 10}, c: 20}
     *  If 'this' KeyPattern is {a : "hashed"}
     *   {a: 10, b: 20} --> returns {a: NumberLong("7766103514953448109")}
     *   {b: 20} --> returns {a: NumberLong("2338878944348059895")}
     */
    BSONObj extractShardKeyFromDocumentKey(const BSONObj& documentKey) const;
    BSONObj extractShardKeyFromDocumentKeyThrows(const BSONObj& documentKey) const;

    /**
     * Given a document, extracts the shard key corresponding to the key pattern. Paths to shard key
     * fields must not contain arrays at any level, and shard keys may not be array fields or
     * non-storable sub-documents.  If the shard key pattern is a hashed key pattern, this method
     * performs the hashing.
     *
     * If any shard key fields are missing from the document, the extraction will treat these
     * fields as null.
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
     *  If 'this' KeyPattern is { a: 1 , b: 1 }
     *   { a: 1 } --> returns { a: 1, b: null }
     *   { b: 1 } --> returns { a: null, b: 1 }
     */
    BSONObj extractShardKeyFromDoc(const BSONObj& doc) const;
    BSONObj extractShardKeyFromDocThrows(const BSONObj& doc) const;

    /**
     * Returns the document with missing shard key values set to null.
     */
    BSONObj emplaceMissingShardKeyValuesForDocument(BSONObj doc) const;

    /**
     * Returns true if the shard key pattern can ensure that the index uniqueness is respected
     * across all shards.
     *
     * Primarily this just checks whether the shard key pattern field names are equal to or a
     * prefix of the 'unique' or 'prepareUnique' index pattern field names. Since documents with the
     * same fields in the shard key pattern are guaranteed to go to the same shard, and all
     * documents must contain the full shard key, an index with {unique: true} or {prepareUnique:
     * true} and a shard key pattern prefix can be sure when resolving duplicates that documents on
     * other shards will have different shard keys, and so are not duplicates.
     *
     * Hashed shard key patterns are similar to ordinary patterns in that they guarantee similar
     * shard keys go to the same shard.
     *
     * Examples:
     *     shard key {a : 1} is compatible with a unique/prepareUnique index on {_id : 1}
     *     shard key {a : 1} is compatible with a unique/prepareUnique index on {a : 1, b : 1}
     *     shard key {a : 1} is compatible with a unique/prepareUnique index on {a : -1, b : 1}
     *     shard key {a : "hashed"} is compatible with a unique/prepareUnique index on {a : 1}
     *     shard key {a : 1} is not compatible with a unique/prepareUnique index on {b : 1}
     *     shard key {a : "hashed", b : 1} is not compatible with unique/prepareUnique index on
     *     {b : 1}
     *
     * All unique index patterns starting with _id are assumed to be enforceable by the fact
     * that _ids must be unique, and so all unique _id prefixed indexes are compatible with
     * any shard key pattern.
     *
     * NOTE: We assume 'indexPattern' is a valid unique/prepareUnique index pattern - a pattern like
     * { k : "hashed" } is not capable of being a unique/prepareUnique index and is an invalid
     * argument to this method.
     */
    bool isIndexUniquenessCompatible(const BSONObj& indexPattern) const;

    /**
     * Returns true if the key pattern has an "_id" field of any flavor.
     */
    bool hasId() const {
        return _hasId;
    };

    size_t getApproximateSize() const;

    static bool isValidShardKeyElementForStorage(const BSONElement& element);

private:
    KeyPattern _keyPattern;

    // Ordered, parsed paths
    std::vector<std::unique_ptr<FieldRef>> _keyPatternPaths;

    bool _hasId;
    BSONElement _hashedField;
};

}  // namespace mongo
