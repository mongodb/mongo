// repair_database.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"
#include "mongo/util/file.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/log.h"
#include "mongo/util/mmap.h"

namespace mongo {

    typedef boost::filesystem::path Path;

    // inheritable class to implement an operation that may be applied to all
    // files in a database using _applyOpToDataFiles()
    class FileOp {
    public:
        virtual ~FileOp() {}
        // Return true if file exists and operation successful
        virtual bool apply( const boost::filesystem::path &p ) = 0;
        virtual const char * op() const = 0;
    };

    void _applyOpToDataFiles(const string& database, FileOp &fo, bool afterAllocator = false,
                             const string& path = storageGlobalParams.dbpath);

    void _deleteDataFiles(const std::string& database) {
        if (storageGlobalParams.directoryperdb) {
            FileAllocator::get()->waitUntilFinished();
            MONGO_ASSERT_ON_EXCEPTION_WITH_MSG(
                    boost::filesystem::remove_all(
                        boost::filesystem::path(storageGlobalParams.dbpath) / database),
                                                "delete data files with a directoryperdb");
            return;
        }
        class : public FileOp {
            virtual bool apply( const boost::filesystem::path &p ) {
                return boost::filesystem::remove( p );
            }
            virtual const char * op() const {
                return "remove";
            }
        } deleter;
        _applyOpToDataFiles( database, deleter, true );
    }

    void boostRenameWrapper( const Path &from, const Path &to ) {
        try {
            boost::filesystem::rename( from, to );
        }
        catch ( const boost::filesystem::filesystem_error & ) {
            // boost rename doesn't work across partitions
            boost::filesystem::copy_file( from, to);
            boost::filesystem::remove( from );
        }
    }

    // back up original database files to 'temp' dir
    void _renameForBackup( const std::string& database, const Path &reservedPath ) {
        Path newPath( reservedPath );
        if (storageGlobalParams.directoryperdb)
            newPath /= database;
        class Renamer : public FileOp {
        public:
            Renamer( const Path &newPath ) : newPath_( newPath ) {}
        private:
            const boost::filesystem::path &newPath_;
            virtual bool apply( const Path &p ) {
                if ( !boost::filesystem::exists( p ) )
                    return false;
                boostRenameWrapper( p, newPath_ / ( p.leaf().string() + ".bak" ) );
                return true;
            }
            virtual const char * op() const {
                return "renaming";
            }
        } renamer( newPath );
        _applyOpToDataFiles( database, renamer, true );
    }

    intmax_t dbSize( const string& database ) {
        class SizeAccumulator : public FileOp {
        public:
            SizeAccumulator() : totalSize_( 0 ) {}
            intmax_t size() const {
                return totalSize_;
            }
        private:
            virtual bool apply( const boost::filesystem::path &p ) {
                if ( !boost::filesystem::exists( p ) )
                    return false;
                totalSize_ += boost::filesystem::file_size( p );
                return true;
            }
            virtual const char *op() const {
                return "checking size";
            }
            intmax_t totalSize_;
        };
        SizeAccumulator sa;
        _applyOpToDataFiles( database, sa );
        return sa.size();
    }

    // move temp files to standard data dir
    void _replaceWithRecovered( const string& database, const char *reservedPathString ) {
        Path newPath(storageGlobalParams.dbpath);
        if (storageGlobalParams.directoryperdb)
            newPath /= database;
        class Replacer : public FileOp {
        public:
            Replacer( const Path &newPath ) : newPath_( newPath ) {}
        private:
            const boost::filesystem::path &newPath_;
            virtual bool apply( const Path &p ) {
                if ( !boost::filesystem::exists( p ) )
                    return false;
                boostRenameWrapper( p, newPath_ / p.leaf() );
                return true;
            }
            virtual const char * op() const {
                return "renaming";
            }
        } replacer( newPath );
        _applyOpToDataFiles( database, replacer, true, reservedPathString );
    }

    // generate a directory name for storing temp data files
    Path uniqueReservedPath( const char *prefix ) {
        Path repairPath = Path(storageGlobalParams.repairpath);
        Path reservedPath;
        int i = 0;
        bool exists = false;
        do {
            stringstream ss;
            ss << prefix << "_repairDatabase_" << i++;
            reservedPath = repairPath / ss.str();
            MONGO_ASSERT_ON_EXCEPTION( exists = boost::filesystem::exists( reservedPath ) );
        }
        while ( exists );
        return reservedPath;
    }

    void _applyOpToDataFiles( const string& database, FileOp &fo, bool afterAllocator, const string& path ) {
        if ( afterAllocator )
            FileAllocator::get()->waitUntilFinished();
        string c = database;
        c += '.';
        boost::filesystem::path p(path);
        if (storageGlobalParams.directoryperdb)
            p /= database;
        boost::filesystem::path q;
        q = p / (c+"ns");
        bool ok = false;
        MONGO_ASSERT_ON_EXCEPTION( ok = fo.apply( q ) );
        if ( ok ) {
            LOG(2) << fo.op() << " file " << q.string() << endl;
        }
        int i = 0;
        int extra = 10; // should not be necessary, this is defensive in case there are missing files
        while ( 1 ) {
            verify( i <= DiskLoc::MaxFiles );
            stringstream ss;
            ss << c << i;
            q = p / ss.str();
            MONGO_ASSERT_ON_EXCEPTION( ok = fo.apply(q) );
            if ( ok ) {
                if ( extra != 10 ) {
                    LOG(1) << fo.op() << " file " << q.string() << endl;
                    log() << "  _applyOpToDataFiles() warning: extra == " << extra << endl;
                }
            }
            else if ( --extra <= 0 )
                break;
            i++;
        }
    }

    class RepairFileDeleter {
    public:
        RepairFileDeleter( OperationContext* txn,
                           const string& dbName,
                           const string& pathString,
                           const Path& path )
            : _txn(txn),
              _dbName( dbName ),
              _pathString( pathString ),
              _path( path ),
              _success( false ) {
        }

        ~RepairFileDeleter() {
            if ( _success )
                 return;

            log() << "cleaning up failed repair "
                  << "db: " << _dbName << " path: " << _pathString;

            try {
                _txn->recoveryUnit()->syncDataAndTruncateJournal();

                globalStorageEngine->flushAllFiles(true); // need both in case journaling is disabled

                MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::remove_all( _path ) );
            }
            catch ( DBException& e ) {
                error() << "RepairFileDeleter failed to cleanup: " << e;
                error() << "aborting";
                fassertFailed( 17402 );
            }
        }

        void success() {
            _success = true;
        }

    private:
        OperationContext* _txn;
        string _dbName;
        string _pathString;
        Path _path;
        bool _success;
    };

    Status MMAPV1Engine::repairDatabase( OperationContext* txn,
                                         const std::string& dbName,
                                         bool preserveClonedFilesOnFailure,
                                         bool backupOriginalFiles ) {
        // We must hold some form of lock here
        invariant(txn->lockState()->threadState());
        invariant( dbName.find( '.' ) == string::npos );

        scoped_ptr<RepairFileDeleter> repairFileDeleter;

        log() << "repairDatabase " << dbName << endl;

        BackgroundOperation::assertNoBgOpInProgForDb(dbName);

        txn->recoveryUnit()->syncDataAndTruncateJournal(); // Must be done before and after repair

        intmax_t totalSize = dbSize( dbName );
        intmax_t freeSize = File::freeSpace(storageGlobalParams.repairpath);

        if ( freeSize > -1 && freeSize < totalSize ) {
            return Status( ErrorCodes::OutOfDiskSpace,
                           str::stream() << "Cannot repair database " << dbName
                           << " having size: " << totalSize
                           << " (bytes) because free disk space is: " << freeSize << " (bytes)" );
        }

        txn->checkForInterrupt();

        Path reservedPath =
            uniqueReservedPath( ( preserveClonedFilesOnFailure || backupOriginalFiles ) ?
                                "backup" : "_tmp" );
        MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::create_directory( reservedPath ) );
        string reservedPathString = reservedPath.string();

        if ( !preserveClonedFilesOnFailure )
            repairFileDeleter.reset( new RepairFileDeleter( txn,
                                                            dbName,
                                                            reservedPathString,
                                                            reservedPath ) );

        {
            Database* originalDatabase =
                            dbHolder().get(txn, dbName);
            if (originalDatabase == NULL) {
                return Status(ErrorCodes::NamespaceNotFound, "database does not exist to repair");
            }

            scoped_ptr<MMAPV1DatabaseCatalogEntry> dbEntry;
            scoped_ptr<Database> tempDatabase;
            {
                dbEntry.reset( new MMAPV1DatabaseCatalogEntry( txn,
                                                               dbName,
                                                               reservedPathString,
                                                               storageGlobalParams.directoryperdb,
                                                               true ) );
                invariant( !dbEntry->exists() );
                tempDatabase.reset( new Database( txn,
                                                  dbName,
                                                  dbEntry.get() ) );

            }

            map<string,CollectionOptions> namespacesToCopy;
            {
                string ns = dbName + ".system.namespaces";
                Client::Context ctx(txn,  ns );
                Collection* coll = originalDatabase->getCollection( txn, ns );
                if ( coll ) {
                    scoped_ptr<RecordIterator> it( coll->getIterator( txn,
                                                                      DiskLoc(),
                                                                      false,
                                                                      CollectionScanParams::FORWARD ) );
                    while ( !it->isEOF() ) {
                        DiskLoc loc = it->getNext();
                        BSONObj obj = coll->docFor( loc );

                        string ns = obj["name"].String();

                        NamespaceString nss( ns );
                        if ( nss.isSystem() ) {
                            if ( nss.isSystemDotIndexes() )
                                continue;
                            if ( nss.coll() == "system.namespaces" )
                                continue;
                        }

                        if ( !nss.isNormal() )
                            continue;

                        CollectionOptions options;
                        if ( obj["options"].isABSONObj() ) {
                            Status status = options.parse( obj["options"].Obj() );
                            if ( !status.isOK() )
                                return status;
                        }
                        namespacesToCopy[ns] = options;
                    }
                }
            }

            for ( map<string,CollectionOptions>::const_iterator i = namespacesToCopy.begin();
                  i != namespacesToCopy.end();
                  ++i ) {
                string ns = i->first;
                CollectionOptions options = i->second;

                Collection* tempCollection = NULL;
                {
                    Client::Context tempContext(txn, ns, tempDatabase );
                    WriteUnitOfWork wunit(txn->recoveryUnit());
                    tempCollection = tempDatabase->createCollection(txn, ns, options, true, false);
                    wunit.commit();
                }

                Client::Context readContext(txn, ns, originalDatabase);
                Collection* originalCollection = originalDatabase->getCollection( txn, ns );
                invariant( originalCollection );

                // data

                // TODO SERVER-14812 add a mode that drops duplicates rather than failing
                MultiIndexBlock indexer(txn, tempCollection );
                {
                    vector<BSONObj> indexes;
                    IndexCatalog::IndexIterator ii =
                        originalCollection->getIndexCatalog()->getIndexIterator( false );
                    while ( ii.more() ) {
                        IndexDescriptor* desc = ii.next();
                        indexes.push_back( desc->infoObj() );
                    }

                    Client::Context tempContext(txn, ns, tempDatabase);
                    Status status = indexer.init( indexes );
                    if ( !status.isOK() )
                        return status;
                }

                scoped_ptr<RecordIterator> iterator(
                    originalCollection->getIterator( txn, DiskLoc(), false,
                                                     CollectionScanParams::FORWARD ));
                while ( !iterator->isEOF() ) {
                    DiskLoc loc = iterator->getNext();
                    invariant( !loc.isNull() );

                    BSONObj doc = originalCollection->docFor( loc );

                    Client::Context tempContext(txn, ns, tempDatabase);
                    
                    WriteUnitOfWork wunit(txn->recoveryUnit());
                    StatusWith<DiskLoc> result = tempCollection->insertDocument(txn,
                                                                                doc,
                                                                                &indexer,
                                                                                false);
                    if ( !result.isOK() )
                        return result.getStatus();

                    wunit.commit();
                    txn->checkForInterrupt(false);
                }
                
                Status status = indexer.doneInserting();
                if (!status.isOK())
                    return status;

                {
                    Client::Context tempContext(txn, ns, tempDatabase);
                    WriteUnitOfWork wunit(txn->recoveryUnit());
                    indexer.commit();
                    wunit.commit();
                }

            }

            txn->recoveryUnit()->syncDataAndTruncateJournal();
            globalStorageEngine->flushAllFiles(true); // need both in case journaling is disabled

            txn->checkForInterrupt(false);
        }

        // at this point if we abort, we don't want to delete new files
        // as they might be the only copies

        if ( repairFileDeleter.get() )
            repairFileDeleter->success();

        dbHolder().close( txn, dbName );

        if ( backupOriginalFiles ) {
            _renameForBackup( dbName, reservedPath );
        }
        else {
            // first make new directory before deleting data
            Path newDir = Path(storageGlobalParams.dbpath) / dbName;
            MONGO_ASSERT_ON_EXCEPTION(boost::filesystem::create_directory(newDir));

            // this deletes old files
            _deleteDataFiles( dbName );

            if ( !boost::filesystem::exists(newDir) ) {
                // we deleted because of directoryperdb
                // re-create
                MONGO_ASSERT_ON_EXCEPTION(boost::filesystem::create_directory(newDir));
            }
        }

        _replaceWithRecovered( dbName, reservedPathString.c_str() );

        if ( !backupOriginalFiles )
            MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::remove_all( reservedPath ) );

        return Status::OK();
    }


}
