// wiredtiger_engine.cpp

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

#include "mongo/db/storage/wiredtiger/wiredtiger_engine.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/regex.hpp>

#include "mongo/db/storage/wiredtiger/wiredtiger_database_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/db/storage_options.h"

namespace mongo {

    WiredTigerEngine::WiredTigerEngine( const std::string &path) : _path( path ) {
            int ret = wiredtiger_open(path.c_str(), NULL,
                 "create,extensions=[local=(entry=index_collator_extension)]", &_wt_conn);
            invariant(ret == 0);
    }

    RecoveryUnit* WiredTigerEngine::newRecoveryUnit( OperationContext* opCtx ) {
        return new WiredTigerRecoveryUnit();
    }

    void WiredTigerEngine::cleanShutdown( OperationContext* opCtx) {
        for ( DBMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
            delete i->second;
        } 
        int ret = _wt_conn->close(_wt_conn, NULL);
        invariant ( ret == 0 );
    }

    void WiredTigerEngine::listDatabases( std::vector<std::string>* out ) const {
        // TODO: invariant(storageGlobalParams.directoryperdb);

        for ( DBMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
            out->push_back( i->first );
        } 
    }

    Status WiredTigerEngine::closeDatabase(OperationContext*, const StringData& db ) {
        boost::mutex::scoped_lock lk( _dbLock );
        WiredTigerDatabaseCatalogEntry *entry = _dbs[db.toString()];
        delete entry;
        _dbs.erase( db.toString() );
        return Status::OK();
    }

    Status WiredTigerEngine::dropDatabase(OperationContext* txn, const StringData& db) {
        boost::mutex::scoped_lock lk( _dbLock );
        WiredTigerDatabaseCatalogEntry *entry = _dbs[db.toString()];
        if (entry == NULL)
            return Status::OK();
        entry->dropAllCollections(txn);
        delete entry;
        _dbs.erase( db.toString() );
        return Status::OK();
    }

    DatabaseCatalogEntry* WiredTigerEngine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                const StringData& dbName ) {
        boost::mutex::scoped_lock lk( _dbLock );

        WiredTigerDatabaseCatalogEntry*& db = _dbs[dbName.toString()];
        if ( !db ) {
            // The WiredTigerDatabase lifespan is currently tied to a
            // WiredTigerDatabaseCatalogEntry. In future we may want to split
            // that out
            WiredTigerDatabase *database = new WiredTigerDatabase(_wt_conn);
            db = new WiredTigerDatabaseCatalogEntry( dbName, *database );
            _dbs[dbName.toString()] = db;
        }
        return db;
    }

}
