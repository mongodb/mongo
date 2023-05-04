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
#include "mongo/bson/util/builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/util/str.h"

namespace mongo {

/**
 * A KeyPattern is an expression describing a transformation of a document into a
 * document key.  Document keys are used to store documents in indices and to target
 * sharded queries.
 *
 * The root field names of KeyPatterns are always (potentially-dotted) paths, and the values of
 * the fields describe the type of indexing over the found elements.
 *
 * Examples:
 *    { a : 1 }
 *    { a : 1 , b  : -1 }
 *    { a : "hashed" }
 */
class KeyPattern {
public:
    /**
     * Is the provided key pattern ordered increasing or decreasing or not?
     */
    static bool isOrderedKeyPattern(const BSONObj& pattern);

    /**
     * Does the provided key pattern hash its keys?
     */
    static bool isHashedKeyPattern(const BSONObj& pattern);

    /**
     * Constructs a new key pattern based on a BSON document.
     * Used as an interface to the IDL parser.
     */
    static KeyPattern fromBSON(const BSONObj& pattern) {
        return KeyPattern(pattern.getOwned());
    }

    /**
     * Constructs a new key pattern based on a BSON document.
     */
    KeyPattern(const BSONObj& pattern);

    explicit KeyPattern() = default;

    /**
     * Returns a BSON representation of this KeyPattern.
     */
    const BSONObj& toBSON() const {
        return _pattern;
    }

    BSONObj serializeForIDL(const SerializationOptions& options = {}) const {
        BSONObjBuilder bob;
        for (const auto& e : _pattern) {
            bob.appendAs(e, options.serializeIdentifier(e.fieldNameStringData()));
        }
        return bob.obj();
    }

    /**
     * Returns a string representation of this KeyPattern.
     */
    std::string toString() const {
        return str::stream() << *this;
    }

    /**
     * Writes to 'sb' a string representation of this KeyPattern.
     */
    friend StringBuilder& operator<<(StringBuilder& sb, const KeyPattern& keyPattern);

    /* Takes a BSONObj whose field names are a prefix of the fields in this keyPattern, and
     * outputs a new bound with MinKey values appended to match the fields in this keyPattern
     * (or MaxKey values for descending -1 fields). This is useful in sharding for
     * calculating chunk boundaries when tag ranges are specified on a prefix of the actual
     * shard key, or for calculating index bounds when the shard key is a prefix of the actual
     * index used.
     *
     * @param makeUpperInclusive If true, then MaxKeys instead of MinKeys will be appended, so
     * that the output bound will compare *greater* than the bound being extended (note that
     * -1's in the keyPattern will swap MinKey/MaxKey vals. See examples).
     *
     * Examples:
     * If this keyPattern is {a : 1}
     *   extendRangeBound( {a : 55}, false) --> {a : 55}
     *
     * If this keyPattern is {a : 1, b : 1}
     *   extendRangeBound( {a : 55}, false) --> {a : 55, b : MinKey}
     *   extendRangeBound( {a : 55}, true ) --> {a : 55, b : MaxKey}
     *
     * If this keyPattern is {a : 1, b : -1}
     *   extendRangeBound( {a : 55}, false) --> {a : 55, b : MaxKey}
     *   extendRangeBound( {a : 55}, true ) --> {a : 55, b : MinKey}
     */
    BSONObj extendRangeBound(const BSONObj& bound, bool makeUpperInclusive) const;

    BSONObj globalMin() const;

    BSONObj globalMax() const;

    size_t getApproximateSize() const;

private:
    BSONObj _pattern;
};

}  // namespace mongo
