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

extern int do_md5_test(void);

namespace mongo {
    
	Security::Security() {
		static int n;
		massert( 10352 , "Security is a singleton class", ++n == 1);
		init(); 
	}

    void Security::init(){
		if( _initialized ) return;
		_initialized = true;

#if defined(__linux__) || defined(__sunos__)
        _devrandom = new ifstream("/dev/urandom", ios::binary|ios::in);
        massert( 10353 ,  "can't open dev/urandom", _devrandom->is_open() );
#elif defined(_WIN32)
        srand(curTimeMicros());
#else
        srandomdev();
#endif
        assert( sizeof(nonce) == 8 );
        
#ifndef NDEBUG
        if ( do_md5_test() )
	    massert( 10354 , "md5 unit test fails", false);
#endif
    }
    
    nonce Security::getNonce(){
        static mongo::mutex m("getNonce");
        scoped_lock lk(m);

		/* question/todo: /dev/random works on OS X.  is it better 
		   to use that than random() / srandom()?
		*/

        nonce n;
#if defined(__linux__) || defined(__sunos__)
        _devrandom->read((char*)&n, sizeof(n));
        massert( 10355 , "devrandom failed", !_devrandom->fail());
#elif defined(_WIN32)
        n = (((unsigned long long)rand())<<32) | rand();
#else
        n = (((unsigned long long)random())<<32) | random();
#endif
        return n;
    }
    unsigned getRandomNumber() { return (unsigned) security.getNonce(); }
    
	bool Security::_initialized;
    Security security;
        
} // namespace mongo
