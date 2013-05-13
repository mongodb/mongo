// @file util.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/pch.h"

#include <iomanip>

#include "mongo/platform/atomic_word.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/goodies.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

    string hexdump(const char *data, unsigned len) {
        verify( len < 1000000 );
        const unsigned char *p = (const unsigned char *) data;
        stringstream ss;
        for( unsigned i = 0; i < 4 && i < len; i++ ) {
            ss << std::hex << setw(2) << setfill('0');
            unsigned n = p[i];
            ss << n;
            ss << ' ';
        }
        string s = ss.str();
        return s;
    }

    bool isPrime(int n) {
        int z = 2;
        while ( 1 ) {
            if ( z*z > n )
                break;
            if ( n % z == 0 )
                return false;
            z++;
        }
        return true;
    }

    int nextPrime(int n) {
        n |= 1; // 2 goes to 3...don't care...
        while ( !isPrime(n) )
            n += 2;
        return n;
    }

    struct UtilTest : public StartupTest {
        void run() {
            verify( isPrime(3) );
            verify( isPrime(2) );
            verify( isPrime(13) );
            verify( isPrime(17) );
            verify( !isPrime(9) );
            verify( !isPrime(6) );
            verify( nextPrime(4) == 5 );
            verify( nextPrime(8) == 11 );

            verify( endsWith("abcde", "de") );
            verify( !endsWith("abcde", "dasdfasdfashkfde") );

            verify( swapEndian(0x01020304) == 0x04030201 );

        }
    } utilTest;

    ostream& operator<<( ostream &s, const ThreadSafeString &o ) {
        s << o.toString();
        return s;
    }

    bool StaticObserver::_destroyingStatics = false;

} // namespace mongo
