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
#include <map>
#include <string>

#include <boost/optional.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include <rocksdb/status.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/string_map.h"

namespace rocksdb {
    class ColumnFamilyHandle;
    struct ColumnFamilyDescriptor;
    struct ColumnFamilyOptions;
    class DB;
    class Comparator;
    struct Options;
    struct ReadOptions;
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

        virtual void cleanShutdown(OperationContext* txn);

        virtual Status closeDatabase( OperationContext* txn, const StringData& db );

        virtual Status dropDatabase( OperationContext* txn, const StringData& db );

        // rocks specific api

        rocksdb::DB* getDB() { return _db.get(); }
        const rocksdb::DB* getDB() const { return _db.get(); }

        void getCollectionNamespaces( const StringData& dbName, std::list<std::string>* out ) const;

        Status createCollection( OperationContext* txn,
                                 const StringData& ns,
                                 const CollectionOptions& options );

        Status dropCollection( OperationContext* opCtx, const StringData& ns );

        /**
         * Will create if doesn't exist. The collection has to exist first, though.
         * The ordering argument is only used if the column family does not exist. If the
         * column family does not exist, then ordering must be a non-empty optional, containing
         * the Ordering for the index.
         */
        rocksdb::ColumnFamilyHandle* getIndexColumnFamily(
                              const StringData& ns,
                              const StringData& indexName,
                              const boost::optional<Ordering> order = boost::none );
        /**
         * Completely removes a column family. Input pointer is invalid after calling
         */
        void removeColumnFamily( rocksdb::ColumnFamilyHandle** cfh,
                                 const StringData& indexName,
                                 const StringData& ns );

        /**
         * Returns a ReadOptions object that uses the snapshot contained in opCtx
         */
        static rocksdb::ReadOptions readOptionsWithSnapshot( OperationContext* opCtx );

        struct Entry {
            boost::scoped_ptr<rocksdb::ColumnFamilyHandle> cfHandle;
            boost::scoped_ptr<rocksdb::ColumnFamilyHandle> metaCfHandle;
            boost::scoped_ptr<RocksCollectionCatalogEntry> collectionEntry;
            boost::scoped_ptr<RocksRecordStore> recordStore;
            // These ColumnFamilyHandles must be deleted by removeIndex
            StringMap<boost::shared_ptr<rocksdb::ColumnFamilyHandle>> indexNameToCF;
            StringMap<boost::shared_ptr<const rocksdb::Comparator>> indexNameToComparator;
        };

        Entry* getEntry( const StringData& ns );
        const Entry* getEntry( const StringData& ns ) const;

        typedef std::vector<rocksdb::ColumnFamilyDescriptor> CfdVector;

        static rocksdb::Options dbOptions();

    private:
        rocksdb::ColumnFamilyOptions _collectionOptions() const;
        rocksdb::ColumnFamilyOptions _indexOptions() const;

        std::string _path;
        boost::scoped_ptr<rocksdb::DB> _db;
        boost::scoped_ptr<rocksdb::Comparator> _collectionComparator;

        typedef StringMap< boost::shared_ptr<Entry> > EntryMap;
        mutable boost::mutex _entryMapMutex;
        EntryMap _entryMap;

        // private methods that should usually only be called from the RocksEngine constructor

        // RocksDB necessitates opening a default column family. This method exists to identify
        // that column family so that it can be ignored.
        bool _isDefaultFamily( const string& name );

        // See larger comment in .cpp for why this is necessary
        void _createNonIndexCatalogEntries( const std::vector<std::string>& families );

        /**
         * Return a vector containing the name of every column family in the database
         */
        std::vector<std::string> _listFamilyNames( std::string filepath );

        /**
         * @param namespaces a vector containing all the namespaces in this database.
         * @param metaDataCfds a vector of the column family descriptors for every column family
         * in the database representing metadata.
         */
        std::map<std::string, Ordering> _createIndexOrderings(
                const std::vector<string>& namespaces,
                const std::string& filepath );

        /**
         * @param namespaces a vector containing all the namespaces in this database
         */
        CfdVector _createCfds ( const std::vector<std::string>& namespaces,
                                const std::map<std::string, Ordering>& indexOrderings );

        /**
         * Create a complete Entry object in _entryMap for every ColumnFamilyDescriptor. Assumes
         * that if the collectionEntry field should be initialized, that is already has been prior
         * to this function call.
         *
         * @param families A vector of column family descriptors for every column family in the
         * database
         * @param handles A vector of column family handles for every column family in the
         * database
         */
        void _createEntries( const CfdVector& families,
                             const std::vector<rocksdb::ColumnFamilyHandle*> handles );
    };

    Status toMongoStatus( rocksdb::Status s );
}
