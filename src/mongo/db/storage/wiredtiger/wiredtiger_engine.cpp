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

#include "mongo/db/storage/wiredtiger/wiredtiger_database_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/db/storage_options.h"

namespace mongo {

    RecoveryUnit* WiredTigerEngine::newRecoveryUnit( OperationContext* opCtx ) {
        return new WiredTigerRecoveryUnit();
    }

    void WiredTigerEngine::listDatabases( std::vector<std::string>* out ) const {
        boost::mutex::scoped_lock lk( _dbLock );
        for ( DBMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i ) {
            out->push_back( *i );
        }
    }

    Status WiredTigerEngine::closeDatabase(OperationContext*, const StringData& db ) {
        boost::mutex::scoped_lock lk( _dbLock );
        _dbs.erase( db.toString() );
        return Status::OK();
    }

    Status WiredTigerEngine::dropDatabase(OperationContext* txn, const StringData& db) {
        invariant (storageGlobalParams.directoryperdb);
        Status status = closeDatabase( txn, db );
        if ( !status.isOK() )
            return status;

        // Remove the database files. Code borrowed from mmap deleteDataFiles method.
        MONGO_ASSERT_ON_EXCEPTION_WITH_MSG(
            boost::filesystem::remove_all(
                boost::filesystem::path(storageGlobalParams.dbpath) / db.toString()),
                    "delete database files");

        return Status::OK();
    }

    DatabaseCatalogEntry* WiredTigerEngine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                const StringData& dbName ) {
        boost::mutex::scoped_lock lk( _dbLock );

        // THIS is temporary I think
        _dbs.insert( dbName.toString() );
        return new WiredTigerDatabaseCatalogEntry( dbName, _db );
        /*
        WiredTigerDatabaseCatalogEntry*& db = _dbs[dbName.toString()];
        if ( !db )
            db = new WiredTigerDatabaseCatalogEntry( dbName );
        return db;
        */
    }

}
