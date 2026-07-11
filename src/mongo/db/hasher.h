// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * Defines a simple hash function class
 */


#include "mongo/bson/bsonelement.h"
#include "mongo/util/modules.h"

#include <cstdint>

[[MONGO_MOD_PUBLIC]];
namespace mongo {

class BSONElementHasher {
    BSONElementHasher(const BSONElementHasher&) = delete;
    BSONElementHasher& operator=(const BSONElementHasher&) = delete;

public:
    using HashSeed = int32_t;

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
