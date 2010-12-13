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
         for now we are in read lock which is not ideal.
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
#include "cmdline.h"
#include "client.h"
#include "dur.h"
#include "dur_journal.h"
#include "dur_commitjob.h"
#include "../util/mongoutils/hash.h"
#include "../util/mongoutils/str.h"
#include "../util/timer.h"

using namespace mongoutils;

namespace mongo { 
    DurableInterface* DurableInterface::_impl = new NonDurableImpl();

#if !defined(_DURABLE)
    void enableDurability() {}
#else

    namespace dur { 
        void groupCommit();
    }

    namespace { 
        using namespace dur;

#if defined(_DEBUG)
        const bool DebugValidateMapsMatch = false;
        const bool DebugCheckLastDeclaredWrite = false;
#else
        const bool DebugValidateMapsMatch = false;
        const bool DebugCheckLastDeclaredWrite = false;
#endif

        CommitJob commitJob;
    }

    void enableDurability() { // TODO: merge with startup() ?
        assert(typeid(*DurableInterface::_impl) == typeid(NonDurableImpl));
        // lets NonDurableImpl instance leak, but its tiny and only happens once
        DurableInterface::_impl = new DurableImpl();
    }

    bool DurableImpl::awaitCommit() { 
        commitJob.awaitNextCommit();
        return true;
    }

        /** Declare that a file has been created 
            Normally writes are applied only after journalling, for safety.  But here the file 
            is created first, and the journal will just replay the creation if the create didn't 
            happen because of crashing.
        */
        void DurableImpl::createdFile(string filename, unsigned long long len) { 
            shared_ptr<DurOp> op( new FileCreatedOp(filename, len) );
            commitJob.noteOp(op);
        }

        /** indicate that a database is about to be dropped.  call before the actual drop. */
        void DurableImpl::droppingDb(string db) { 
            shared_ptr<DurOp> op( new DropDbOp(db) );
            commitJob.noteOp(op);

            // must commit now, before files are actually unlinked:
            dur::groupCommit();
        }

        /** declare write intent.  when already in the write view if testIntent is true. */
        void DurableImpl::declareWriteIntent(void *p, unsigned len) {
            WriteIntent w(p, len);
            commitJob.note(w);
        }

        void* DurableImpl::writingPtr(void *x, unsigned len) { 
            void *p = x;
            if( testIntent )
                p = MongoMMF::switchToPrivateView(x);
            declareWriteIntent(p, len);
            return p;
        }

        /** declare intent to write
            @param ofs offset within buf at which we will write
            @param len the length at ofs we will write
            @return new buffer pointer.  this is modified when testIntent is true.
        */
        void* DurableImpl::writingAtOffset(void *buf, unsigned ofs, unsigned len) {
            char *p = (char *) buf;
            if( testIntent )
                p = (char *) MongoMMF::switchToPrivateView(buf);
            declareWriteIntent(p+ofs, len);
            return p;
        }

        /** Used in _DEBUG builds to check that we didn't overwrite the last intent
            that was declared.  called just before writelock release.  we check a few
            bytes after the declared region to see if they changed.

            @see MongoMutex::_releasedWriteLock

            SLOW
        */
#if defined(_DEBUG)
        void DurableImpl::debugCheckLastDeclaredWrite() { 
            if( !DebugCheckLastDeclaredWrite )
                return;

            if( testIntent )
                return;

            static int n;
            ++n;

            assert(debug && cmdLine.dur);
            vector<WriteIntent>& w = commitJob.writes();
            if( w.size() == 0 ) 
                return;
            const WriteIntent &i = w[w.size()-1];
            size_t ofs;
            MongoMMF *mmf = privateViews.find(i.p, ofs);
            if( mmf == 0 ) 
                return;
            size_t past = ofs + i.len;
            if( mmf->length() < past + 8 ) 
                return; // too close to end of view
            char *priv = (char *) mmf->getView();
            char *writ = (char *) mmf->view_write();
            unsigned long long *a = (unsigned long long *) (priv+past);
            unsigned long long *b = (unsigned long long *) (writ+past);
            if( *a != *b ) { 
                for( unsigned z = 0; z < w.size() - 1; z++ ) { 
                    const WriteIntent& wi = w[z];
                    char *r1 = (char*) wi.p;
                    char *r2 = r1 + wi.len;
                    if( r1 <= (((char*)a)+8) && r2 > (char*)a ) { 
                        //log() << "it's ok " << wi.p << ' ' << wi.len << endl;
                        return;
                    }
                }
                log() << "dur data after write area " << i.p << " does not agree" << endl;
                log() << " was:  " << ((void*)b) << "  " << hexdump((char*)b, 8) << endl;
                log() << " now:  " << ((void*)a) << "  " << hexdump((char*)a, 8) << endl;
                log() << " n:    " << n << endl;
                log() << endl;
            }
        }
#endif

    namespace dur {
        /** we will build an output buffer ourself and then use O_DIRECT
            we could be in read lock for this
            caller handles locking 
            */
        static void PREPLOGBUFFER() { 
            assert( cmdLine.dur );
            AlignedBuilder& bb = commitJob._ab;
            bb.reset();

            unsigned lenOfs;
            // JSectHeader
            {
                bb.appendStr("\nHH\n", false);
                lenOfs = bb.skip(4);
            }

            // ops other than basic writes
            {
                for( vector< shared_ptr<DurOp> >::iterator i = commitJob.ops().begin(); i != commitJob.ops().end(); ++i ) { 
                    (*i)->serialize(bb);
                }
            }

            // write intents
            {
                scoped_lock lk(privateViews._mutex());
                string lastFilePath;
                for( vector<WriteIntent>::iterator i = commitJob.writes().begin(); i != commitJob.writes().end(); i++ ) {
                    size_t ofs;
                    MongoMMF *mmf = privateViews._find(i->p, ofs);
                    if( mmf == 0 ) {
                        string s = str::stream() << "view pointer cannot be resolved " << (size_t) i->p;
                        journalingFailure(s.c_str()); // asserts
                        return;
                    }

                    if( !mmf->willNeedRemap() ) {
                        mmf->willNeedRemap() = true; // usually it will already be dirty so don't bother writing then
                    }
                    //size_t ofs = ((char *)i->p) - ((char*)mmf->getView().p);
                    i->w_ptr = ((char*)mmf->view_write()) + ofs;
                    if( mmf->filePath() != lastFilePath ) { 
                        lastFilePath = mmf->filePath();
                        JDbContext c;
                        bb.appendStruct(c);
                        bb.appendStr(lastFilePath);
                    }
                    JEntry e;
                    e.len = i->len;
                    assert( ofs <= 0x80000000 );
                    e.ofs = (unsigned) ofs;
                    e.fileNo = mmf->fileSuffixNo();
                    bb.appendStruct(e);
                    bb.appendBuf(i->p, i->len);
                }
            }

            {
                JSectFooter f(bb.buf(), bb.len());
                bb.appendStruct(f);
            }

            {
                assert( 0xffffe000 == (~(Alignment-1)) );
                unsigned L = (bb.len() + Alignment-1) & (~(Alignment-1)); // fill to alignment
                dassert( L >= (unsigned) bb.len() );
                *((unsigned*)bb.atOfs(lenOfs)) = L;
                unsigned padding = L - bb.len();
                bb.skip(padding);
                dassert( bb.len() % Alignment == 0 );
            }

            return;
        }

        /** write the buffer we have built to the journal and fsync it.
            outside of lock as that could be slow.
        */
        static void WRITETOJOURNAL(AlignedBuilder& ab) { 
            journal(ab);
        }

        /** (SLOW) diagnostic to check that the private view and the non-private view are in sync.
        */
        static void debugValidateMapsMatch() {
            if( !DebugValidateMapsMatch ) 
                 return;

            Timer t;
            set<MongoFile*>& files = MongoFile::getAllFiles();
            for( set<MongoFile*>::iterator i = files.begin(); i != files.end(); i++ ) { 
                MongoFile *mf = *i;
                if( mf->isMongoMMF() ) { 
                    MongoMMF *mmf = (MongoMMF*) mf;
                    const char *p = (const char *) mmf->getView();
                    const char *w = (const char *) mmf->view_write();
                    unsigned low = 0xffffffff;
                    unsigned high = 0;
                    for( unsigned i = 0; i < mmf->length(); i++ ) {
                        if( p[i] != w[i] ) { 
                            log() << i << '\t' << (int) p[i] << '\t' << (int) w[i] << endl;
                            if( i < low ) low = i;
                            if( i > high ) high = i;
                        }
                    }
                    if( low != 0xffffffff ) { 
                        std::stringstream ss;
                        ss << "dur error warning views mismatch " << mmf->filename() << ' ' << (hex) << low << ".." << high << " len:" << high-low+1;
                        log() << ss.str() << endl;
                        log() << "priv loc: " << (void*)(p+low) << endl;
                        vector<WriteIntent>& w = commitJob.writes();
                        (void)w; // mark as unused. Useful for inspection in debugger

                        breakpoint();
                    }
                }
            }
            log() << "debugValidateMapsMatch " << t.millis() << "ms " << endl;
        }

        /** apply the writes back to the non-private MMF after they are for certain in redo log 

            (1) todo we don't need to write back everything every group commit.  we MUST write back
            that which is going to be a remapped on its private view - but that might not be all 
            views.

            (2) todo should we do this using N threads?  would be quite easy
                see Hackenberg paper table 5 and 6.  2 threads might be a good balance.

            locking: in read lock when called
        */
        static void WRITETODATAFILES() { 
            /* we go backwards as what is at the end is most likely in the cpu cache.  it won't be much, but we'll take it. */
            for( int i = commitJob.writes().size() - 1; i >= 0; i-- ) {
                const WriteIntent& intent = commitJob.writes()[i];
                char *dst = (char *) (intent.w_ptr);
                memcpy(dst, intent.p, intent.len);
            }

            debugValidateMapsMatch();
        }

        /** We need to remap the private views periodically. otherwise they would become very large.
            Call within write lock.
        */
        void REMAPPRIVATEVIEW() { 
            static unsigned startAt;
            static unsigned long long lastRemap;

            dbMutex.assertWriteLocked();
            dbMutex._remapPrivateViewRequested = false;
            assert( !commitJob.hasWritten() );

            if( 0 ) { 
                log() << "TEMP remapprivateview disabled for testing - will eventually run oom in this mode if db bigger than ram" << endl;
                return;
            }

            // we want to remap all private views about every 2 seconds.  there could be ~1000 views so 
            // we do a little each pass; beyond the remap time, more significantly, there will be copy on write 
            // faults after remapping, so doing a little bit at a time will avoid big load spikes on 
            // remapping.
            unsigned long long now = curTimeMicros64();
            double fraction = (now-lastRemap)/20000000.0;

            set<MongoFile*>& files = MongoFile::getAllFiles();
            unsigned sz = files.size();
            if( sz == 0 ) 
                return;

            unsigned ntodo = (unsigned) (sz * fraction);
            if( ntodo < 1 ) ntodo = 1;
            if( ntodo > sz ) ntodo = sz;

            const set<MongoFile*>::iterator b = files.begin();
            const set<MongoFile*>::iterator e = files.end();
            set<MongoFile*>::iterator i = b;
            // skip to our starting position
            for( unsigned x = 0; x < startAt; x++ ) {
                i++;
                if( i == e ) i = b;
            }
            startAt = (startAt + ntodo) % sz; // mark where to start next time

            for( unsigned x = 0; x < ntodo; x++ ) {
                dassert( i != e );
                if( (*i)->isMongoMMF() ) {
                    MongoMMF *mmf = (MongoMMF*) *i;
                    assert(mmf);
                    if( mmf->willNeedRemap() ) {
                        mmf->willNeedRemap() = false;
                        mmf->remapThePrivateView();
                    }
                    i++;
                    if( i == e ) i = b;
                }
            }
        }

        /** locking in read lock when called 
            @see MongoMMF::close()
        */
        void groupCommit() {
            dbMutex.assertAtLeastReadLocked();

            if( !commitJob.hasWritten() )
                return;

            PREPLOGBUFFER();

            WRITETOJOURNAL(commitJob._ab);

            // data is now in the journal, which is sufficient for acknowledging getlasterror. 
            // (ok to crash after that)
            log() << "TEMP NOTIFYING COMMITTED" << endl;
            commitJob.notifyCommitted();

            // write the noted write intent entries to the data files.
            // this has to come after writing to the journal, obviously...
            MongoFile::markAllWritable(); // for _DEBUG. normally we don't write in a read lock
            WRITETODATAFILES();
            if (!dbMutex.isWriteLocked())
                MongoFile::unmarkAllWritable();

            commitJob.reset();

            // REMAPPRIVATEVIEW
            // 
            // remapping private views must occur after WRITETODATAFILES otherwise 
            // we wouldn't see newly written data on reads.
            // 
            DEV assert( !commitJob.hasWritten() );
            if( !dbMutex.isWriteLocked() ) { 
                // this needs done in a write lock thus we do it on the next acquisition of that 
                // instead of here (there is no rush if you aren't writing anyway -- but it must happen, 
                // if it is done, before any uncommitted writes occur).
                //
                dbMutex._remapPrivateViewRequested = true;
            }
            else { 
                // however, if we are already write locked, we must do it now -- up the call tree someone 
                // may do a write without a new lock acquisition.  this can happen when MongoMMF::close() calls
                // this method when a file (and its views) is about to go away.
                //
                REMAPPRIVATEVIEW();
            }
        }

        static void go() {
            if( !commitJob.hasWritten() )
                return;

            {
                readlocktry lk("", 1000);
                if( lk.got() ) {
                    groupCommit();
                    return;
                }
            }

            // starvation on read locks could occur.  so if read lock acquisition is slow, try to get a 
            // write lock instead.  otherwise writes could use too much RAM.
            writelock lk;
            groupCommit();
        }

        /** called when a MongoMMF is closing -- we need to go ahead and group commit in that case before its 
            views disappear 
        */
        void closingFileNotification() {
            if( dbMutex.atLeastReadLocked() ) {
                groupCommit(); 
            }
            else {
                assert( inShutdown() );
                if( commitJob.hasWritten() ) { 
                    log() << "dur warning files are closing outside locks with writes pending" << endl;
                }
            }
        }

        static void durThread() { 
            Client::initThread("dur");
            const int HowOftenToGroupCommitMs = 100;
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
                    go();
                }
                catch(std::exception& e) { 
                    log() << "exception in durThread " << e.what() << endl;
                }
            }
        }

        void unlinkThread();
        void recover();

        void _debugCheckLastDeclaredWrite() { return getDur().debugCheckLastDeclaredWrite(); }

    } // namespace dur

    void DurableImpl::startup() {
        if( !cmdLine.dur )
            return;
        if( testIntent )
            return;
        recover();
        journalMakeDir();
        boost::thread t(durThread);
        boost::thread t2(unlinkThread);
    }

#endif

} // namespace mongo

