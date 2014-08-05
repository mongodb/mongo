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

#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>

#if defined(_WIN32)
#include <io.h>
#else
#include <sys/file.h>
#endif

#include "mongo/db/mongod_options.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"
#include "mongo/db/storage_options.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/log.h"
#include "mongo/util/mmap.h"

namespace mongo {

namespace {
#ifdef _WIN32
    HANDLE lockFileHandle;
#endif

    // This is used by everyone, including windows.
    int lockFile = 0;

#if !defined(__sunos__)
    void writePid(int fd) {
        stringstream ss;
        ss << ProcessId::getCurrent() << endl;
        string s = ss.str();
        const char * data = s.c_str();
#ifdef _WIN32
        verify( _write( fd, data, strlen( data ) ) );
#else
        verify( write( fd, data, strlen( data ) ) );
#endif
    }

    // if doingRepair is true don't consider unclean shutdown an error
    void acquirePathLock(MMAPV1Engine* storageEngine, bool doingRepair) {
        string name = (boost::filesystem::path(storageGlobalParams.dbpath) / "mongod.lock").string();

        bool oldFile = false;

        if ( boost::filesystem::exists( name ) && boost::filesystem::file_size( name ) > 0 ) {
            oldFile = true;
        }

#ifdef _WIN32
        lockFileHandle = CreateFileA( name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0 /* do not allow anyone else access */, NULL, 
            OPEN_ALWAYS /* success if fh can open */, 0, NULL );

        if (lockFileHandle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            char *msg;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&msg, 0, NULL);
            string m = msg;
            str::stripTrailing(m, "\r\n");
            uasserted(ErrorCodes::DBPathInUse,
                      str::stream() << "Unable to create/open lock file: "
                                    << name << ' ' << m
                                    << ". Is a mongod instance already running?");
        }
        lockFile = _open_osfhandle((intptr_t)lockFileHandle, 0);
#else
        lockFile = open( name.c_str(), O_RDWR | O_CREAT , S_IRWXU | S_IRWXG | S_IRWXO );
        if( lockFile <= 0 ) {
            uasserted( ErrorCodes::DBPathInUse,
                       str::stream() << "Unable to create/open lock file: "
                                     << name << ' ' << errnoWithDescription()
                                     << " Is a mongod instance already running?" );
        }
        if (flock( lockFile, LOCK_EX | LOCK_NB ) != 0) {
            close ( lockFile );
            lockFile = 0;
            uasserted(ErrorCodes::DBPathInUse,
                      "Unable to lock file: " + name + ". Is a mongod instance already running?");
        }
#endif

        if ( oldFile ) {
            // we check this here because we want to see if we can get the lock
            // if we can't, then its probably just another mongod running
            
            string errmsg;
            if (doingRepair && dur::haveJournalFiles()) {
                errmsg = "************** \n"
                         "You specified --repair but there are dirty journal files. Please\n"
                         "restart without --repair to allow the journal files to be replayed.\n"
                         "If you wish to repair all databases, please shutdown cleanly and\n"
                         "run with --repair again.\n"
                         "**************";
            }
            else if (storageGlobalParams.dur) {
                if (!dur::haveJournalFiles(/*anyFiles=*/true)) {
                    // Passing anyFiles=true as we are trying to protect against starting in an
                    // unclean state with the journal directory unmounted. If there are any files,
                    // even prealloc files, then it means that it is mounted so we can continue.
                    // Previously there was an issue (SERVER-5056) where we would fail to start up
                    // if killed during prealloc.

                    vector<string> dbnames;
                    storageEngine->listDatabases( &dbnames );

                    if ( dbnames.size() == 0 ) {
                        // this means that mongod crashed
                        // between initial startup and when journaling was initialized
                        // it is safe to continue
                    }
                    else {
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
            }
            else {
                if (!dur::haveJournalFiles() && !doingRepair) {
                    errmsg = str::stream()
                             << "************** \n"
                             << "Unclean shutdown detected.\n"
                             << "Please visit http://dochub.mongodb.org/core/repair for recovery instructions.\n"
                             << "*************";
                }
            }

            if (!errmsg.empty()) {
                log() << errmsg << endl;
#ifdef _WIN32
                CloseHandle( lockFileHandle );
#else
                close ( lockFile );
#endif
                lockFile = 0;
                uassert( 12596 , "old lock file" , 0 );
            }
        }

        // Not related to lock file, but this is where we handle unclean shutdown
        if (!storageGlobalParams.dur && dur::haveJournalFiles()) {
            log() << "**************" << endl;
            log() << "Error: journal files are present in journal directory, yet starting without journaling enabled." << endl;
            log() << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            log() << "**************" << endl;
            uasserted(13597, "can't start without --journal enabled when journal/ files are present");
        }

#ifdef _WIN32
        uassert( 13625, "Unable to truncate lock file", _chsize(lockFile, 0) == 0);
        writePid( lockFile );
        _commit( lockFile );
#else
        uassert( 13342, "Unable to truncate lock file", ftruncate(lockFile, 0) == 0);
        writePid( lockFile );
        fsync( lockFile );
        flushMyDirectory(name);
#endif
    }
#else
    void acquirePathLock(MMAPV1Engine* storageEngine, bool) {
        // TODO - this is very bad that the code above not running here.

        // Not related to lock file, but this is where we handle unclean shutdown
        if (!storageGlobalParams.dur && dur::haveJournalFiles()) {
            log() << "**************" << endl;
            log() << "Error: journal files are present in journal directory, yet starting without --journal enabled." << endl;
            log() << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            log() << "Alternatively (not recommended), you can backup everything, then delete the journal files, and run --repair" << endl;
            log() << "**************" << endl;
            uasserted(13618, "can't start without --journal enabled when journal/ files are present");
        }
    }
#endif


    /// warn if readahead > 256KB (gridfs chunk size)
    void checkReadAhead(const string& dir) {
#ifdef __linux__
        try {
            const dev_t dev = getPartition(dir);

            // This path handles the case where the filesystem uses the whole device (including LVM)
            string path = str::stream() <<
                "/sys/dev/block/" << major(dev) << ':' << minor(dev) << "/queue/read_ahead_kb";

            if (!boost::filesystem::exists(path)){
                // This path handles the case where the filesystem is on a partition.
                path = str::stream()
                    << "/sys/dev/block/" << major(dev) << ':' << minor(dev) // this is a symlink
                    << "/.." // parent directory of a partition is for the whole device
                    << "/queue/read_ahead_kb";
            }

            if (boost::filesystem::exists(path)) {
                ifstream file (path.c_str());
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
        }
        catch (const std::exception& e) {
            log() << "unable to validate readahead settings due to error: " << e.what()
                  << startupWarningsLog;
            log() << "for more information, see http://dochub.mongodb.org/core/readahead"
                  << startupWarningsLog;
        }
#endif // __linux__
    }

    // This is unrelated to the _tmp directory in dbpath.
    void clearTmpFiles() {
        boost::filesystem::path path(storageGlobalParams.dbpath);
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            string fileName = boost::filesystem::path(*i).leaf().string();
            if ( boost::filesystem::is_directory( *i ) &&
                    fileName.length() && fileName[ 0 ] == '$' )
                boost::filesystem::remove_all( *i );
        }
    }
} // namespace

    MMAPV1Engine::MMAPV1Engine() {
        // TODO check non-journal subdirs if using directory-per-db
        checkReadAhead(storageGlobalParams.dbpath);

        acquirePathLock(this, storageGlobalParams.repair);

        FileAllocator::get()->start();

        MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( clearTmpFiles(), "clear tmp files" );

        // dur::startup() depends on globalStorageEngine being set before calling.
        // TODO clean up dur::startup() so this isn't needed.
        invariant(!globalStorageEngine);
        globalStorageEngine = this;

        dur::startup();
    }

    MMAPV1Engine::~MMAPV1Engine() {
        for ( EntryMap::const_iterator it = _entryMap.begin(); it != _entryMap.end(); ++it ) {
            delete it->second;
        }
        _entryMap.clear();
    }

    RecoveryUnit* MMAPV1Engine::newRecoveryUnit( OperationContext* opCtx ) {
        return new DurRecoveryUnit( opCtx );
    }

    void MMAPV1Engine::listDatabases( std::vector<std::string>* out ) const {
        _listDatabases( storageGlobalParams.dbpath, out );
    }

    DatabaseCatalogEntry* MMAPV1Engine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                 const StringData& db ) {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        MMAPV1DatabaseCatalogEntry*& entry = _entryMap[db.toString()];
        if ( !entry ) {
            entry =  new MMAPV1DatabaseCatalogEntry( opCtx,
                                                     db,
                                                     storageGlobalParams.dbpath,
                                                     storageGlobalParams.directoryperdb,
                                                     false );
        }
        return entry;
    }

    Status MMAPV1Engine::closeDatabase( OperationContext* txn, const StringData& db ) {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        MMAPV1DatabaseCatalogEntry* entry = _entryMap[db.toString()];
        delete entry;
        _entryMap.erase( db.toString() );
        return Status::OK();
    }

    Status MMAPV1Engine::dropDatabase( OperationContext* txn, const StringData& db ) {
        Status status = closeDatabase( txn, db );
        if ( !status.isOK() )
            return status;

        _deleteDataFiles( db.toString() );

        return Status::OK();
    }

    void MMAPV1Engine::_listDatabases( const std::string& directory,
                                       std::vector<std::string>* out ) {
        boost::filesystem::path path( directory );
        for ( boost::filesystem::directory_iterator i( path );
              i != boost::filesystem::directory_iterator();
              ++i ) {
            if (storageGlobalParams.directoryperdb) {
                boost::filesystem::path p = *i;
                string dbName = p.leaf().string();
                p /= ( dbName + ".ns" );
                if ( exists( p ) )
                    out->push_back( dbName );
            }
            else {
                string fileName = boost::filesystem::path(*i).leaf().string();
                if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                    out->push_back( fileName.substr( 0, fileName.length() - 3 ) );
            }
        }
    }

    int MMAPV1Engine::flushAllFiles( bool sync ) {
        return MongoFile::flushAll( sync );
    }

    void MMAPV1Engine::cleanShutdown(OperationContext* txn) {
        // wait until file preallocation finishes
        // we would only hang here if the file_allocator code generates a
        // synchronous signal, which we don't expect
        log() << "shutdown: waiting for fs preallocator..." << endl;
        FileAllocator::get()->waitUntilFinished();

        if (storageGlobalParams.dur) {
            log() << "shutdown: final commit..." << endl;
            getDur().commitNow(txn);

            flushAllFiles(true);
        }

        log() << "shutdown: closing all files..." << endl;
        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        log() << ss3.str() << endl;

        if (storageGlobalParams.dur) {
            dur::journalCleanup(true);
        }

#if !defined(__sunos__)
        if ( lockFile ) {
            log() << "shutdown: removing fs lock..." << endl;
            /* This ought to be an unlink(), but Eliot says the last
               time that was attempted, there was a race condition
               with acquirePathLock().  */
#ifdef _WIN32
            if( _chsize( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription(_doserrno) << endl;
            CloseHandle(lockFileHandle);
#else
            if( ftruncate( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription() << endl;
            flock( lockFile, LOCK_UN );
#endif
        }
#endif
    }
}
