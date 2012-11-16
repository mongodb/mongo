// random.h

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/platform/cstdint.h"

namespace mongo {

    /**
     * Uses http://en.wikipedia.org/wiki/Xorshift
     */
    class PseudoRandom {
    public:
        PseudoRandom( int32_t seed );

        PseudoRandom( uint32_t seed );

        PseudoRandom( int64_t seed );

        int32_t nextInt32();

        int64_t nextInt64();

        /**
         * @return a number between 0 and max
         */
        int32_t nextInt32( int32_t max ) { return nextInt32() % max; }

    private:
        int32_t _x;
        int32_t _y;
        int32_t _z;
        int32_t _w;
    };

    /**
     * More secure random numbers
     * Suitable for nonce/crypto
     * Slower than PseudoRandom, so only use when really need
     */
    class SecureRandom {
    public:
        virtual ~SecureRandom();

        virtual int64_t nextInt64() = 0;

        static SecureRandom* create();
    };

}
