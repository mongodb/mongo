// random_test.cpp


/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
