/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

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
     * Is the provided key pattern the index over the ID field?
     * The always required ID index is always {_id: 1} or {_id: -1}.
     */
    static bool isIdKeyPattern(const BSONObj& pattern);

    /**
     * Is the provided key pattern ordered increasing or decreasing or not?
     */
    static bool isOrderedKeyPattern(const BSONObj& pattern);

    /**
     * Does the provided key pattern hash its keys?
     */
    static bool isHashedKeyPattern(const BSONObj& pattern);

    /**
     * Constructs a new key pattern based on a BSON document
     */
    KeyPattern(const BSONObj& pattern);

    /**
     * Returns a BSON representation of this KeyPattern.
     */
    const BSONObj& toBSON() const {
        return _pattern;
    }

    /**
     * Returns a string representation of this KeyPattern
     */
    std::string toString() const {
        return toBSON().toString();
    }


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

private:
    BSONObj _pattern;
};

}  // namespace mongo
