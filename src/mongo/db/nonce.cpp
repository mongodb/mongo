// nonce.cpp

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

#include "pch.h"
#include "nonce.h"
#include "../util/time_support.h"
#include <fstream>

extern int do_md5_test(void);

namespace mongo {

    BOOST_STATIC_ASSERT( sizeof(nonce64) == 8 );

    static Security security; // needs to be static so _initialized is preset to false (see initsafe below)

    Security::Security() {
        static int n;
        massert( 10352 , "Security is a singleton class", ++n == 1);
        init();
    }

    NOINLINE_DECL void Security::init() {
        if( _initialized ) return;
        _initialized = true;

#if defined(__linux__) || defined(__sunos__) || defined(__APPLE__)
        _devrandom = new ifstream("/dev/urandom", ios::binary|ios::in);
        if ( !_devrandom->is_open() )
            massert( 10353 , std::string("can't open dev/urandom: ") + strerror(errno), 0 );
#elif defined(_WIN32)
        srand(curTimeMicros()); // perhaps not relevant for rand_s but we might want elsewhere anyway
#else
        srandomdev();
#endif

#ifndef NDEBUG
        if ( do_md5_test() )
            massert( 10354 , "md5 unit test fails", false);
#endif
    }

    nonce64 Security::__getNonce() { 
        dassert( _initialized );
        nonce64 n;
#if defined(__linux__) || defined(__sunos__) || defined(__APPLE__)
        _devrandom->read((char*)&n, sizeof(n));
        massert(10355 , "devrandom failed", !_devrandom->fail());
#elif defined(_WIN32)
        unsigned a=0, b=0;
        verify( rand_s(&a) == 0 );
        verify( rand_s(&b) == 0 );
        n = (((unsigned long long)a)<<32) | b;
#else
        n = (((unsigned long long)random())<<32) | random();
#endif
        return n;
    }

    SimpleMutex nonceMutex("nonce");
    nonce64 Security::_getNonce() {
        SimpleMutex::scoped_lock lk(nonceMutex);
        if( !_initialized )
            init();
        return __getNonce();
    }

    nonce64 Security::getNonceDuringInit() {
        // the mutex might not be inited yet.  init phase should be one thread anyway (hopefully we don't spawn threads therein)
        if( !security._initialized )
            security.init();
        return security.__getNonce();
    }

    nonce64 Security::getNonce() {
        return security._getNonce();
    }

    // name warns us this might be a little slow (see code above)
    unsigned goodRandomNumberSlow() { return (unsigned) Security::getNonce(); }

} // namespace mongo
