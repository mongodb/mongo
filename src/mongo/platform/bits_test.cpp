// bits_test.cpp

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

#include "mongo/platform/bits.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST( BitsTest, HeaderCorrect ) {
#if defined(MONGO_PLATFORM_64)
        ASSERT_EQUALS( 8U, sizeof(char*) );
#elif defined(MONGO_PLATFORM_32)
        ASSERT_EQUALS( 4U, sizeof(char*) );
#else
        ASSERT( false );
#endif
    }

#if defined(MONGO_SYSTEM_FFS)
    TEST( BitsTest, FIRST_BIT_SET ) {
        ASSERT_EQUALS( firstBitSet(0), mongo_firstBitSet(0) );
        ASSERT_EQUALS( firstBitSet(0x1234), mongo_firstBitSet(0x1234) );
        
        for ( int i = 0; i < 64; i++ ) {
            unsigned long long x = 1ULL << i;
            ASSERT_EQUALS( firstBitSet(x), mongo_firstBitSet(x) );
            x &= 0x5;
            ASSERT_EQUALS( firstBitSet(x), mongo_firstBitSet(x) );
        }
    }
#endif
}
