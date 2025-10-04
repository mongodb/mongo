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

/**
 * Defines a simple hash function class
 */


#include "mongo/bson/bsonelement.h"

#include <cstdint>

namespace mongo {

typedef int32_t HashSeed;

class BSONElementHasher {
    BSONElementHasher(const BSONElementHasher&) = delete;
    BSONElementHasher& operator=(const BSONElementHasher&) = delete;

public:
    /* The hash function we use can be given a seed, to effectively randomize it
     * by choosing from among a family of hash functions. When it is not specified,
     * use this.
     *
     * WARNING: do not change the hash see value. Hash-based sharding clusters will
     * expect that value to be zero.
     */
    static constexpr HashSeed const DEFAULT_HASH_SEED = 0;

    /* This computes a 64-bit hash of the value part of BSONElement "e",
     * preceded by the seed "seed".  Squashes element (and any sub-elements)
     * of the same canonical type, so hash({a:{b:4}}) will be the same
     * as hash({a:{b:4.1}}). In particular, this squashes doubles to 64-bit long
     * ints via truncation, so floating point values round towards 0 to the
     * nearest int representable as a 64-bit long.
     *
     * This function is used in the computation of hashed indexes
     * and hashed shard keys, and thus should not be changed unless
     * the associated "getKeys" and "makeSingleKey" method in the
     * hashindex type is changed accordingly.
     */
    static long long int hash64(const BSONElement& e, HashSeed seed);

private:
    BSONElementHasher();
};
}  // namespace mongo
