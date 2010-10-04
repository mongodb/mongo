// @file dur.cpp

#include "pch.h"
#include "dur.h"
#include "../util/mmap.h"
#include "../util/mongoutils/hash.h"
#include "mongommf.h"

namespace mongo { 

    namespace dur { 

#if defined(_DURABLE)

        struct Where { 
            Where() : p(0) { }
            Where(void *a, unsigned b) : p(a), len(b) { }
            void *p;
            unsigned len;
        };

        /* try to remember things we have already marked for journalling.  false negatives are ok if infrequent - 
           we will just log them twice.
           */
        static class Already {
            enum { N = 127 }; // this should be small the idea is that it fits in the cpu cache easily
            Where nodes[N];
        public:
            Already() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
            bool checkAndSet(const Where& w) {
                unsigned x = mongoutils::hashAPointer(w.p);
                Where& n = nodes[x % N];
                if( n.p != w.p || n.len < w.len ) {
                    n = w;
                    return false;
                }
                return true; // already done
            }
        } alreadyNoted;

        static vector<Where> writes;

        void* writingPtr(void *x, size_t len) { 
            Where w(x, len);
            if( !alreadyNoted.checkAndSet(w) ) {
                // remember this intent to write, we will journal it before long
                writes.push_back(w);
                assert( writes.size() < 20000000 );
            }
#if defined(_DEBUG)
            //cout << "TEMP writing " << x << ' ' << len << endl;
            return MongoMMF::switchToPrivateView(x);
#else
            return x;
#endif
        }

        void assertReading(void *p) { 
#if defined(_DEBUG)
            assert( MongoMMF::switchToPrivateView(p) != p );
#endif
        }

#endif

    }

}
