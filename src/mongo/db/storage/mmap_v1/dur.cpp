/**
 *    Copyright (C) 2009-2014 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/*
   phases:

     PREPLOGBUFFER
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     WRITETOJOURNAL
       we could be unlocked (the main db lock that is...) for this, with sufficient care, but there is some complexity
         have to handle falling behind which would use too much ram (going back into a read lock would suffice to stop that).
         for now (1.7.5/1.8.0) we are in read lock which is not ideal.
     WRITETODATAFILES
       actually write to the database data files in this phase.  currently done by memcpy'ing the writes back to 
       the non-private MMF.  alternatively one could write to the files the traditional way; however the way our 
       storage engine works that isn't any faster (actually measured a tiny bit slower).
     REMAPPRIVATEVIEW
       we could in a write lock quickly flip readers back to the main view, then stay in read lock and do our real
         remapping. with many files (e.g., 1000), remapping could be time consuming (several ms), so we don't want
         to be too frequent.
       there could be a slow down immediately after remapping as fresh copy-on-writes for commonly written pages will
         be required.  so doing these remaps fractionally is helpful. 

   mutexes:

     READLOCK dbMutex (big 'R')
     LOCK groupCommitMutex
       PREPLOGBUFFER()
     READLOCK mmmutex
       commitJob.reset()
     UNLOCK dbMutex                      // now other threads can write
       WRITETOJOURNAL()
       WRITETODATAFILES()
     UNLOCK mmmutex
     UNLOCK groupCommitMutex

   every Nth groupCommit, at the end, we REMAPPRIVATEVIEW() at the end of the work. because of
   that we are in W lock for that groupCommit, which is nonideal of course.

   @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kJournal

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur.h"

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <iomanip>

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace dur {

    using boost::shared_ptr;

namespace {

    // Used to activate the flush thread
    boost::mutex flushMutex;
    boost::condition_variable flushRequested;

    // When set, the flush thread will exit
    AtomicUInt32 shutdownRequested(0);

    // One instance of each durability interface
    DurableImpl durableImpl;
    NonDurableImpl nonDurableImpl;

    // Remap loop state
    unsigned remapFileToStartAt;
    unsigned long long remapLastTimestamp;


    /**
     * MMAP V1 durability server status section.
     */
    class DurSSS : public ServerStatusSection {
    public:
        DurSSS() : ServerStatusSection("dur") {

        }

        virtual bool includeByDefault() const { return true; }

        virtual BSONObj generateSection(OperationContext* txn,
                                        const BSONElement& configElement) const {

            if (!storageGlobalParams.dur) {
                return BSONObj();
            }

            return dur::stats.asObj();
        }

    } durSSS;

}

        // Declared in dur_preplogbuffer.cpp
        void PREPLOGBUFFER(JSectHeader& outHeader, AlignedBuilder& outBuffer);
        void WRITETOJOURNAL(const JSectHeader& h, const AlignedBuilder& uncompressed);
        void WRITETODATAFILES(const JSectHeader& h, const AlignedBuilder& uncompressed);

        // Declared in dur_journal.cpp
        boost::filesystem::path getJournalDir();
        void preallocateFiles();

        // Declared and maintained in dur_commitjob.cpp
        extern size_t privateMapBytes;

        // Durability activity statistics
        Stats stats;

        // Reference to the write intents tracking object
        CommitJob& commitJob = *(new CommitJob()); // don't destroy

        // The durability interface to use
        DurableInterface* DurableInterface::_impl = &nonDurableImpl;


        //
        // Stats
        //

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

        string Stats::S::_CSVHeader() { 
            return "cmts  jrnMB\twrDFMB\tcIWLk\tearly\tprpLgB  wrToJ\twrToDF\trmpPrVw";
        }

        string Stats::S::_asCSV() { 
            stringstream ss;
            ss << 
                setprecision(2) << 
                _commits << '\t' << fixed << 
                _journaledBytes / 1000000.0 << '\t' << 
                _writeToDataFilesBytes / 1000000.0 << '\t' << 
                0 << '\t' <<
                0 <<  '\t' <<
                (unsigned) (_prepLogBufferMicros/1000) << '\t' << 
                (unsigned) (_writeToJournalMicros/1000) << '\t' << 
                (unsigned) (_writeToDataFilesMicros/1000) << '\t' << 
                (unsigned) (_remapPrivateViewMicros/1000);
            return ss.str();
        }

        BSONObj Stats::S::_asObj() {
            BSONObjBuilder b;
            b << 
                       "commits" << _commits <<
                       "journaledMB" << _journaledBytes / 1000000.0 <<
                       "writeToDataFilesMB" << _writeToDataFilesBytes / 1000000.0 <<
                       "compression" << _journaledBytes / (_uncompressedBytes+1.0) <<
                       "commitsInWriteLock" << 0 <<
                       "earlyCommits" << 0 <<
                       "timeMs" <<
                       BSON( "dt" << _dtMillis <<
                             "prepLogBuffer" << (unsigned) (_prepLogBufferMicros/1000) <<
                             "writeToJournal" << (unsigned) (_writeToJournalMicros/1000) <<
                             "writeToDataFiles" << (unsigned) (_writeToDataFilesMicros/1000) <<
                             "remapPrivateView" << (unsigned) (_remapPrivateViewMicros/1000)
                           );
            if (mmapv1GlobalOptions.journalCommitInterval != 0)
                b << "journalCommitIntervalMs" << mmapv1GlobalOptions.journalCommitInterval;
            return b.obj();
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


        //
        // DurableInterface
        //

        DurableInterface::DurableInterface() {

        }

        DurableInterface::~DurableInterface() {

        }

        void DurableInterface::enableDurability() {
            _impl = &durableImpl;
        }


        //
        // NonDurableImpl
        //

        void* NonDurableImpl::writingPtr(void *x, unsigned len) {
            dassert(shutdownRequested.load() == 0);
            return x; 
        }

        void NonDurableImpl::declareWriteIntent(void *, unsigned) { 
        }

        bool NonDurableImpl::commitNow(OperationContext* txn) {
            return false;
        }

        bool NonDurableImpl::commitIfNeeded() {
            return false;
        }


        //
        // DurableImpl
        //

        bool DurableImpl::commitNow(OperationContext* txn) {
            NotifyAll::When when = commitJob._notify.now();

            AutoYieldFlushLockForMMAPV1Commit flushLockYield(txn->lockState());

            // There is always just one waiting anyways
            flushRequested.notify_one();
            commitJob._notify.waitFor(when);

            return true;
        }

        bool DurableImpl::awaitCommit() {
            commitJob._notify.awaitBeyondNow();
            return true;
        }

        void DurableImpl::createdFile(const std::string& filename, unsigned long long len) {
            shared_ptr<DurOp> op( new FileCreatedOp(filename, len) );
            commitJob.noteOp(op);
        }

        void* DurableImpl::writingPtr(void *x, unsigned len) {
            dassert(shutdownRequested.load() == 0);
            void *p = x;
            declareWriteIntent(p, len);
            return p;
        }

        bool DurableImpl::commitIfNeeded() {
            if (MONGO_likely(commitJob.bytes() < UncommittedBytesLimit)) {
                return false;
            }

            // Just wake up the flush thread
            flushRequested.notify_one();
            return true;
        }

        void DurableImpl::syncDataAndTruncateJournal(OperationContext* txn) {
            invariant(txn->lockState()->isW());

            commitNow(txn);
            MongoFile::flushAll(true);
            journalCleanup();

            invariant(!haveJournalFiles()); // Double check post-conditions
        }

        void DurableImpl::commitAndStopDurThread() {
            NotifyAll::When when = commitJob._notify.now();

            // There is always just one waiting anyways
            flushRequested.notify_one();
            commitJob._notify.waitFor(when);

            shutdownRequested.store(1);
        }


        /**
         * Diagnostic to check that the private view and the non-private view are in sync after
         * applying the journal changes. This function is very slow and only runs when paranoid
         * checks are enabled.
         *
         * Must be called under at least S flush lock to ensure that there are no concurrent
         * writes happening.
         */
        static void debugValidateFileMapsMatch(const DurableMappedFile* mmf) {
            const unsigned char *p = (const unsigned char *)mmf->getView();
            const unsigned char *w = (const unsigned char *)mmf->view_write();

            // Ignore pre-allocated files that are not fully created yet
            if (!p || !w) {
                return;
            }

            if (memcmp(p, w, (unsigned)mmf->length()) == 0) {
                return;
            }

            unsigned low = 0xffffffff;
            unsigned high = 0;

            log() << "DurParanoid mismatch in " << mmf->filename();

            int logged = 0;
            unsigned lastMismatch = 0xffffffff;

            for (unsigned i = 0; i < mmf->length(); i++) {
                if (p[i] != w[i]) {

                    if (lastMismatch != 0xffffffff && lastMismatch + 1 != i) {
                        // Separate blocks of mismatches
                        log() << std::endl;
                    }

                    lastMismatch = i;

                    if (++logged < 60) {
                        if (logged == 1) {
                            // For .ns files to find offset in record
                            log() << "ofs % 628 = 0x" << hex << (i % 628) << endl;
                        }

                        stringstream ss;
                        ss << "mismatch ofs:" << hex << i
                           << "\tfilemap:" << setw(2) << (unsigned)w[i]
                           << "\tprivmap:" << setw(2) << (unsigned)p[i];

                        if (p[i] > 32 && p[i] <= 126) {
                            ss << '\t' << p[i];
                        }

                        log() << ss.str() << endl;
                    }

                    if (logged == 60) {
                        log() << "..." << endl;
                    }

                    if (i < low) low = i;
                    if (i > high) high = i;
                }
            }

            if (low != 0xffffffff) {
                std::stringstream ss;
                ss << "journal error warning views mismatch " << mmf->filename() << ' '
                   << hex << low << ".." << high
                   << " len:" << high - low + 1;

                log() << ss.str() << endl;
                log() << "priv loc: " << (void*)(p + low) << ' ' << endl;

                severe() << "Written data does not match in-memory view. Missing WriteIntent?";
                invariant(false);
            }
        }


        static void _remapPrivateView() {
            LOG(4) << "journal REMAPPRIVATEVIEW" << endl;

            // We want to remap all private views about every 2 seconds. There could be ~1000 views
            // so we do a little each pass. There will be copy on write faults after remapping, so
            // doing a little bit at a time will avoid big load spikes when the pages are touched.
            //
            // TODO: Instead of the time-based logic above, consider using ProcessInfo and watching
            //       for getResidentSize to drop, which is more precise
            unsigned long long now = curTimeMicros64();
            double fraction = (now - remapLastTimestamp) / 2000000.0;
            if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalAlwaysRemap) {
                fraction = 1;
            }

            // We don't want to get close to the UncommittedBytesLimit
            const double f = privateMapBytes / ((double)UncommittedBytesLimit);
            if (f > fraction) {
                fraction = f;
            }

            // We are definitely doing some remap, so reset the private map estimate
            privateMapBytes = 0;
            remapLastTimestamp = now;

            // There is no way that the set of files can change while we are in this method,
            // because we hold the flush lock in X mode. For files to go away, a database needs to
            // be dropped, which means acquiring the flush lock in at least IX mode.
            //
            // However, the record fetcher logic unfortunately operates without any locks and on
            // Windows and Solaris remap is not atomic and there is a window where the record
            // fetcher might get an access violation. That's why we acquire the mongo files mutex
            // here in X mode and the record fetcher takes in in S-mode (see MmapV1RecordFetcher
            // for more detail).
            //
            // See SERVER-5723 for performance improvement.
            // See SERVER-5680 to see why this code is necessary on Windows.
            // See SERVER-8795 to see why this code is necessary on Solaris.
#if defined(_WIN32) || defined(__sunos__)
            LockMongoFilesExclusive lk;
#else
            LockMongoFilesShared lk;
#endif

            std::set<MongoFile*>& files = MongoFile::getAllFiles();

            const unsigned sz = files.size();
            if (sz == 0) {
                return;
            }

            unsigned ntodo = (unsigned) (sz * fraction);
            if( ntodo < 1 ) ntodo = 1;
            if( ntodo > sz ) ntodo = sz;

            const set<MongoFile*>::iterator b = files.begin();
            const set<MongoFile*>::iterator e = files.end();
            set<MongoFile*>::iterator i = b;

            // Skip to our starting position as remembered from the last remap cycle
            for( unsigned x = 0; x < remapFileToStartAt; x++ ) {
                i++;
                if( i == e ) i = b;
            }

            // Mark where to start on the next cycle
            const unsigned startedAt = remapFileToStartAt;
            remapFileToStartAt = (remapFileToStartAt + ntodo) % sz;

            Timer t;

            for( unsigned x = 0; x < ntodo; x++ ) {
                dassert( i != e );
                if ((*i)->isDurableMappedFile()) {
                    DurableMappedFile *mmf = (DurableMappedFile*) *i;

                    // Sanity check that the contents of the shared and the private view match so
                    // we don't end up overwriting data.
                    if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalParanoid) {
                        debugValidateFileMapsMatch(mmf);
                    }

                    if (mmf->willNeedRemap()) {
                        mmf->remapThePrivateView();
                    }

                    i++;

                    if( i == e ) i = b;
                }
            }

            LOG(3) << "journal REMAPPRIVATEVIEW done startedAt: " << startedAt << " n:" << ntodo
                   << ' ' << t.millis() << "ms";
        }


        /**
         * Remaps the private view from the shared view so that it does not consume too much
         * copy-on-write/swap space. Must only be called after the in-memory journal has been
         * flushed to disk and applied on top of the shared view.
         */
        static void remapPrivateView() {
            // Remapping private views must occur after WRITETODATAFILES otherwise we wouldn't see
            // any newly written data on reads.
            invariant(!commitJob.hasWritten());

            try {
                Timer t;
                _remapPrivateView();
                stats.curr->_remapPrivateViewMicros += t.micros();

                LOG(4) << "remapPrivateView end";
                return;
            }
            catch (DBException& e) {
                severe() << "dbexception in remapPrivateView causing immediate shutdown: "
                         << e.toString();
            }
            catch (std::ios_base::failure& e) {
                severe() << "ios_base exception in remapPrivateView causing immediate shutdown: "
                         << e.what();
            }
            catch (std::bad_alloc& e) {
                severe() << "bad_alloc exception in remapPrivateView causing immediate shutdown: "
                         << e.what();
            }
            catch (std::exception& e) {
                severe() << "exception in remapPrivateView causing immediate shutdown: "
                         << e.what();
            }
            catch (...) {
                severe() << "unknown exception in remapPrivateView causing immediate shutdown: ";
            }

            invariant(false);
        }


        /**
         * Called when a DurableMappedFile is closing and must only happen under a global lock (at
         * least R, so that no writes can happen). Asserts that there are no unwritten changes,
         * because that would mean journal replay on recovery would try to write to non-existent
         * files and fail.
         */
        void closingFileNotification() {
            if (commitJob.hasWritten()) {
                severe() << "journal warning files are closing outside locks with writes pending";

                // File is closing while there are unwritten changes
                invariant(false);
            }
        }


        static void durThread() {
            Client::initThread("journal");

            bool samePartition = true;
            try {
                const std::string dbpathDir =
                    boost::filesystem::path(storageGlobalParams.dbpath).string();
                samePartition = onSamePartition(getJournalDir().string(), dbpathDir);
            }
            catch(...) {

            }

            // Pre-allocated buffer for building the journal
            AlignedBuilder journalBuilder(4 * 1024 * 1024);

            while (shutdownRequested.loadRelaxed() == 0) {
                unsigned ms = mmapv1GlobalOptions.journalCommitInterval;
                if( ms == 0 ) { 
                    ms = samePartition ? 100 : 30;
                }

                unsigned oneThird = (ms / 3) + 1; // +1 so never zero

                try {
                    stats.rotate();

                    boost::mutex::scoped_lock lock(flushMutex);

                    for (unsigned i = 0; i <= 2; i++) {
                        if (flushRequested.timed_wait(lock, Milliseconds(oneThird))) {
                            // Someone forced a flush
                            break;
                        }

                        if (commitJob._notify.nWaiting()) {
                            // One or more getLastError j:true is pending
                            break;
                        }

                        if (commitJob.bytes() > UncommittedBytesLimit / 2) {
                            // The number of written bytes is growing
                            break;
                        }
                    }

                    // The commit logic itself
                    LOG(4) << "groupCommit";

                    OperationContextImpl txn;

                    AutoAcquireFlushLockForMMAPV1Commit autoFlushLock(txn.lockState());

                    commitJob.commitingBegin();

                    if (commitJob.hasWritten()) {
                        JSectHeader h;
                        PREPLOGBUFFER(h, journalBuilder);

                        // todo : write to the journal outside locks, as this write can be slow.
                        //        however, be careful then about remapprivateview as that cannot be done 
                        //        if new writes are then pending in the private maps.
                        WRITETOJOURNAL(h, journalBuilder);

                        // Data is now in the journal, which is sufficient for acknowledging getLastError.
                        commitJob.committingNotifyCommitted();
                        commitJob.committingReset();

                        WRITETODATAFILES(h, journalBuilder);

                        journalBuilder.reset();
                    }
                    else {
                        // getlasterror request could have came after the data was already
                        // committed
                        commitJob.committingNotifyCommitted();
                    }

                    autoFlushLock.upgradeFlushLockToExclusive();
                    remapPrivateView();

                    LOG(4) << "groupCommit end";
                }
                catch (DBException& e) {
                    severe() << "dbexception in durThread causing immediate shutdown: "
                             << e.toString();
                    invariant(false);
                }
                catch (std::ios_base::failure& e) {
                    severe() << "ios_base exception in durThread causing immediate shutdown: "
                             << e.what();
                    invariant(false);
                }
                catch (std::bad_alloc& e) {
                    severe() << "bad_alloc exception in durThread causing immediate shutdown: "
                             << e.what();
                    invariant(false);
                }
                catch (std::exception& e) {
                    severe() << "exception in durThread causing immediate shutdown: "
                             << e.what();
                    invariant(false);
                }
                catch (...) {
                    severe() << "unhandled exception in durThread causing immediate shutdown";
                    invariant(false);
                }
            }

            cc().shutdown();
        }


        /**
         * Invoked at server startup. Recovers the database by replaying journal files and then
         * starts the durability thread.
         */
        void startup() {
            if (!storageGlobalParams.dur) {
                return;
            }

            journalMakeDir();

            try {
                replayJournalFilesAtStartup();
            }
            catch(DBException& e) {
                severe() << "dbexception during recovery: " << e.toString();
                throw;
            }
            catch(std::exception& e) {
                severe() << "std::exception during recovery: " << e.what();
                throw;
            }
            catch(...) {
                severe() << "exception during recovery";
                throw;
            }

            preallocateFiles();

            DurableInterface::enableDurability();
            boost::thread t(durThread);
        }

} // namespace dur
} // namespace mongo
