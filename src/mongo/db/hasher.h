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
*/

#pragma once

#include "mongo/pch.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/md5.hpp"

namespace mongo {

    typedef int HashSeed;
    typedef unsigned char HashDigest[16];

    class Hasher : private boost::noncopyable {
    public:

        explicit Hasher( HashSeed seed );
        ~Hasher() { };

        //pointer to next part of input key, length in bytes to read
        void addData( const void * keyData , size_t numBytes );

        //finish computing the hash, put the result in the digest
        //only call this once per Hasher
        void finish( HashDigest out );

    private:
        md5_state_t _md5State;
        HashSeed _seed;
    };

    class HasherFactory : private boost::noncopyable  {
    public:
        /* Eventually this may be a more sophisticated factory
         * for creating other hashers, but for now use MD5.
         */
        static Hasher* createHasher( HashSeed seed ) {
            return new Hasher( seed );
        }

    private:
        HasherFactory();
    };

    class BSONElementHasher : private boost::noncopyable  {
    public:

        /* The hash function we use can be given a seed, to effectively randomize it
         * by choosing from among a family of hash functions. When it is not specified,
         * use this.
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
        static long long int hash64( const BSONElement& e , HashSeed seed );

    private:
        BSONElementHasher();

        /* This incrementally computes the hash of BSONElement "e"
         * using hash function "h".  If "includeFieldName" is true,
         * then the name of the field is hashed in between the type of
         * the element and the element value.  The hash function "h"
         * is applied recursively to any sub-elements (arrays/sub-documents),
         * squashing elements of the same canonical type.
         * Used as a helper for hash64 above.
         */
        static void recursiveHash( Hasher* h , const BSONElement& e , bool includeFieldName );

    };

}
