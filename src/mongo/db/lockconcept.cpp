// @file lockconcepts.cpp

#include <string>
#include <map>
#include "lockconcept.h"
//#include "../util/log.h"

using namespace std;

#include "../util/assert_util.h"

namespace mongo {
    namespace lockconcept { 

        struct C { 
            C() : c(err) { }
            C(concept x) : c(x) { }
            C(concept x, string y) : c(x),desc(y) { }
            concept c;
            string desc;
        };

        typedef map<void*,C> M;

        M &ranges( * new M() );

        void is(void *p, concept c, string description) {
            ranges[p] = C(c,description);
        }

        void is(void *p, concept c) {
            ranges[p] = C(c);
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
