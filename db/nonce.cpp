// nonce.cpp

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "stdafx.h"
#include "nonce.h"

namespace mongo {

    extern "C" int do_md5_test(void);
    
    Security::Security(){
#if defined(__linux__)
        devrandom = new ifstream("/dev/urandom", ios::binary|ios::in);
        massert( "can't open dev/urandom", devrandom->is_open() );
#elif defined(_WIN32)
        srand(curTimeMicros());
#else
        srandomdev();
#endif
        assert( sizeof(nonce) == 8 );
        
#ifndef NDEBUG
        if ( do_md5_test() )
	    massert("md5 unit test fails", false);
#endif
    }
    
    nonce Security::getNonce(){
        nonce n;
#if defined(__linux__)
        devrandom->read((char*)&n, sizeof(n));
        massert("devrandom failed", !devrandom->fail());
#elif defined(_WIN32)
        n = (((unsigned long long)rand())<<32) | rand();
#else
        n = (((unsigned long long)random())<<32) | random();
#endif
        return n;
    }
    
    Security security;
        
} // namespace mongo
