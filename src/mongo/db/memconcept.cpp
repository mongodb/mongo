//#if defined(_DEBUG)
#if 0

#include <string>
#include <map>
#include "memconcept.h"

using namespace std;

#include "../util/assert_util.h"

namespace mongo {
    namespace memconcept { 

        struct C { 
            C() : c(err) { }
            C(concept x) : c(x) { }
            C(concept x, string y) : c(x),desc(y) { }
            concept c;
            string desc;
        };

        typedef map<void*,C> M;

        M &ranges( * new M() );

        void is(void *p, concept c, string description, unsigned len) {
            ranges[p] = C(c,description);
        }

        void invalidate(void *p, unsigned len) {
            invalidate(p);
        }

        void invalidate(void *p) {
            if( ranges.count(p) ) { 
            }
            ranges.erase(p);
        }

    }
}

#endif
