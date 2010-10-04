// @file dur.cpp durability in the storage engine (crash-safeness / journaling)

#include "pch.h"

#if !defined(_DURABLE)

        // _DURABLE flag turns on durability in storage module.  if off you can ignore rest of this file

#else

#include "dur.h"
#include "../util/mongoutils/hash.h"

namespace mongo { 

    void dbunlocking_write() {
        // pending ...
    }

    namespace dur { 

        struct WriteIntent { 
            WriteIntent() : p(0) { }
            WriteIntent(void *a, unsigned b) : p(a), len(b) { }
            void *p;
            unsigned len;
        };

        /* try to remember things we have already marked for journalling.  false negatives are ok if infrequent - 
           we will just log them twice.
           */
        template<int Prime>
        class Already {
            enum { N = Prime }; // this should be small the idea is that it fits in the cpu cache easily
            WriteIntent nodes[N];
        public:
            Already() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
            bool checkAndSet(const WriteIntent& w) {
                mongoutils::hash(123);
                unsigned x = mongoutils::hashPointer(w.p);
                WriteIntent& n = nodes[x % N];
                if( n.p != w.p || n.len < w.len ) {
                    n = w;
                    return false;
                }
                return true; // already done
            }
        };

        static Already<127> alreadyNoted;
        static vector<WriteIntent> writes;

        void* writingPtr(void *x, size_t len) { 
            //log() << "TEMP writing " << x << ' ' << len << endl;
            WriteIntent w(x, len);
            if( !alreadyNoted.checkAndSet(w) ) {
                // remember, we will journal it in a bit
                writes.push_back(w);
                wassert( writes.size() <  2000000 );
                assert(  writes.size() < 20000000 );
            }
            DEV return MongoMMF::switchToPrivateView(x);
            return x;
        }

    } // namespace dur

} // namespace mongo

#endif
