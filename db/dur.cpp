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
         for now (1.7.5/1.8.0) we are in read lock which is not ideal.
     WRITETODATAFILES
       apply the writes back to the non-private MMF after they are for certain in redo log
     REMAPPRIVATEVIEW
       we could in a write lock quickly flip readers back to the main view, then stay in read lock and do our real
         remapping. with many files (e.g., 1000), remapping could be time consuming (several ms), so we don't want
         to be too frequent.
       there could be a slow down immediately after remapping as fresh copy-on-writes for commonly written pages will
         be required.  so doing these remaps fractionally is helpful. 

     @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#include "pch.h"
#include "cmdline.h"
#include "client.h"
#include "dur.h"
#include "dur_journal.h"
#include "dur_commitjob.h"
#include "dur_recover.h"
#include "../util/concurrency/race.h"
#include "../util/mongoutils/hash.h"
#include "../util/mongoutils/str.h"
#include "../util/timer.h"
#include "dur_stats.h"

using namespace mongoutils;

namespace mongo {

    namespace dur {

        void WRITETODATAFILES();
        void PREPLOGBUFFER();

        /** declared later in this file
            only used in this file -- use DurableInterface::commitNow() outside
        */
        static void groupCommit();

        CommitJob commitJob;

        Stats stats;

        void Stats::S::reset() {
            memset(this, 0, sizeof(*this));
        }

        Stats::Stats() {
            _a.reset();
            _b.reset();
            curr = &_a;
            _intervalMicros = 3000000;
        }

        Stats::S * Stats::other() {
            return curr == &_a ? &_b : &_a;
        }
                        string _CSVHeader();

        string Stats::S::_CSVHeader() { 
            return "commits\tjournaledMB\twriteToDataFilesMB\tcommitsInWriteLock\tearlyCommits\tprepLogBuffer\twriteToJournal\twriteToDataFiles\tremapPrivateView";
        }

        string Stats::S::_asCSV() { 
            stringstream ss;
            ss << 
                _commits << '\t' << 
                _journaledBytes / 1000000.0 << '\t' << 
                _writeToDataFilesBytes / 1000000.0 << '\t' << 
                _commitsInWriteLock << '\t' << 
                _earlyCommits <<  '\t' << 
                (unsigned) (_prepLogBufferMicros/1000) << '\t' << 
                (unsigned) (_writeToJournalMicros/1000) << '\t' << 
                (unsigned) (_writeToDataFilesMicros/1000) << '\t' << 
                (unsigned) (_remapPrivateViewMicros/1000);
            return ss.str();
        }

        BSONObj Stats::S::_asObj() {
            return BSON(
                       "commits" << _commits <<
                       "journaledMB" << _journaledBytes / 1000000.0 <<
                       "writeToDataFilesMB" << _writeToDataFilesBytes / 1000000.0 <<
                       "commitsInWriteLock" << _commitsInWriteLock <<
                       "earlyCommits" << _earlyCommits << 
                       "timeMs" <<
                       BSON( "dt" << _dtMillis <<
                             "prepLogBuffer" << (unsigned) (_prepLogBufferMicros/1000) <<
                             "writeToJournal" << (unsigned) (_writeToJournalMicros/1000) <<
                             "writeToDataFiles" << (unsigned) (_writeToDataFilesMicros/1000) <<
                             "remapPrivateView" << (unsigned) (_remapPrivateViewMicros/1000)
                           )
                   );
        }

        BSONObj Stats::asObj() {
            return other()->_asObj();
        }

        void Stats::rotate() {
            unsigned long long now = curTimeMicros64();
            unsigned long long dt = now - _lastRotate;
            if( dt >= _intervalMicros && _intervalMicros ) {
                // rotate
                curr->_dtMillis = (unsigned) (dt/1000);
                _lastRotate = now;
                curr = other();
                curr->reset();
            }
        }

        void NonDurableImpl::setNoJournal(void *dst, void *src, unsigned len) {
            memcpy(dst, src, len);
        }

        void DurableImpl::setNoJournal(void *dst, void *src, unsigned len) {
            MemoryMappedFile::makeWritable(dst, len);

            // we stay in this mutex for everything to work with DurParanoid/validateSingleMapMatches
            //
            // this also makes setNoJournal threadsafe, which is good as we call it from a read (not a write) lock 
            // in class SlaveTracking
            //
            scoped_lock lk( privateViews._mutex() );
            size_t ofs;
            MongoMMF *f = privateViews.find_inlock(dst, ofs);
            assert(f);
            void *w = (((char *)f->view_write())+ofs);
            // first write it to the writable (file) view
            memcpy(w, src, len);
            if( memcmp(w, dst, len) ) {
                // if we get here, a copy-on-write had previously occurred. so write it to the private view too
                // to keep them in sync.  we do this as we do not want to cause a copy on write unnecessarily.
                memcpy(dst, src, len);
            }
        }

        /** base declare write intent function that all the helpers call. */
        void DurableImpl::declareWriteIntent(void *p, unsigned len) {
            commitJob.note(p, len);
        }

        static DurableImpl* durableImpl = new DurableImpl();
        static NonDurableImpl* nonDurableImpl = new NonDurableImpl();
        DurableInterface* DurableInterface::_impl = nonDurableImpl;

        void DurableInterface::enableDurability() {
            assert(_impl == nonDurableImpl);
            _impl = durableImpl;
        }

        void DurableInterface::disableDurability() {
            assert(_impl == durableImpl);
            massert(13616, "can't disable durability with pending writes", !commitJob.hasWritten());
            _impl = nonDurableImpl;
        }

        bool DurableImpl::commitNow() {
            stats.curr->_earlyCommits++;
            groupCommit();
            return true;
        }

        bool DurableImpl::awaitCommit() {
            commitJob.awaitNextCommit();
            return true;
        }

        /** Declare that a file has been created
            Normally writes are applied only after journaling, for safety.  But here the file
            is created first, and the journal will just replay the creation if the create didn't
            happen because of crashing.
        */
        void DurableImpl::createdFile(string filename, unsigned long long len) {
            shared_ptr<DurOp> op( new FileCreatedOp(filename, len) );
            commitJob.noteOp(op);
        }

        void* DurableImpl::writingPtr(void *x, unsigned len) {
            void *p = x;
            declareWriteIntent(p, len);
            return p;
        }

        /** declare intent to write
            @param ofs offset within buf at which we will write
            @param len the length at ofs we will write
            @return new buffer pointer.
        */
        void* DurableImpl::writingAtOffset(void *buf, unsigned ofs, unsigned len) {
            char *p = (char *) buf;
            declareWriteIntent(p+ofs, len);
            return p;
        }

        void* DurableImpl::writingRangesAtOffsets(void *buf, const vector< pair< long long, unsigned > > &ranges ) {
            char *p = (char *) buf;
            for( vector< pair< long long, unsigned > >::const_iterator i = ranges.begin();
                    i != ranges.end(); ++i ) {
                declareWriteIntent( p + i->first, i->second );
            }
            return p;
        }

        bool DurableImpl::commitIfNeeded() {
            DEV commitJob._nSinceCommitIfNeededCall = 0;
            if (commitJob.bytes() > UncommittedBytesLimit) { // should this also fire if CmdLine::DurAlwaysCommit?
                stats.curr->_earlyCommits++;
                groupCommit();
                return true;
            }
            return false;
        }

        /** Used in _DEBUG builds to check that we didn't overwrite the last intent
            that was declared.  called just before writelock release.  we check a few
            bytes after the declared region to see if they changed.

            @see MongoMutex::_releasedWriteLock

            SLOW
        */
#if 0
        void DurableImpl::debugCheckLastDeclaredWrite() {
            static int n;
            ++n;

            assert(debug && cmdLine.dur);
            if (commitJob.writes().empty())
                return;
            const WriteIntent &i = commitJob.lastWrite();
            size_t ofs;
            MongoMMF *mmf = privateViews.find(i.start(), ofs);
            if( mmf == 0 )
                return;
            size_t past = ofs + i.length();
            if( mmf->length() < past + 8 )
                return; // too close to end of view
            char *priv = (char *) mmf->getView();
            char *writ = (char *) mmf->view_write();
            unsigned long long *a = (unsigned long long *) (priv+past);
            unsigned long long *b = (unsigned long long *) (writ+past);
            if( *a != *b ) {
                for( set<WriteIntent>::iterator it(commitJob.writes().begin()), end((commitJob.writes().begin())); it != end; ++it ) {
                    const WriteIntent& wi = *it;
                    char *r1 = (char*) wi.start();
                    char *r2 = (char*) wi.end();
                    if( r1 <= (((char*)a)+8) && r2 > (char*)a ) {
                        //log() << "it's ok " << wi.p << ' ' << wi.len << endl;
                        return;
                    }
                }
                log() << "dur data after write area " << i.start() << " does not agree" << endl;
                log() << " was:  " << ((void*)b) << "  " << hexdump((char*)b, 8) << endl;
                log() << " now:  " << ((void*)a) << "  " << hexdump((char*)a, 8) << endl;
                log() << " n:    " << n << endl;
                log() << endl;
            }
        }
#endif

        /** write the buffer we have built to the journal and fsync it.
            outside of lock as that could be slow.
        */
        static void WRITETOJOURNAL(AlignedBuilder& ab) {
            Timer t;
            journal(ab);
            stats.curr->_writeToJournalMicros += t.micros();
        }

        // Functor to be called over all MongoFiles

        class validateSingleMapMatches {
        public:
            validateSingleMapMatches(unsigned long long& bytes) :_bytes(bytes)  {}
            void operator () (MongoFile *mf) {
                if( mf->isMongoMMF() ) {
                    MongoMMF *mmf = (MongoMMF*) mf;
                    const char *p = (const char *) mmf->getView();
                    const char *w = (const char *) mmf->view_write();

                    if (!p || !w) return; // File not fully opened yet

                    _bytes += mmf->length();

                    assert( mmf->length() == (unsigned) mmf->length() );
                    {
                        scoped_lock lk( privateViews._mutex() ); // see setNoJournal
                        if (memcmp(p, w, (unsigned) mmf->length()) == 0)
                            return; // next file
                    }

                    unsigned low = 0xffffffff;
                    unsigned high = 0;
                    log() << "DurParanoid mismatch in " << mmf->filename() << endl;
                    int logged = 0;
                    unsigned lastMismatch = 0xffffffff;
                    for( unsigned i = 0; i < mmf->length(); i++ ) {
                        if( p[i] != w[i] ) {
                            if( lastMismatch != 0xffffffff && lastMismatch+1 != i )
                                log() << endl; // separate blocks of mismatches
                            lastMismatch= i;
                            if( ++logged < 60 ) {
                                stringstream ss;
                                ss << "mismatch ofs:" << hex << i <<  "\tfilemap:" << setw(2) << (unsigned) w[i] << "\tprivmap:" << setw(2) << (unsigned) p[i];
                                if( p[i] > 32 && p[i] <= 126 )
                                    ss << '\t' << p[i];
                                log() << ss.str() << endl;
                            }
                            if( logged == 60 )
                                log() << "..." << endl;
                            if( i < low ) low = i;
                            if( i > high ) high = i;
                        }
                    }
                    if( low != 0xffffffff ) {
                        std::stringstream ss;
                        ss << "dur error warning views mismatch " << mmf->filename() << ' ' << (hex) << low << ".." << high << " len:" << high-low+1;
                        log() << ss.str() << endl;
                        log() << "priv loc: " << (void*)(p+low) << ' ' << endl;
                        set<WriteIntent>& b = commitJob.writes();
                        (void)b; // mark as unused. Useful for inspection in debugger

                        // should we abort() here so this isn't unnoticed in some circumstances?
                        massert(13599, "Written data does not match in-memory view. Missing WriteIntent?", false);
                    }
                }
            }
        private:
            unsigned long long& _bytes;
        };

        /** (SLOW) diagnostic to check that the private view and the non-private view are in sync.
        */
        void debugValidateAllMapsMatch() {
            if( ! (cmdLine.durOptions & CmdLine::DurParanoid) )
                return;

            unsigned long long bytes = 0;
            Timer t;
            MongoFile::forEach(validateSingleMapMatches(bytes));
            OCCASIONALLY log() << "DurParanoid map check " << t.millis() << "ms for " <<  (bytes / (1024*1024)) << "MB" << endl;
        }

        extern size_t privateMapBytes;

        /** We need to remap the private views periodically. otherwise they would become very large.
            Call within write lock.
        */
        void _REMAPPRIVATEVIEW() {
            static unsigned startAt;
            static unsigned long long lastRemap;

            dbMutex.assertWriteLocked();
            dbMutex._remapPrivateViewRequested = false;
            assert( !commitJob.hasWritten() );

            // we want to remap all private views about every 2 seconds.  there could be ~1000 views so
            // we do a little each pass; beyond the remap time, more significantly, there will be copy on write
            // faults after remapping, so doing a little bit at a time will avoid big load spikes on
            // remapping.
            unsigned long long now = curTimeMicros64();
            double fraction = (now-lastRemap)/20000000.0;
            lastRemap = now;

            rwlock lk(MongoFile::mmmutex, false);
            set<MongoFile*>& files = MongoFile::getAllFiles();
            unsigned sz = files.size();
            if( sz == 0 )
                return;

            {
                // be careful not to use too much memory if the write rate is 
                // extremely high
                double f = privateMapBytes / ((double)UncommittedBytesLimit);
                if( f > fraction ) { 
                    fraction = f;
                }
                privateMapBytes = 0;
            }

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
        void REMAPPRIVATEVIEW() {
            Timer t;
            _REMAPPRIVATEVIEW();
            stats.curr->_remapPrivateViewMicros += t.micros();
        }

        mutex groupCommitMutex("groupCommit");

        /** locking: in read lock when called. */
        static void _groupCommit() {
            stats.curr->_commits++;

            if( !commitJob.hasWritten() ) {
                // getlasterror request could have came after the data was already committed
                commitJob.notifyCommitted();
                return;
            }

            // we need to make sure two group commits aren't running at the same time
            // (and we are only read locked in the dbMutex, so it could happen)
            scoped_lock lk(groupCommitMutex);

            PREPLOGBUFFER();

            // todo : write to the journal outside locks, as this write can be slow.
            //        however, be careful then about remapprivateview as that cannot be done 
            //        if new writes are then pending in the private maps.
            WRITETOJOURNAL(commitJob._ab);

            // data is now in the journal, which is sufficient for acknowledging getLastError.
            // (ok to crash after that)
            commitJob.notifyCommitted();

            WRITETODATAFILES();

            commitJob.reset();

            // REMAPPRIVATEVIEW
            //
            // remapping private views must occur after WRITETODATAFILES otherwise
            // we wouldn't see newly written data on reads.
            //
            DEV assert( !commitJob.hasWritten() );
            if( !dbMutex.isWriteLocked() ) {
                // this needs done in a write lock (as there is a short window during remapping when each view 
                // might not exist) thus we do it on the next acquisition of that instead of here (there is no 
                // rush if you aren't writing anyway -- but it must happen, if it is done, before any uncommitted 
                // writes occur).  If desired, perhpas this can be eliminated on posix as it may be that the remap 
                // is race-free there.
                //
                dbMutex._remapPrivateViewRequested = true;
            }
            else {
                stats.curr->_commitsInWriteLock++;
                // however, if we are already write locked, we must do it now -- up the call tree someone
                // may do a write without a new lock acquisition.  this can happen when MongoMMF::close() calls
                // this method when a file (and its views) is about to go away.
                //
                REMAPPRIVATEVIEW();
            }
        }

        /** locking in read lock when called
            @see MongoMMF::close()
        */
        static void groupCommit() {
            // we need to be at least read locked on the dbMutex so that we know the write intent data 
            // structures are not changing while we work
            dbMutex.assertAtLeastReadLocked();

            try {
                _groupCommit();
            }
            catch(DBException& e ) { 
                log() << "dbexception in groupCommit causing immediate shutdown: " << e.toString() << endl;
                abort();
            }
            catch(std::ios_base::failure& e) { 
                log() << "ios_base exception in groupCommit causing immediate shutdown: " << e.what() << endl;
                abort();
            }
            catch(std::bad_alloc& e) { 
                log() << "bad_alloc exception in groupCommit causing immediate shutdown: " << e.what() << endl;
                abort();
            }
            catch(std::exception& e) {
                log() << "exception in dur::groupCommit causing immediate shutdown: " << e.what() << endl;
                abort(); // based on myTerminate()
            }
        }

        static void go() {
            {
                readlocktry lk("", 1000);
                if( lk.got() ) {
                    groupCommit();
                    return;
                }
            }

            // starvation on read locks could occur.  so if read lock acquisition is slow, try to get a
            // write lock instead.  otherwise journaling could be delayed too long (too much data will 
            // not accumulate though, as commitIfNeeded logic will have executed in the meantime if there 
            // has been writes)
            writelock lk;
            groupCommit();
        }

        /** called when a MongoMMF is closing -- we need to go ahead and group commit in that case before its
            views disappear
        */
        void closingFileNotification() {
            if (!cmdLine.dur)
                return;

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

        CodeBlock durThreadMain;

        void durThread() {
            Client::initThread("dur");
            const int HowOftenToGroupCommitMs = 90;
            while( !inShutdown() ) {
                sleepmillis(10);
                CodeBlock::Within w(durThreadMain);
                try {
                    int millis = HowOftenToGroupCommitMs;
                    {
                        stats.rotate();
                        {
                            Timer t;
                            journalRotate(); // note we do this part outside of mongomutex
                            millis -= t.millis();
                            assert( millis <= HowOftenToGroupCommitMs );
                            if( millis < 5 )
                                millis = 5;
                        }

                        // we do this in a couple blocks, which makes it a tiny bit faster (only a little) on throughput,
                        // but is likely also less spiky on our cpu usage, which is good:
                        sleepmillis(millis/2);
                        commitJob.wi()._deferred.invoke();
                        sleepmillis(millis/2);
                        commitJob.wi()._deferred.invoke();
                    }

                    go();
                }
                catch(std::exception& e) {
                    log() << "exception in durThread causing immediate shutdown: " << e.what() << endl;
                    abort(); // based on myTerminate()
                }
            }
            cc().shutdown();
        }

        void recover();

        void releasingWriteLock() {
            // implicit commitIfNeeded check on each write unlock
            DEV commitJob._nSinceCommitIfNeededCall = 0; // implicit commit if needed
            if( commitJob.bytes() > UncommittedBytesLimit || cmdLine.durOptions & CmdLine::DurAlwaysCommit ) {
                stats.curr->_earlyCommits++;
                groupCommit();
            }
        }

        void preallocateFiles();

        /** at startup, recover, and then start the journal threads */
        void startup() {
            if( !cmdLine.dur )
                return;

            DurableInterface::enableDurability();

            journalMakeDir();
            try {
                recover();
            }
            catch(...) {
                log() << "exception during recovery" << endl;
                throw;
            }

            preallocateFiles();

            boost::thread t(durThread);
        }

        void DurableImpl::syncDataAndTruncateJournal() {
            dbMutex.assertWriteLocked();

            groupCommit();
            MongoFile::flushAll(true);
            journalCleanup();

            assert(!haveJournalFiles()); // Double check post-conditions
        }

    } // namespace dur

} // namespace mongo
