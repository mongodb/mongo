// wiredtiger_engine.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_database.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_metadata.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"

#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerEngine::WiredTigerEngine( const std::string &path) : _path( path ), _db(0) {
        WT_CONNECTION *conn;
        const char * default_config = "create,cache_size=1G,extensions=[local=(entry=index_collator_extension)],";

        std::string config = std::string(
                default_config + wiredTigerGlobalOptions.databaseConfig);
        int ret = wiredtiger_open(path.c_str(), NULL, config.c_str(), &conn);
        if (ret != 0) {
            log() << "Starting engine with custom options ( " << config <<
                     ") failed. Using default options instead." << endl;
            ret = wiredtiger_open(path.c_str(), NULL, default_config, &conn);
        }
        invariantWTOK(ret);
        _db = new WiredTigerDatabase(conn);
        _db->InitMetaData();
        loadExistingDatabases();
    }

    void WiredTigerEngine::cleanShutdown( OperationContext* txn ) {
        for ( DBMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
            delete i->second;
        } 
        delete _db;
        _db = 0;
    }

    void WiredTigerEngine::loadExistingDatabases() {
        boost::mutex::scoped_lock lk( _dbLock );

        _db->DropDeletedTables();

        WiredTigerMetaData &md = _db->GetMetaData();
        std::vector<uint64_t> tables = md.getAllTables();
        for ( std::vector<uint64_t>::iterator it = tables.begin(); it != tables.end(); ++it) {
            // The database name is all up to the first period
            std::string dbName = md.getName( *it );
            dbName = dbName.substr( 0, dbName.find('.') );
            // We've seen it already.
            if (_dbs[dbName])
                continue;

            _dbs[dbName] = new WiredTigerDatabaseCatalogEntry( *_db, dbName );
        }

    }

    void WiredTigerEngine::listDatabases( std::vector<std::string>* out ) const {
        for ( DBMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
            out->push_back( i->first );
        } 
    }

    Status WiredTigerEngine::closeDatabase(OperationContext*, const StringData& dbName) {
        boost::mutex::scoped_lock lk( _dbLock );
        DBMap::iterator i = _dbs.find(dbName.toString());
        if (i == _dbs.end())
            return Status::OK();
        WiredTigerDatabaseCatalogEntry *entry = i->second;
        delete entry;
        _dbs.erase( i );
        return Status::OK();
    }

    Status WiredTigerEngine::dropDatabase(OperationContext* txn, const StringData& dbName) {
        WiredTigerDatabaseCatalogEntry *entry;
        boost::mutex::scoped_lock lk( _dbLock );
        DBMap::iterator i = _dbs.find(dbName.toString());
        /* Open if not found: even closed databases need to be dropped. */
        if (i == _dbs.end())
            entry = new WiredTigerDatabaseCatalogEntry( *_db, dbName );
        else {
            entry = i->second;
            _dbs.erase( i );
        }
        Status s = entry->dropAllCollections(txn);
        delete entry;
        return s;
    }

    DatabaseCatalogEntry* WiredTigerEngine::getDatabaseCatalogEntry( OperationContext* txn,
                                                                const StringData& dbName ) {
        boost::mutex::scoped_lock lk( _dbLock );

        DBMap::const_iterator i = _dbs.find(dbName.toString());
        if (i != _dbs.end())
            return i->second;
        WiredTigerDatabaseCatalogEntry *entry = new WiredTigerDatabaseCatalogEntry( *_db, dbName );
        _dbs[dbName.toString()] = entry;
        return entry;
    }

    RecoveryUnit* WiredTigerEngine::newRecoveryUnit( OperationContext* txn ) {
        invariant(_db);
        return new WiredTigerRecoveryUnit(*_db, true);
    }
}
