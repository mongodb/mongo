// random_test.cpp


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

#include <set>

#include "mongo/platform/random.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST( RandomTest, Seed1 ) {
        PseudoRandom a( 12 );
        PseudoRandom b( 12 );

        for ( int i = 0; i < 100; i++ ) {
            ASSERT_EQUALS( a.nextInt32(), b.nextInt32() );
        }
    }

    TEST( RandomTest, Seed2 ) {
        PseudoRandom a( 12 );
        PseudoRandom b( 12 );

        for ( int i = 0; i < 100; i++ ) {
            ASSERT_EQUALS( a.nextInt64(), b.nextInt64() );
        }
    }

    TEST( RandomTest, Seed3 ) {
        PseudoRandom a( 11 );
        PseudoRandom b( 12 );

        ASSERT_NOT_EQUALS( a.nextInt32(), b.nextInt32() );
    }

    TEST( RandomTest, Seed4 ) {
        PseudoRandom a( 11 );
        std::set<int32_t> s;
        for ( int i = 0; i < 100; i++ ) {
            s.insert( a.nextInt32() );
        }
        ASSERT_EQUALS( 100U, s.size() );
    }

    TEST( RandomTest, Seed5 ) {
        const int64_t seed = 0xCC453456FA345FABLL;
        PseudoRandom a(seed);
        std::set<int32_t> s;
        for ( int i = 0; i < 100; i++ ) {
            s.insert( a.nextInt32() );
        }
        ASSERT_EQUALS( 100U, s.size() );
    }

    TEST( RandomTest, R1 ) {
        PseudoRandom a( 11 );
        std::set<int32_t> s;
        for ( int i = 0; i < 100; i++ ) {
            s.insert( a.nextInt32() );
        }
        ASSERT_EQUALS( 100U, s.size() );
    }

    TEST( RandomTest, R2 ) {
        PseudoRandom a( 11 );
        std::set<int64_t> s;
        for ( int i = 0; i < 100; i++ ) {
            s.insert( a.nextInt64() );
        }
        ASSERT_EQUALS( 100U, s.size() );
    }


    TEST( RandomTest, Secure1 ) {
        SecureRandom* a = SecureRandom::create();
        SecureRandom* b = SecureRandom::create();

        for ( int i = 0; i< 100; i++ ) {
            ASSERT_NOT_EQUALS( a->nextInt64(), b->nextInt64() );
        }

        delete a;
        delete b;

    }
}
