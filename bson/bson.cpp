/* bson.cpp
*/

#include "util/builder.h"

namespace mongo { 

    /* there is an inline-only subset of the bson library; however, it is best if grow_reallocate function is not inlined to 
       improve performance.  Thus grow_reallocate is here.  The idea is that this file is a very MINIMAL set of code 
       for use when using the C++ BSON library and that it does not pull in a lot of other code prerequisites.

       bsondemo.cpp will compile and link with itself and this file only (no libs) -- that's the idea.

       jsobj.cpp currently #include's this file, so don't include both bson.cpp and jsobj.cpp in your project 
       at the same time -- just use jsobj.cpp if you need all that...

       This is interim and will evolve, but gets bsondemo.cpp compiling again sans libraries.
       */

    /* BufBuilder --------------------------------------------------------*/

    void BufBuilder::grow_reallocate() {
        int a = size * 2;
        if ( a == 0 )
            a = 512;
        if ( l > a )
            a = l + 16 * 1024;
        if( a > 64 * 1024 * 1024 )
            msgasserted(10000, "BufBuilder grow() > 64MB");
        data = (char *) realloc(data, a);
        size= a;
    }

}
