/* hasher.h
 *
 * Defines a simple hash function class
 */


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

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/md5.hpp"

namespace mongo {

typedef int HashSeed;
typedef unsigned char HashDigest[16];

class Hasher {
    MONGO_DISALLOW_COPYING(Hasher);

public:
    explicit Hasher(HashSeed seed);
    ~Hasher(){};

    // pointer to next part of input key, length in bytes to read
    void addData(const void* keyData, size_t numBytes);

    // finish computing the hash, put the result in the digest
    // only call this once per Hasher
    void finish(HashDigest out);

private:
    md5_state_t _md5State;
    HashSeed _seed;
};

class HasherFactory {
    MONGO_DISALLOW_COPYING(HasherFactory);

public:
    /* Eventually this may be a more sophisticated factory
     * for creating other hashers, but for now use MD5.
     */
    static Hasher* createHasher(HashSeed seed) {
        return new Hasher(seed);
    }

private:
    HasherFactory();
};

class BSONElementHasher {
    MONGO_DISALLOW_COPYING(BSONElementHasher);

public:
    /* The hash function we use can be given a seed, to effectively randomize it
     * by choosing from among a family of hash functions. When it is not specified,
     * use this.
     *
     * WARNING: do not change the hash see value. Hash-based sharding clusters will
     * expect that value to be zero.
     */
    static const int DEFAULT_HASH_SEED = 0;

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

    /* This incrementally computes the hash of BSONElement "e"
     * using hash function "h".  If "includeFieldName" is true,
     * then the name of the field is hashed in between the type of
     * the element and the element value.  The hash function "h"
     * is applied recursively to any sub-elements (arrays/sub-documents),
     * squashing elements of the same canonical type.
     * Used as a helper for hash64 above.
     */
    static void recursiveHash(Hasher* h, const BSONElement& e, bool includeFieldName);

private:
    BSONElementHasher();
};
}
