// bits_test.cpp

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
