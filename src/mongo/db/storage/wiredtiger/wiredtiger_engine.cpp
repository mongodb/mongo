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

#include "mongo/db/storage/wiredtiger/wiredtiger_database_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/db/storage_options.h"

namespace mongo {

    RecoveryUnit* WiredTigerEngine::newRecoveryUnit( OperationContext* opCtx ) {
        return new WiredTigerRecoveryUnit();
    }

    void WiredTigerEngine::cleanShutdown( OperationContext* opCtx) {
        for ( DBMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
            delete i->second;
        } 
    }

    void WiredTigerEngine::listDatabases( std::vector<std::string>* out ) const {
        // TODO: invariant(storageGlobalParams.directoryperdb);

        // TODO: Save this information into a map, don't keep hitting disk.
        boost::filesystem::path path( storageGlobalParams.dbpath );
        for ( boost::filesystem::directory_iterator i( path );
            i != boost::filesystem::directory_iterator();
            ++i) {
            boost::filesystem::path p = *i;
            string dbName = p.leaf().string();
            p /= "WiredTiger.wt";
            if ( exists ( p ) )
                    out->push_back( dbName );
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
        // TODO: invariant(storageGlobalParams.directoryperdb);
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

        WiredTigerDatabaseCatalogEntry*& db = _dbs[dbName.toString()];
        if ( !db ) {
            boost::filesystem::path dbpath =
                boost::filesystem::path(storageGlobalParams.dbpath) / dbName.toString();
            // Ensure that the database directory exists
            if ( !exists( dbpath ) )
                MONGO_ASSERT_ON_EXCEPTION(boost::filesystem::create_directory(dbpath));
            WT_CONNECTION *conn;
            int ret = wiredtiger_open(dbpath.string().c_str(), NULL, "create,extensions=[local=(entry=index_collator_extension)]", &conn);
            invariant(ret == 0);
            // The WiredTigerDatabase lifespan is currently tied to a
            // WiredTigerDatabaseCatalogEntry. In future we may want to split
            // that out
            WiredTigerDatabase *database = new WiredTigerDatabase(conn);
            db = new WiredTigerDatabaseCatalogEntry( dbName, *database );
            _dbs[dbName.toString()] = db;
        }
        return db;
    }

}
