// mmap_v1_engine.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fstream>

#include "mongo/db/mongod_options.h"
#include "mongo/db/storage/mmap_v1/data_file_sync.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"
#include "mongo/db/storage/mmap_v1/file_allocator.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"


namespace mongo {

using std::endl;
using std::ifstream;
using std::string;
using std::stringstream;
using std::vector;

MMAPV1Options mmapv1GlobalOptions;

namespace {

#if !defined(__sun)
// if doingRepair is true don't consider unclean shutdown an error
void checkForUncleanShutdown(MMAPV1Engine* storageEngine,
                             bool doingRepair,
                             const StorageEngineLockFile& lockFile) {
    string name = lockFile.getFilespec();
    bool oldFile = lockFile.createdByUncleanShutdown();

    if (oldFile) {
        // we check this here because we want to see if we can get the lock
        // if we can't, then its probably just another mongod running

        string errmsg;
        if (doingRepair && dur::haveJournalFiles()) {
            errmsg =
                "************** \n"
                "You specified --repair but there are dirty journal files. Please\n"
                "restart without --repair to allow the journal files to be replayed.\n"
                "If you wish to repair all databases, please shutdown cleanly and\n"
                "run with --repair again.\n"
                "**************";
        } else if (storageGlobalParams.dur) {
            if (!dur::haveJournalFiles(/*anyFiles=*/true)) {
                // Passing anyFiles=true as we are trying to protect against starting in an
                // unclean state with the journal directory unmounted. If there are any files,
                // even prealloc files, then it means that it is mounted so we can continue.
                // Previously there was an issue (SERVER-5056) where we would fail to start up
                // if killed during prealloc.

                vector<string> dbnames;
                storageEngine->listDatabases(&dbnames);

                if (dbnames.size() == 0) {
                    // this means that mongod crashed
                    // between initial startup and when journaling was initialized
                    // it is safe to continue
                } else {
                    errmsg = str::stream()
                        << "************** \n"
                        << "old lock file: " << name << ".  probably means unclean shutdown,\n"
                        << "but there are no journal files to recover.\n"
                        << "this is likely human error or filesystem corruption.\n"
                        << "please make sure that your journal directory is mounted.\n"
                        << "found " << dbnames.size() << " dbs.\n"
                        << "see: http://dochub.mongodb.org/core/repair for more information\n"
                        << "*************";
                }
            }
        } else {
            if (!dur::haveJournalFiles() && !doingRepair) {
                errmsg = str::stream() << "************** \n"
                                       << "Unclean shutdown detected.\n"
                                       << "Please visit http://dochub.mongodb.org/core/repair for "
                                          "recovery instructions.\n"
                                       << "*************";
            }
        }

        if (!errmsg.empty()) {
            log() << errmsg << endl;
            uassert(12596, "old lock file", 0);
        }
    }

    // Not related to lock file, but this is where we handle unclean shutdown
    if (!storageGlobalParams.dur && dur::haveJournalFiles()) {
        log() << "**************" << endl;
        log() << "Error: journal files are present in journal directory, yet starting without "
                 "journaling enabled."
              << endl;
        log() << "It is recommended that you start with journaling enabled so that recovery may "
                 "occur."
              << endl;
        log() << "**************" << endl;
        uasserted(13597, "can't start without --journal enabled when journal/ files are present");
    }
}
#else
void checkForUncleanShutdown(MMAPV1Engine* storageEngine,
                             bool doingRepair,
                             const StorageEngineLockFile& lockFile) {
    // TODO - this is very bad that the code above not running here.

    // Not related to lock file, but this is where we handle unclean shutdown
    if (!storageGlobalParams.dur && dur::haveJournalFiles()) {
        log() << "**************" << endl;
        log() << "Error: journal files are present in journal directory, yet starting without "
                 "--journal enabled."
              << endl;
        log() << "It is recommended that you start with journaling enabled so that recovery may "
                 "occur."
              << endl;
        log() << "Alternatively (not recommended), you can backup everything, then delete the "
                 "journal files, and run --repair"
              << endl;
        log() << "**************" << endl;
        uasserted(13618, "can't start without --journal enabled when journal/ files are present");
    }
}
#endif  //  !defined(__sun)


/// warn if readahead > 256KB (gridfs chunk size)
void checkReadAhead(const string& dir) {
#ifdef __linux__
    try {
        const dev_t dev = getPartition(dir);

        // This path handles the case where the filesystem uses the whole device (including LVM)
        string path = str::stream() << "/sys/dev/block/" << major(dev) << ':' << minor(dev)
                                    << "/queue/read_ahead_kb";

        if (!boost::filesystem::exists(path)) {
            // This path handles the case where the filesystem is on a partition.
            path =
                str::stream() << "/sys/dev/block/" << major(dev) << ':'
                              << minor(dev)  // this is a symlink
                              << "/.."  // parent directory of a partition is for the whole device
                              << "/queue/read_ahead_kb";
        }

        if (boost::filesystem::exists(path)) {
            ifstream file(path.c_str());
            if (file.is_open()) {
                int kb;
                file >> kb;
                if (kb > 256) {
                    log() << startupWarningsLog;

                    log() << "** WARNING: Readahead for " << dir << " is set to " << kb << "KB"
                          << startupWarningsLog;

                    log() << "**          We suggest setting it to 256KB (512 sectors) or less"
                          << startupWarningsLog;

                    log() << "**          http://dochub.mongodb.org/core/readahead"
                          << startupWarningsLog;
                }
            }
        }
    } catch (const std::exception& e) {
        log() << "unable to validate readahead settings due to error: " << e.what()
              << startupWarningsLog;
        log() << "for more information, see http://dochub.mongodb.org/core/readahead"
              << startupWarningsLog;
    }
#endif  // __linux__
}

// This is unrelated to the _tmp directory in dbpath.
void clearTmpFiles() {
    boost::filesystem::path path(storageGlobalParams.dbpath);
    for (boost::filesystem::directory_iterator i(path);
         i != boost::filesystem::directory_iterator();
         ++i) {
        string fileName = boost::filesystem::path(*i).leaf().string();
        if (boost::filesystem::is_directory(*i) && fileName.length() && fileName[0] == '$')
            boost::filesystem::remove_all(*i);
    }
}
}  // namespace

MMAPV1Engine::MMAPV1Engine(const StorageEngineLockFile* lockFile, ClockSource* cs)
    : MMAPV1Engine(lockFile, cs, stdx::make_unique<MmapV1ExtentManager::Factory>()) {}

MMAPV1Engine::MMAPV1Engine(const StorageEngineLockFile* lockFile,
                           ClockSource* cs,
                           std::unique_ptr<ExtentManager::Factory> extentManagerFactory)
    : _recordAccessTracker(cs),
      _extentManagerFactory(std::move(extentManagerFactory)),
      _clock(cs),
      _startMs(_clock->now().toMillisSinceEpoch()) {
    // TODO check non-journal subdirs if using directory-per-db
    checkReadAhead(storageGlobalParams.dbpath);

    if (!storageGlobalParams.readOnly) {
        invariant(lockFile);
        checkForUncleanShutdown(this, storageGlobalParams.repair, *lockFile);

        FileAllocator::get()->start();

        MONGO_ASSERT_ON_EXCEPTION_WITH_MSG(clearTmpFiles(), "clear tmp files");
    }
}

void MMAPV1Engine::finishInit() {
    dataFileSync.go();

    // Replays the journal (if needed) and starts the background thread. This requires the
    // ability to create OperationContexts.
    dur::startup(_clock, _startMs);
}

MMAPV1Engine::~MMAPV1Engine() {
    for (EntryMap::const_iterator it = _entryMap.begin(); it != _entryMap.end(); ++it) {
        delete it->second;
    }
    _entryMap.clear();
}

RecoveryUnit* MMAPV1Engine::newRecoveryUnit() {
    return new DurRecoveryUnit();
}

void MMAPV1Engine::listDatabases(std::vector<std::string>* out) const {
    _listDatabases(storageGlobalParams.dbpath, out);
}

DatabaseCatalogEntry* MMAPV1Engine::getDatabaseCatalogEntry(OperationContext* opCtx,
                                                            StringData db) {
    {
        stdx::lock_guard<stdx::mutex> lk(_entryMapMutex);
        EntryMap::const_iterator iter = _entryMap.find(db.toString());
        if (iter != _entryMap.end()) {
            return iter->second;
        }
    }

    // This is an on-demand database create/open. At this point, we are locked under X lock for
    // the database (MMAPV1DatabaseCatalogEntry's constructor checks that) so no two threads
    // can be creating the same database concurrenty. We need to create the database outside of
    // the _entryMapMutex so we do not deadlock (see SERVER-15880).
    MMAPV1DatabaseCatalogEntry* entry = new MMAPV1DatabaseCatalogEntry(
        opCtx,
        db,
        storageGlobalParams.dbpath,
        storageGlobalParams.directoryperdb,
        false,
        _extentManagerFactory->create(
            db, storageGlobalParams.dbpath, storageGlobalParams.directoryperdb));

    stdx::lock_guard<stdx::mutex> lk(_entryMapMutex);

    // Sanity check that we are not overwriting something
    invariant(_entryMap.insert(EntryMap::value_type(db.toString(), entry)).second);

    return entry;
}

Status MMAPV1Engine::closeDatabase(OperationContext* txn, StringData db) {
    // Before the files are closed, flush any potentially outstanding changes, which might
    // reference this database. Otherwise we will assert when subsequent applications of the
    // global journal entries occur, which happen to have write intents for the removed files.
    getDur().syncDataAndTruncateJournal(txn);

    stdx::lock_guard<stdx::mutex> lk(_entryMapMutex);
    MMAPV1DatabaseCatalogEntry* entry = _entryMap[db.toString()];
    delete entry;
    _entryMap.erase(db.toString());
    return Status::OK();
}

Status MMAPV1Engine::dropDatabase(OperationContext* txn, StringData db) {
    Status status = closeDatabase(txn, db);
    if (!status.isOK())
        return status;

    _deleteDataFiles(db.toString());

    return Status::OK();
}

void MMAPV1Engine::_listDatabases(const std::string& directory, std::vector<std::string>* out) {
    boost::filesystem::path path(directory);
    for (boost::filesystem::directory_iterator i(path);
         i != boost::filesystem::directory_iterator();
         ++i) {
        if (storageGlobalParams.directoryperdb) {
            boost::filesystem::path p = *i;
            string dbName = p.leaf().string();
            p /= (dbName + ".ns");
            if (exists(p))
                out->push_back(dbName);
        } else {
            string fileName = boost::filesystem::path(*i).leaf().string();
            if (fileName.length() > 3 && fileName.substr(fileName.length() - 3, 3) == ".ns")
                out->push_back(fileName.substr(0, fileName.length() - 3));
        }
    }
}

int MMAPV1Engine::flushAllFiles(bool sync) {
    return MongoFile::flushAll(sync);
}

Status MMAPV1Engine::beginBackup(OperationContext* txn) {
    return Status::OK();
}

void MMAPV1Engine::endBackup(OperationContext* txn) {
    return;
}

bool MMAPV1Engine::isDurable() const {
    return getDur().isDurable();
}

bool MMAPV1Engine::isEphemeral() const {
    return false;
}

RecordAccessTracker& MMAPV1Engine::getRecordAccessTracker() {
    return _recordAccessTracker;
}

void MMAPV1Engine::cleanShutdown() {
    // wait until file preallocation finishes
    // we would only hang here if the file_allocator code generates a
    // synchronous signal, which we don't expect
    log() << "shutdown: waiting for fs preallocator..." << endl;
    FileAllocator::get()->waitUntilFinished();

    if (storageGlobalParams.dur) {
        log() << "shutdown: final commit..." << endl;

        getDur().commitAndStopDurThread();
    }

    log() << "shutdown: closing all files..." << endl;
    stringstream ss3;
    MemoryMappedFile::closeAllFiles(ss3);
    log() << ss3.str() << endl;
}

void MMAPV1Engine::setJournalListener(JournalListener* jl) {
    dur::setJournalListener(jl);
}
}
