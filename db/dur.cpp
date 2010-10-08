// @file dur.cpp durability in the storage engine (crash-safeness / journaling)

/* 
   phases

     PREPLOGBUFFER 
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?  will be faster.
     WRITETOREDOLOG 
       we could be unlocked (the main db lock that is...) for this, with sufficient care, but there is some complexity
         have to handle falling behind which would use too much ram (going back into a read lock would suffice to stop that).
         downgrading to (a perhaps upgradable) read lock would be a good start
     CHECKPOINT
       apply the writes back to the non-private MMF after they are for certain in redo log
     REMAPPRIVATEVIEW
       we could in a write lock quickly flip readers back to the main view, then stay in read lock and do our real 
         remapping. with many files (e.g., 1000), remapping could be time consuming (several ms), so we don't want 
         to be too frequent.  tracking time for this step would be wise.
       there could be a slow down immediately after remapping as fresh copy-on-writes for commonly written pages will 
         be required.  so doing these remaps more incrementally in the future might make sense - but have to be careful
         not to introduce bugs.
*/

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
