// heap1_engine.cpp

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

#include "mongo/db/storage/heap1/heap1_engine.h"

#include "mongo/db/storage/heap1/heap1_database_catalog_entry.h"
#include "mongo/db/storage/heap1/heap1_recovery_unit.h"

namespace mongo {

    Heap1Engine::~Heap1Engine() {
        for ( DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it ) {
            delete it->second;
        }
        _dbs.clear();
    }

    RecoveryUnit* Heap1Engine::newRecoveryUnit( OperationContext* opCtx ) {
        return new Heap1RecoveryUnit();
    }

    void Heap1Engine::listDatabases( std::vector<std::string>* out ) const {
        boost::mutex::scoped_lock lk( _dbLock );
        for ( DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it ) {
            out->push_back( it->first );
        }
    }

    DatabaseCatalogEntry* Heap1Engine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                const StringData& dbName ) {
        boost::mutex::scoped_lock lk( _dbLock );

        Heap1DatabaseCatalogEntry*& db = _dbs[dbName.toString()];
        if ( !db )
            db = new Heap1DatabaseCatalogEntry( dbName );
        return db;
    }

    Status Heap1Engine::closeDatabase(OperationContext* txn, const StringData& dbName ) {
        // no-op as not file handles
        return Status::OK();
    }

    Status Heap1Engine::dropDatabase(OperationContext* txn, const StringData& dbName ) {
        boost::mutex::scoped_lock lk( _dbLock );

        Heap1DatabaseCatalogEntry*& db = _dbs[dbName.toString()];
        delete db;
        _dbs.erase( dbName.toString() );

        return Status::OK();
    }
}
