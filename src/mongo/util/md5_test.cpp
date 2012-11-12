// md5_test.cpp

#include "mongo/unittest/unittest.h"

extern int do_md5_test(void);

namespace mongo {
    TEST( MD5, BuiltIn1 ) {
        ASSERT_TRUE( do_md5_test() == 0 );
    }
}
