
// security.cpp

#include "stdafx.h"
#include "security.h"
#include "../util/md5.hpp"
#include "json.h" 
#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"

namespace mongo {

    extern "C" int do_md5_test(void);

    boost::thread_specific_ptr<AuthenticationInfo> authInfo;

    bool noauth = true;

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
        
        if ( do_md5_test() )
            massert("md5 unit test fails", false);
    }

    nonce Security::getNonce(){
        nonce n;
#if defined(__linux__)
        devrandom->read((char*)&n, sizeof(n));
        massert("devrandom failed", !devrandom->fail());
#elif defined(_WIN32)
        n = ((unsigned long long)rand())<<32 | rand();
#else
        n = random();
#endif
        return n;
    }

    Security security;

} // namespace mongo

