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
     WRITETOJOURNAL
       we could be unlocked (the main db lock that is...) for this, with sufficient care, but there is some complexity
         have to handle falling behind which would use too much ram (going back into a read lock would suffice to stop that).
         downgrading to (a perhaps upgradable) read lock would be a good start
     WRITETODATAFILES
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

#include "client.h"
#include "dur.h"
#include "dur_journal.h"
#include "../util/mongoutils/hash.h"
#include "../util/timer.h"
#include "../util/alignedbuilder.h"

namespace mongo { 

    void dbunlocking_write() {
        // pending ...
    }

    namespace dur { 

        //MongoMMF* pointerToMMF(void *p, size_t& ofs);

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
            Already() { clear(); }
            void clear() { memset(this, 0, sizeof(*this)); }

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
            void *p = x;
            DEV p = MongoMMF::switchToPrivateView(x);
            WriteIntent w(p, len);
            if( !alreadyNoted.checkAndSet(w) ) {
                // remember intent. we will journal it in a bit
                writes.push_back(w);
                wassert( writes.size() <  2000000 );
                assert(  writes.size() < 20000000 );
            }
            return p;
        }

        /** caller handles locking */
        static bool PREPLOGBUFFER(AlignedBuilder& bb) { 
            if( writes.empty() )
                return false;

            bb.reset();

            unsigned *lenInBlockHeader;
            {
                // JSectHeader
                bb.appendStr("\nHH\n", false);
                lenInBlockHeader = (unsigned *) bb.skip(4);
            }

            string lastFilePath;

            {
                scoped_lock lk(privateViews._mutex());
                for( vector<WriteIntent>::iterator i = writes.begin(); i != writes.end(); i++ ) {
                    size_t ofs;
                    MongoMMF *mmf = privateViews._find(i->p, ofs);
                    if( mmf == 0 ) {
                        journalingFailure("view pointer cannot be resolved");
                    }
                    else {
                        if( mmf->filePath() != lastFilePath ) { 
                            lastFilePath = mmf->filePath();
                            JDbContext c;
                            bb.appendStruct(c);
                            bb.appendStr(lastFilePath);
                        }
                        JEntry e;
                        e.len = i->len;
                        e.fileNo = mmf->fileSuffixNo();
                        bb.appendStruct(e);
                        bb.appendBuf(i->p, i->len);
                    }
                }
            }

            {
                JSectFooter f;
                f.hash = 0;
                bb.appendStruct(f);
            }

            {
                unsigned L = (bb.len() + 8191) & 0xffffe000; // fill to alignment
                dassert( L >= (unsigned) bb.len() );
                *lenInBlockHeader = L;
                unsigned padding = L - bb.len();
                bb.skip(padding);
                dassert( bb.len() % 8192 == 0 );
            }

            writes.clear();
            alreadyNoted.clear();
            return true;
        }

        static void WRITETOJOURNAL(const AlignedBuilder& bb) { 
            journal(bb);
        }

        static void _go(AlignedBuilder& bb) {
            PREPLOGBUFFER(bb);

            // todo: add double buffering so we can be (not even read locked) during WRITETOJOURNAL
            WRITETOJOURNAL(bb);
        }

        static void go(AlignedBuilder& bb) {
            {
                readlocktry lk("", 1000);
                if( lk.got() ) {
                    _go(bb);
                    return;
                }
            }
            // starvation on read locks could occur.  so if read lock acquisition is slow, try to get a 
            // write lock instead.  otherwise writes could use too much RAM.
            writelock lk;
            _go(bb);
        }

        static void durThread() { 
            Client::initThread("dur");
            const int HowOftenToGroupCommitMs = 100;
            AlignedBuilder bb(1024 * 1024 * 16); // reuse to avoid any heap fragmentation
            while( 1 ) { 
                try {
                    int millis = HowOftenToGroupCommitMs;
                    {
                        Timer t;
                        journalRotate(); // note we do this part outside of mongomutex
                        millis -= t.millis();
                        if( millis < 5 || millis > HowOftenToGroupCommitMs )
                            millis = 5;
                    }
                    sleepmillis(millis);
                    go(bb);
                }
                catch(std::exception& e) { 
                    log() << "exception in durThread " << e.what() << endl;
                }
            }
        }

        void unlinkThread();

        void startup() {
            journalMakeDir();
            boost::thread t(durThread);
            boost::thread t2(unlinkThread);
        }

    } // namespace dur

} // namespace mongo

#endif
