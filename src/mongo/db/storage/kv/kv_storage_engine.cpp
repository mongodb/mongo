// kv_storage_engine.cpp

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

#include "mongo/db/storage/kv/kv_storage_engine.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace {
        const std::string catalogInfo = "_mdb_catalog";
    }

    class KVStorageEngine::RemoveDBChange : public RecoveryUnit::Change {
    public:
        RemoveDBChange(KVStorageEngine* engine, const StringData& db, KVDatabaseCatalogEntry* entry)
            : _engine(engine)
            , _db(db.toString())
            , _entry(entry)
        {}

        virtual void commit() {
            delete _entry;
        }

        virtual void rollback() {
            boost::mutex::scoped_lock lk(_engine->_dbsLock);
            _engine->_dbs[_db] = _entry;
        }

        KVStorageEngine* const _engine;
        const std::string _db;
        KVDatabaseCatalogEntry* const _entry;
    };

    KVStorageEngine::KVStorageEngine( KVEngine* engine )
        : _engine( engine )
        , _initialized( false )
        , _supportsDocLocking(_engine->supportsDocLocking()) {
    }

    void KVStorageEngine::cleanShutdown(OperationContext* txn) {

        for ( DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it ) {
            delete it->second;
        }
        _dbs.clear();

        _catalog.reset( NULL );
        _catalogRecordStore.reset( NULL );

        _engine->cleanShutdown(txn);
        // intentionally not deleting _engine
    }

    KVStorageEngine::~KVStorageEngine() {
    }

    void KVStorageEngine::finishInit() {
        if ( _initialized )
            return;

        OperationContextNoop opCtx( _engine->newRecoveryUnit() );
        WriteUnitOfWork uow( &opCtx );

        Status status = _engine->createRecordStore( &opCtx,
                                                    catalogInfo,
                                                    catalogInfo,
                                                    CollectionOptions() );
        fassert( 28520, status );

        _catalogRecordStore.reset( _engine->getRecordStore( &opCtx,
                                                            catalogInfo,
                                                            catalogInfo,
                                                            CollectionOptions() ) );
        _catalog.reset( new KVCatalog( _catalogRecordStore.get(), _supportsDocLocking ) );
        _catalog->init( &opCtx );

        std::vector<std::string> collections;
        _catalog->getAllCollections( &collections );

        for ( size_t i = 0; i < collections.size(); i++ ) {
            std::string coll = collections[i];
            NamespaceString nss( coll );
            string dbName = nss.db().toString();

            // No rollback since this is only for committed dbs.
            KVDatabaseCatalogEntry*& db = _dbs[dbName];
            if ( !db ) {
                db = new KVDatabaseCatalogEntry( dbName, this );
            }
            db->initCollection( &opCtx, coll );
        }

        uow.commit();

        _initialized = true;
    }

    RecoveryUnit* KVStorageEngine::newRecoveryUnit( OperationContext* opCtx ) {
        invariant( _initialized );
        if ( !_engine ) {
            // shutdown
            return NULL;
        }
        return _engine->newRecoveryUnit();
    }

    void KVStorageEngine::listDatabases( std::vector<std::string>* out ) const {
        invariant( _initialized );
        boost::mutex::scoped_lock lk( _dbsLock );
        for ( DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it ) {
            if ( it->second->isEmpty() )
                continue;
            out->push_back( it->first );
        }
    }

    DatabaseCatalogEntry* KVStorageEngine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                    const StringData& dbName ) {
        invariant( _initialized );
        boost::mutex::scoped_lock lk( _dbsLock );
        KVDatabaseCatalogEntry*& db = _dbs[dbName.toString()];
        if ( !db ) {
            // Not registering change since db creation is implicit and never rolled back.
            db = new KVDatabaseCatalogEntry( dbName, this );
        }
        return db;
    }

    Status KVStorageEngine::closeDatabase( OperationContext* txn, const StringData& db ) {
        invariant( _initialized );
        // This is ok to be a no-op as there is no database layer in kv.
        return Status::OK();
    }

    Status KVStorageEngine::dropDatabase( OperationContext* txn, const StringData& db ) {
        invariant( _initialized );

        KVDatabaseCatalogEntry* entry;
        {
            boost::mutex::scoped_lock lk( _dbsLock );
            DBMap::const_iterator it = _dbs.find( db.toString() );
            if ( it == _dbs.end() )
                return Status( ErrorCodes::NamespaceNotFound, "db not found to drop" );
            entry = it->second;
        }

        // This is called outside of a WUOW since MMAPv1 has unfortunate behavior around dropping
        // databases. We need to create one here since we want db dropping to all-or-nothing
        // wherever possible. Eventually we want to move this up so that it can include the logOp
        // inside of the WUOW, but that would require making DB dropping happen inside the Dur
        // system for MMAPv1.
        WriteUnitOfWork wuow(txn);

        std::list<std::string> toDrop;
        entry->getCollectionNamespaces( &toDrop );

        for ( std::list<std::string>::iterator it = toDrop.begin(); it != toDrop.end(); ++it ) {
            string coll = *it;
            entry->dropCollection( txn, coll );
        }
        toDrop.clear();
        entry->getCollectionNamespaces( &toDrop );
        invariant( toDrop.empty() );

        {
            boost::mutex::scoped_lock lk( _dbsLock );
            txn->recoveryUnit()->registerChange(new RemoveDBChange(this, db, entry));
            _dbs.erase( db.toString() );
        }

        wuow.commit();
        return Status::OK();
    }

    int KVStorageEngine::flushAllFiles( bool sync ) {
        return _engine->flushAllFiles( sync );
    }

    bool KVStorageEngine::isDurable() const {
        return _engine->isDurable();
    }

    Status KVStorageEngine::repairDatabase( OperationContext* txn,
                                            const std::string& dbName,
                                            bool preserveClonedFilesOnFailure,
                                            bool backupOriginalFiles ) {
        if ( preserveClonedFilesOnFailure ) {
            return Status( ErrorCodes::BadValue, "preserveClonedFilesOnFailure not supported" );
        }
        if ( backupOriginalFiles ) {
            return Status( ErrorCodes::BadValue, "backupOriginalFiles not supported" );
        }

        vector<string> idents = _catalog->getAllIdentsForDB( dbName );
        for ( size_t i = 0; i < idents.size(); i++ ) {
            Status status = _engine->repairIdent( txn, idents[i] );
            if ( !status.isOK() )
                return status;
        }
        return Status::OK();
    }

}
