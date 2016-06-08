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
       we could be unlocked (the main db lock that is...) for this, with sufficient care, but there
       is some complexity have to handle falling behind which would use too much ram (going back
       into a read lock would suffice to stop that). for now (1.7.5/1.8.0) we are in read lock which
       is not ideal.
     WRITETODATAFILES
       actually write to the database data files in this phase.  currently done by memcpy'ing the
       writes back to the non-private MMF.  alternatively one could write to the files the
       traditional way; however the way our storage engine works that isn't any faster (actually
       measured a tiny bit slower).
     REMAPPRIVATEVIEW
       we could in a write lock quickly flip readers back to the main view, then stay in read lock
       and do our real remapping. with many files (e.g., 1000), remapping could be time consuming
       (several ms), so we don't want to be too frequent. there could be a slow down immediately
       after remapping as fresh copy-on-writes for commonly written pages will
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

#include <iomanip>
#include <utility>

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/aligned_builder.h"
#include "mongo/db/storage/mmap_v1/commit_notifier.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_journal_writer.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::endl;
using std::fixed;
using std::hex;
using std::set;
using std::setprecision;
using std::setw;
using std::string;
using std::stringstream;

namespace dur {

namespace {

// Used to activate the flush thread
stdx::mutex flushMutex;
stdx::condition_variable flushRequested;

// This is waited on for getlasterror acknowledgements. It means that data has been written to
// the journal, but not necessarily applied to the shared view, so it is all right to
// acknowledge the user operation, but NOT all right to delete the journal files for example.
CommitNotifier commitNotify;

// This is waited on for complete flush. It means that data has been both written to journal
// and applied to the shared view, so it is allowed to delete the journal files. Used for
// fsync:true, close DB, shutdown acknowledgements.
CommitNotifier applyToDataFilesNotify;

// When set, the flush thread will exit
AtomicUInt32 shutdownRequested(0);

enum {
    // How many commit cycles to do before considering doing a remap
    NumCommitsBeforeRemap = 10,

    // How many outstanding journal flushes should be allowed before applying writer back
    // pressure. Size of 1 allows two journal blocks to be in the process of being written -
    // one on the journal writer's buffer and one blocked waiting to be picked up.
    NumAsyncJournalWrites = 1,
};

// Remap loop state
unsigned remapFileToStartAt;

// How frequently to reset the durability statistics
enum { DurStatsResetIntervalMillis = 3 * 1000 };

// Size sanity checks
static_assert(UncommittedBytesLimit > BSONObjMaxInternalSize * 3,
              "UncommittedBytesLimit > BSONObjMaxInternalSize * 3");
static_assert(sizeof(void*) == 4 || UncommittedBytesLimit > BSONObjMaxInternalSize * 6,
              "sizeof(void*) == 4 || UncommittedBytesLimit > BSONObjMaxInternalSize * 6");


/**
 * MMAP V1 durability server status section.
 */
class DurSSS : public ServerStatusSection {
public:
    DurSSS() : ServerStatusSection("dur") {}

    virtual bool includeByDefault() const {
        return true;
    }

    virtual BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        if (!getDur().isDurable()) {
            return BSONObj();
        }

        return dur::stats.asObj();
    }

} durSSS;


/**
 * A no-op durability interface. Used for the case when journaling is not enabled.
 */
class NonDurableImpl : public DurableInterface {
public:
    NonDurableImpl() {}

    // DurableInterface virtual methods
    virtual void* writingPtr(void* x, unsigned len) {
        return x;
    }
    virtual void declareWriteIntent(void*, unsigned) {}
    virtual void declareWriteIntents(const std::vector<std::pair<void*, unsigned>>& intents) {}
    virtual void createdFile(const std::string& filename, unsigned long long len) {}
    virtual bool waitUntilDurable() {
        return false;
    }
    virtual bool commitNow(OperationContext* txn) {
        return false;
    }
    virtual bool commitIfNeeded() {
        return false;
    }
    virtual void syncDataAndTruncateJournal(OperationContext* txn) {}
    virtual bool isDurable() const {
        return false;
    }
    virtual void closingFileNotification() {}
    virtual void commitAndStopDurThread() {}
};


/**
 * The actual durability interface, when journaling is enabled.
 */
class DurableImpl : public DurableInterface {
public:
    DurableImpl() {}

    // DurableInterface virtual methods
    virtual void declareWriteIntents(const std::vector<std::pair<void*, unsigned>>& intents);
    virtual void createdFile(const std::string& filename, unsigned long long len);
    virtual bool waitUntilDurable();
    virtual bool commitNow(OperationContext* txn);
    virtual bool commitIfNeeded();
    virtual void syncDataAndTruncateJournal(OperationContext* txn);
    virtual bool isDurable() const {
        return true;
    }
    virtual void closingFileNotification();
    virtual void commitAndStopDurThread();

    void start(ClockSource* cs, int64_t serverStartMs);

private:
    stdx::thread _durThreadHandle;
};


/**
 * Diagnostic to check that the private view and the non-private view are in sync after
 * applying the journal changes. This function is very slow and only runs when paranoid checks
 * are enabled.
 *
 * Must be called under at least S flush lock to ensure that there are no concurrent writes
 * happening.
 */
void debugValidateFileMapsMatch(const DurableMappedFile* mmf) {
    const unsigned char* p = (const unsigned char*)mmf->getView();
    const unsigned char* w = (const unsigned char*)mmf->view_write();

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
                ss << "mismatch ofs:" << hex << i << "\tfilemap:" << setw(2) << (unsigned)w[i]
                   << "\tprivmap:" << setw(2) << (unsigned)p[i];

                if (p[i] > 32 && p[i] <= 126) {
                    ss << '\t' << p[i];
                }

                log() << ss.str() << endl;
            }

            if (logged == 60) {
                log() << "..." << endl;
            }

            if (i < low)
                low = i;
            if (i > high)
                high = i;
        }
    }

    if (low != 0xffffffff) {
        std::stringstream ss;
        ss << "journal error warning views mismatch " << mmf->filename() << ' ' << hex << low
           << ".." << high << " len:" << high - low + 1;

        log() << ss.str() << endl;
        log() << "priv loc: " << (void*)(p + low) << ' ' << endl;

        severe() << "Written data does not match in-memory view. Missing WriteIntent?";
        invariant(false);
    }
}


/**
 * Main code of the remap private view function.
 */
void remapPrivateViewImpl(double fraction) {
    LOG(4) << "journal REMAPPRIVATEVIEW" << endl;

// There is no way that the set of files can change while we are in this method, because
// we hold the flush lock in X mode. For files to go away, a database needs to be dropped,
// which means acquiring the flush lock in at least IX mode.
//
// However, the record fetcher logic unfortunately operates without any locks and on
// Windows and Solaris remap is not atomic and there is a window where the record fetcher
// might get an access violation. That's why we acquire the mongo files mutex here in X
// mode and the record fetcher takes in in S-mode (see MmapV1RecordFetcher for more
// detail).
//
// See SERVER-5723 for performance improvement.
// See SERVER-5680 to see why this code is necessary on Windows.
// See SERVER-8795 to see why this code is necessary on Solaris.
#if defined(_WIN32) || defined(__sun)
    LockMongoFilesExclusive lk;
#else
    LockMongoFilesShared lk;
#endif

    std::set<MongoFile*>& files = MongoFile::getAllFiles();

    const unsigned sz = files.size();
    if (sz == 0) {
        return;
    }

    unsigned ntodo = (unsigned)(sz * fraction);
    if (ntodo < 1)
        ntodo = 1;
    if (ntodo > sz)
        ntodo = sz;

    const set<MongoFile*>::iterator b = files.begin();
    const set<MongoFile*>::iterator e = files.end();
    set<MongoFile*>::iterator i = b;

    // Skip to our starting position as remembered from the last remap cycle
    for (unsigned x = 0; x < remapFileToStartAt; x++) {
        i++;
        if (i == e)
            i = b;
    }

    // Mark where to start on the next cycle
    const unsigned startedAt = remapFileToStartAt;
    remapFileToStartAt = (remapFileToStartAt + ntodo) % sz;

    Timer t;

    for (unsigned x = 0; x < ntodo; x++) {
        if ((*i)->isDurableMappedFile()) {
            DurableMappedFile* const mmf = (DurableMappedFile*)*i;

            // Sanity check that the contents of the shared and the private view match so we
            // don't end up overwriting data.
            if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalParanoid) {
                debugValidateFileMapsMatch(mmf);
            }

            if (mmf->willNeedRemap()) {
                mmf->remapThePrivateView();
            }

            i++;

            if (i == e)
                i = b;
        }
    }

    LOG(3) << "journal REMAPPRIVATEVIEW done startedAt: " << startedAt << " n:" << ntodo << ' '
           << t.millis() << "ms";
}


// One instance of each durability interface
DurableImpl durableImpl;
NonDurableImpl nonDurableImpl;

// Notified when we commit to the journal.
static JournalListener* journalListener = &NoOpJournalListener::instance;
// Protects journalListener.
static stdx::mutex journalListenerMutex;

}  // namespace


// Declared in dur_preplogbuffer.cpp
void PREPLOGBUFFER(JSectHeader& outHeader,
                   AlignedBuilder& outBuffer,
                   ClockSource* cs,
                   int64_t serverStartMs);

// Declared in dur_journal.cpp
boost::filesystem::path getJournalDir();
void preallocateFiles();

// Forward declaration
static void durThread(ClockSource* cs, int64_t serverStartMs);

// Durability activity statistics
Stats stats;

// Reference to the write intents tracking object
CommitJob commitJob;

// Reference to the active durability interface
DurableInterface* DurableInterface::_impl(&nonDurableImpl);


//
// Stats
//

Stats::Stats() : _currIdx(0) {}

void Stats::reset() {
    // Seal the current metrics
    _stats[_currIdx]._durationMillis = _stats[_currIdx].getCurrentDurationMillis();

    // Use a new metric
    const unsigned newCurrIdx = (_currIdx + 1) % (sizeof(_stats) / sizeof(_stats[0]));
    _stats[newCurrIdx].reset();

    _currIdx = newCurrIdx;
}

BSONObj Stats::asObj() const {
    // Use the previous statistic
    const S& stats = _stats[(_currIdx - 1) % (sizeof(_stats) / sizeof(_stats[0]))];

    BSONObjBuilder builder;
    stats._asObj(&builder);

    return builder.obj();
}

void Stats::S::reset() {
    memset(this, 0, sizeof(*this));
    _startTimeMicros = curTimeMicros64();
}

std::string Stats::S::_CSVHeader() const {
    return "cmts\t jrnMB\t wrDFMB\t cIWLk\t early\t prpLgB\t wrToJ\t wrToDF\t rmpPrVw";
}

std::string Stats::S::_asCSV() const {
    stringstream ss;
    ss << setprecision(2) << _commits << '\t' << _journaledBytes / 1000000.0 << '\t'
       << _writeToDataFilesBytes / 1000000.0 << '\t' << _commitsInWriteLock << '\t' << 0 << '\t'
       << (unsigned)(_prepLogBufferMicros / 1000) << '\t'
       << (unsigned)(_writeToJournalMicros / 1000) << '\t'
       << (unsigned)(_writeToDataFilesMicros / 1000) << '\t'
       << (unsigned)(_remapPrivateViewMicros / 1000) << '\t' << (unsigned)(_commitsMicros / 1000)
       << '\t' << (unsigned)(_commitsInWriteLockMicros / 1000) << '\t';

    return ss.str();
}

void Stats::S::_asObj(BSONObjBuilder* builder) const {
    BSONObjBuilder& b = *builder;
    b << "commits" << _commits << "journaledMB" << _journaledBytes / 1000000.0
      << "writeToDataFilesMB" << _writeToDataFilesBytes / 1000000.0 << "compression"
      << _journaledBytes / (_uncompressedBytes + 1.0) << "commitsInWriteLock" << _commitsInWriteLock
      << "earlyCommits" << 0 << "timeMs"
      << BSON("dt" << _durationMillis << "prepLogBuffer" << (unsigned)(_prepLogBufferMicros / 1000)
                   << "writeToJournal"
                   << (unsigned)(_writeToJournalMicros / 1000)
                   << "writeToDataFiles"
                   << (unsigned)(_writeToDataFilesMicros / 1000)
                   << "remapPrivateView"
                   << (unsigned)(_remapPrivateViewMicros / 1000)
                   << "commits"
                   << (unsigned)(_commitsMicros / 1000)
                   << "commitsInWriteLock"
                   << (unsigned)(_commitsInWriteLockMicros / 1000));

    if (storageGlobalParams.journalCommitIntervalMs != 0) {
        b << "journalCommitIntervalMs" << storageGlobalParams.journalCommitIntervalMs.load();
    }
}


//
// DurableInterface
//

DurableInterface::DurableInterface() {}

DurableInterface::~DurableInterface() {}


//
// DurableImpl
//

bool DurableImpl::commitNow(OperationContext* txn) {
    CommitNotifier::When when = commitNotify.now();

    AutoYieldFlushLockForMMAPV1Commit flushLockYield(txn->lockState());

    // There is always just one waiting anyways
    flushRequested.notify_one();

    // commitNotify.waitFor ensures that whatever was scheduled for journaling before this
    // call has been persisted to the journal file. This does not mean that this data has been
    // applied to the shared view yet though, that's why we wait for applyToDataFilesNotify.
    applyToDataFilesNotify.waitFor(when);

    return true;
}

bool DurableImpl::waitUntilDurable() {
    commitNotify.awaitBeyondNow();
    return true;
}

void DurableImpl::createdFile(const std::string& filename, unsigned long long len) {
    std::shared_ptr<DurOp> op(new FileCreatedOp(filename, len));
    commitJob.noteOp(op);
}


void DurableImpl::declareWriteIntents(const std::vector<std::pair<void*, unsigned>>& intents) {
    typedef std::vector<std::pair<void*, unsigned>> Intents;
    stdx::lock_guard<SimpleMutex> lk(commitJob.groupCommitMutex);
    for (Intents::const_iterator it(intents.begin()), end(intents.end()); it != end; ++it) {
        commitJob.note(it->first, it->second);
    }
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

    // Once this returns, all the outstanding journal has been applied to the data files and
    // so it's safe to do the flushAll/journalCleanup below.
    commitNow(txn);

    // Flush the shared view to disk.
    MongoFile::flushAll(true);

    // Once the shared view has been flushed, we do not need the journal files anymore.
    journalCleanup(true);

    // Double check post-conditions
    invariant(!haveJournalFiles());
}

void DurableImpl::closingFileNotification() {
    if (commitJob.hasWritten()) {
        severe() << "journal warning files are closing outside locks with writes pending";

        // File is closing while there are unwritten changes
        invariant(false);
    }
}

void DurableImpl::commitAndStopDurThread() {
    CommitNotifier::When when = commitNotify.now();

    // There is always just one waiting anyways
    flushRequested.notify_one();

    // commitNotify.waitFor ensures that whatever was scheduled for journaling before this
    // call has been persisted to the journal file. This does not mean that this data has been
    // applied to the shared view yet though, that's why we wait for applyToDataFilesNotify.
    applyToDataFilesNotify.waitFor(when);

    // Flush the shared view to disk.
    MongoFile::flushAll(true);

    // Once the shared view has been flushed, we do not need the journal files anymore.
    journalCleanup(true);

    // Double check post-conditions
    invariant(!haveJournalFiles());

    shutdownRequested.store(1);

    // Wait for the durability thread to terminate
    log() << "Terminating durability thread ...";
    _durThreadHandle.join();
}

void DurableImpl::start(ClockSource* cs, int64_t serverStartMs) {
    // Start the durability thread
    stdx::thread t(durThread, cs, serverStartMs);
    _durThreadHandle.swap(t);
}


/**
 * Remaps the private view from the shared view so that it does not consume too much
 * copy-on-write/swap space. Must only be called after the in-memory journal has been flushed
 * to disk and applied on top of the shared view.
 *
 * @param fraction Value between (0, 1] indicating what fraction of the memory to remap.
 *      Remapping too much or too frequently incurs copy-on-write page fault cost.
 */
static void remapPrivateView(double fraction) {
    // Remapping private views must occur after WRITETODATAFILES otherwise we wouldn't see any
    // newly written data on reads.
    invariant(!commitJob.hasWritten());

    try {
        Timer t;
        remapPrivateViewImpl(fraction);
        stats.curr()->_remapPrivateViewMicros += t.micros();

        LOG(4) << "remapPrivateView end";
        return;
    } catch (DBException& e) {
        severe() << "dbexception in remapPrivateView causing immediate shutdown: " << e.toString();
    } catch (std::ios_base::failure& e) {
        severe() << "ios_base exception in remapPrivateView causing immediate shutdown: "
                 << e.what();
    } catch (std::bad_alloc& e) {
        severe() << "bad_alloc exception in remapPrivateView causing immediate shutdown: "
                 << e.what();
    } catch (std::exception& e) {
        severe() << "exception in remapPrivateView causing immediate shutdown: " << e.what();
    } catch (...) {
        severe() << "unknown exception in remapPrivateView causing immediate shutdown: ";
    }

    invariant(false);
}


/**
 * The main durability thread loop. There is a single instance of this function running.
 */
static void durThread(ClockSource* cs, int64_t serverStartMs) {
    Client::initThread("durability");

    log() << "Durability thread started";

    bool samePartition = true;
    try {
        const std::string dbpathDir = boost::filesystem::path(storageGlobalParams.dbpath).string();
        samePartition = onSamePartition(getJournalDir().string(), dbpathDir);
    } catch (...) {
    }

    // Spawn the journal writer thread
    JournalWriter journalWriter(&commitNotify, &applyToDataFilesNotify, NumAsyncJournalWrites);
    journalWriter.start();

    // Used as an estimate of how much / how fast to remap
    uint64_t commitCounter(0);
    uint64_t estimatedPrivateMapSize(0);
    uint64_t remapLastTimestamp(0);

    while (shutdownRequested.loadRelaxed() == 0) {
        unsigned ms = storageGlobalParams.journalCommitIntervalMs;
        if (ms == 0) {
            ms = samePartition ? 100 : 30;
        }

        // +1 so it never goes down to zero
        const int64_t oneThird = (ms / 3) + 1;

        // Reset the stats based on the reset interval
        if (stats.curr()->getCurrentDurationMillis() > DurStatsResetIntervalMillis) {
            stats.reset();
        }

        try {
            stdx::unique_lock<stdx::mutex> lock(flushMutex);

            for (unsigned i = 0; i <= 2; i++) {
                if (stdx::cv_status::no_timeout ==
                    flushRequested.wait_for(lock, Milliseconds(oneThird).toSystemDuration())) {
                    // Someone forced a flush
                    break;
                }

                if (commitNotify.nWaiting()) {
                    // One or more getLastError j:true is pending
                    break;
                }

                if (commitJob.bytes() > UncommittedBytesLimit / 2) {
                    // The number of written bytes is growing
                    break;
                }
            }

            // The commit logic itself
            LOG(4) << "groupCommit begin";

            Timer t;

            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
            OperationContext& txn = *txnPtr;
            AutoAcquireFlushLockForMMAPV1Commit autoFlushLock(txn.lockState());

            // We need to snapshot the commitNumber after the flush lock has been obtained,
            // because at this point we know that we have a stable snapshot of the data.
            const CommitNotifier::When commitNumber(commitNotify.now());

            LOG(4) << "Processing commit number " << commitNumber;

            if (!commitJob.hasWritten()) {
                // We do not need the journal lock anymore. Free it here, for the really
                // unlikely possibility that the writeBuffer command below blocks.
                autoFlushLock.release();

                // getlasterror request could have came after the data was already committed.
                // No need to call committingReset though, because we have not done any
                // writes (hasWritten == false).
                JournalWriter::Buffer* const buffer = journalWriter.newBuffer();
                buffer->setNoop();
                buffer->journalListenerToken = getJournalListener()->getToken();

                journalWriter.writeBuffer(buffer, commitNumber);
            } else {
                // This copies all the in-memory changes into the journal writer's buffer.
                JournalWriter::Buffer* const buffer = journalWriter.newBuffer();
                PREPLOGBUFFER(buffer->getHeader(), buffer->getBuilder(), cs, serverStartMs);

                estimatedPrivateMapSize += commitJob.bytes();
                commitCounter++;

                // Now that the write intents have been copied to the buffer, the commit job is
                // free to be reused. We need to reset the commit job's contents while under
                // the S flush lock, because otherwise someone might have done a write and this
                // would wipe out their changes without ever being committed.
                commitJob.committingReset();

                double systemMemoryPressurePercentage =
                    ProcessInfo::getSystemMemoryPressurePercentage();

                // Now that the in-memory modifications have been collected, we can potentially
                // release the flush lock if remap is not necessary.
                // When we remap due to memory pressure, we look at two criteria
                // 1. If the amount of 4k pages touched exceeds 512 MB,
                //    a reasonable estimate of memory pressure on Linux.
                // 2. Check if the amount of free memory on the machine is running low,
                //    since #1 is underestimates the memory pressure on Windows since
                //    commits in 64MB chunks.
                const bool shouldRemap = (estimatedPrivateMapSize >= UncommittedBytesLimit) ||
                    (systemMemoryPressurePercentage > 0.0) ||
                    (commitCounter % NumCommitsBeforeRemap == 0) ||
                    (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalAlwaysRemap);

                double remapFraction = 0.0;

                if (shouldRemap) {
                    // We want to remap all private views about every 2 seconds. There could be
                    // ~1000 views so we do a little each pass. There will be copy on write
                    // faults after remapping, so doing a little bit at a time will avoid big
                    // load spikes when the pages are touched.
                    //
                    // TODO: Instead of the time-based logic above, consider using ProcessInfo
                    //       and watching for getResidentSize to drop, which is more precise.
                    remapFraction = (curTimeMicros64() - remapLastTimestamp) / 2000000.0;

                    if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalAlwaysRemap) {
                        remapFraction = 1;
                    } else {
                        // We don't want to get close to the UncommittedBytesLimit
                        const double remapMemFraction =
                            estimatedPrivateMapSize / ((double)UncommittedBytesLimit);

                        remapFraction = std::max(remapMemFraction, remapFraction);

                        remapFraction = std::max(systemMemoryPressurePercentage, remapFraction);
                    }
                } else {
                    LOG(4) << "Early release flush lock";

                    // We will not be doing a remap so drop the flush lock. That way we will be
                    // doing the journal I/O outside of lock, so other threads can proceed.
                    invariant(!shouldRemap);
                    autoFlushLock.release();
                }

                buffer->journalListenerToken = getJournalListener()->getToken();
                // Request async I/O to the journal. This may block.
                journalWriter.writeBuffer(buffer, commitNumber);

                // Data has now been written to the shared view. If remap was requested, we
                // would still be holding the S flush lock here, so just upgrade it and
                // perform the remap.
                if (shouldRemap) {
                    // Need to wait for the previously scheduled journal writes to complete
                    // before any remap is attempted.
                    journalWriter.flush();
                    journalWriter.assertIdle();

                    // Upgrading the journal lock to flush stops all activity on the system,
                    // because we will be remapping memory and we don't want readers to be
                    // accessing it. Technically this step could be avoided on systems, which
                    // support atomic remap.
                    autoFlushLock.upgradeFlushLockToExclusive();
                    remapPrivateView(remapFraction);

                    autoFlushLock.release();

                    // Reset the private map estimate outside of the lock
                    estimatedPrivateMapSize = 0;
                    remapLastTimestamp = curTimeMicros64();

                    stats.curr()->_commitsInWriteLock++;
                    stats.curr()->_commitsInWriteLockMicros += t.micros();
                }
            }

            stats.curr()->_commits++;
            stats.curr()->_commitsMicros += t.micros();

            LOG(4) << "groupCommit end";
        } catch (DBException& e) {
            severe() << "dbexception in durThread causing immediate shutdown: " << e.toString();
            invariant(false);
        } catch (std::ios_base::failure& e) {
            severe() << "ios_base exception in durThread causing immediate shutdown: " << e.what();
            invariant(false);
        } catch (std::bad_alloc& e) {
            severe() << "bad_alloc exception in durThread causing immediate shutdown: " << e.what();
            invariant(false);
        } catch (std::exception& e) {
            severe() << "exception in durThread causing immediate shutdown: " << e.what();
            invariant(false);
        } catch (...) {
            severe() << "unhandled exception in durThread causing immediate shutdown";
            invariant(false);
        }
    }

    // Stops the journal thread and ensures everything was written
    invariant(!commitJob.hasWritten());

    journalWriter.flush();
    journalWriter.shutdown();

    log() << "Durability thread stopped";
}


/**
 * Invoked at server startup. Recovers the database by replaying journal files and then
 * starts the durability thread.
 */
void startup(ClockSource* cs, int64_t serverStartMs) {
    if (!storageGlobalParams.dur) {
        return;
    }

    journalMakeDir(cs, serverStartMs);

    try {
        replayJournalFilesAtStartup();
    } catch (DBException& e) {
        severe() << "dbexception during recovery: " << e.toString();
        throw;
    } catch (std::exception& e) {
        severe() << "std::exception during recovery: " << e.what();
        throw;
    } catch (...) {
        severe() << "exception during recovery";
        throw;
    }

    preallocateFiles();

    durableImpl.start(cs, serverStartMs);
    DurableInterface::_impl = &durableImpl;
}

void setJournalListener(JournalListener* jl) {
    stdx::unique_lock<stdx::mutex> lk(journalListenerMutex);
    journalListener = jl;
}

JournalListener* getJournalListener() {
    stdx::unique_lock<stdx::mutex> lk(journalListenerMutex);
    return journalListener;
}

}  // namespace dur
}  // namespace mongo
