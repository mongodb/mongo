// security.cpp

#include "stdafx.h"
#include "security.h"
#include "../util/md5.hpp"

namespace mongo {

    extern "C" int do_md5_test(void);

    boost::thread_specific_ptr<AuthenticationInfo> authInfo;

    typedef unsigned long long nonce;

    struct Security {
        ifstream *devrandom;

        nonce getNonce() {
            nonce n;
#if defined(__linux__)
            devrandom->read((char*)&n, sizeof(n));
            massert("devrandom failed", !devrandom->fail());
#else
            n = ((unsigned long long)rand())<<32 | rand();
#endif
            return n;
        }

        Security()
        {
#if defined(__linux__)
            devrandom = new ifstream("/dev/urandom", ios::binary|ios::in);
            massert( "can't open dev/urandom", devrandom->is_open() );
#endif
            assert( sizeof(nonce) == 8 );

            if ( do_md5_test() )
                massert("md5 unit test fails", false);
        }
    } security;

} // namespace mongo

#include "commands.h"
#include "jsobj.h"

namespace mongo {

    class CmdGetNonce : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdGetNonce() : Command("getnonce") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.append("nonce", (double) security.getNonce());
            return true;
        }
    } cmdGetNonce;

    class CmdAuthenticate : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdAuthenticate() : Command("authenticate") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            return false;
        }
    } cmdAuthenticate;


} // namespace mongo
