// rocks_engine.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#pragma once

#include <list>
#include <string>

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/string_map.h"

namespace rocksdb {
    class ColumnFamilyHandle;
    struct ColumnFamilyOptions;
    class DB;
}

namespace mongo {

    class RocksCollectionCatalogEntry;
    class RocksRecordStore;

    struct CollectionOptions;

    /**
     * Since we have one DB for the entire server, the RocksEngine is going to hold all state.
     * DatabaseCatalogEntry will just be a thin slice over the engine.
     */
    class RocksEngine : public StorageEngine {
        MONGO_DISALLOW_COPYING( RocksEngine );
    public:
        RocksEngine( const std::string& path );
        virtual ~RocksEngine();

        virtual RecoveryUnit* newRecoveryUnit( OperationContext* opCtx );

        virtual void listDatabases( std::vector<std::string>* out ) const;

        virtual DatabaseCatalogEntry* getDatabaseCatalogEntry( OperationContext* opCtx,
                                                               const StringData& db );

        /**
         * @return number of files flushed
         */
        virtual int flushAllFiles( bool sync );

        virtual Status repairDatabase( OperationContext* tnx,
                                       const std::string& dbName,
                                       bool preserveClonedFilesOnFailure = false,
                                       bool backupOriginalFiles = false );

        // rocks specific api

        rocksdb::DB* getDB() { return _db; }
        const rocksdb::DB* getDB() const { return _db; }

        void getCollectionNamespaces( const StringData& dbName, std::list<std::string>* out ) const;

        Status createCollection( OperationContext* txn,
                                 const StringData& ns,
                                 const CollectionOptions& options );

        Status dropCollection( OperationContext* opCtx,
                               const StringData& ns );

        // will create if doesn't exist
        // collection has to exist first though
        rocksdb::ColumnFamilyHandle* getIndexColumnFamily( const StringData& ns,
                                                           const StringData& indexName );

        struct Entry {
            boost::scoped_ptr<rocksdb::ColumnFamilyHandle> cfHandle;
            boost::scoped_ptr<RocksCollectionCatalogEntry> collectionEntry;
            boost::scoped_ptr<RocksRecordStore> recordStore;
            StringMap< boost::shared_ptr<rocksdb::ColumnFamilyHandle> > indexNameToCF;
        };

        Entry* getEntry( const StringData& ns );
        const Entry* getEntry( const StringData& ns ) const;

    private:

        rocksdb::ColumnFamilyOptions _collectionOptions() const;
        rocksdb::ColumnFamilyOptions _indexOptions() const;

        std::string _path;
        rocksdb::DB* _db;

        typedef StringMap< boost::shared_ptr<Entry> > Map;
        mutable boost::mutex _mapLock;
        Map _map;

    };
}
