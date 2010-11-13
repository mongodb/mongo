// @file dur.cpp durability in the storage engine (crash-safeness / journaling)

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

/* 
   phases

     PREPLOGBUFFER 
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
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

#if defined(_DURABLE)

#include "dur.h"
#include "dur_journal.h"
#include "../util/mongoutils/hash.h"

namespace mongo { 

    void dbunlocking_write() {
        // pending ...
    }

    namespace dur { 

        MongoMMF* pointerToMMF(void *p, size_t& ofs);

        struct WriteIntent { 
            WriteIntent() : p(0) { }
            WriteIntent(void *a, unsigned b) : p(a), len(b) { }
            void *p; // where we will write
            unsigned len; // up to this len
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

            /* see if we have Already recorded/indicated our write intent for this region of memory.
               @return true if already indicated.
            */
            bool checkAndSet(const WriteIntent& w) {
                unsigned x = mongoutils::hashPointer(w.p);
                WriteIntent& nd = nodes[x % N];
                if( nd.p != w.p || nd.len < w.len ) {
                    nd = w;
                    return false;
                }
                return true;
            }
        };

        static Already<127> alreadyNoted;

        /* our record of pending/uncommitted write intents */
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

        void journalingFailure(const char *msg) { 
            /** todo:
                (1) don't log too much
                (2) make an indicator in the journal dir that something bad happened. 
                (2b) refuse to do a recovery startup if that is there without manual override.
            */ 
            log() << "journaling error " << msg << endl;
        }

        void _PREPLOGBUFFER(BufBuilder& bb) { 
            bb.reset();

            JSectHeader h;
            bb.appendStruct(h);

            for( vector<WriteIntent>::iterator i = writes.begin(); i != writes.end(); i++ ) {
                JEntry e;
                e.len = i->len;
                size_t ofs;
                MongoMMF *mmf = pointerToMMF(i->p, ofs);
                if( mmf == 0 ) {
                    journalingFailure("view pointer cannot be resolved");
                }
                else {
                }
            }

            JSectFooter f;
            bb.appendStruct(f);
        }

        void PREPLOGBUFFER(BufBuilder& bb) {
            {
                readlocktry lk("", 1000);
                if( lk.got() ) {
                    _PREPLOGBUFFER(bb);
                    return;
                }
            }
            // starvation on read locks could occur.  so if read lock acquisition is slow, try to get a 
            // write lock instead.  otherwise writes could use too much RAM.
            writelock lk;
            _PREPLOGBUFFER(bb);
        }

        void durThread() { 
            BufBuilder bb(1024 * 1024 * 16);
            while( 1 ) { 
                try {
                    sleepmillis(100);
                    PREPLOGBUFFER(bb);
                }
                catch(...) { 
                    log() << "exception in durThread" << endl;
                }
            }
        }

        void startup() {
            boost::thread t(durThread);
        }

    } // namespace dur

} // namespace mongo

#endif
