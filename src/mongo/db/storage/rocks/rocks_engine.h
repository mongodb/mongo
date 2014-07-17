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

#include "mongo/bson/ordering.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/string_map.h"

namespace rocksdb {
    class ColumnFamilyHandle;
    struct ColumnFamilyDescriptor;
    struct ColumnFamilyOptions;
    class DB;
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

        // This executes a shutdown in rocks, so this rocksdb must not be used after this call
        virtual void cleanShutdown(OperationContext* txn);

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
        rocksdb::ColumnFamilyHandle* getIndexColumnFamily(
                              const StringData& ns,
                              const StringData& indexName,
                              const boost::optional<Ordering> order = boost::optional<Ordering>() );
        /**
         * Completely removes a column family. Input pointer is invalid after calling
         */
        void removeColumnFamily( rocksdb::ColumnFamilyHandle*& cfh, const StringData& indexName,
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
            // These ColumnFamilyHandle must be deleted by removeIndex
            StringMap< rocksdb::ColumnFamilyHandle* > indexNameToCF;
        };

        Entry* getEntry( const StringData& ns );
        const Entry* getEntry( const StringData& ns ) const;

        typedef std::vector<boost::shared_ptr<Entry> > EntryVector;
        typedef std::vector<rocksdb::ColumnFamilyDescriptor> CfdVector;

    private:

        rocksdb::Options _dbOptions() const;
        rocksdb::ColumnFamilyOptions _collectionOptions() const;
        rocksdb::ColumnFamilyOptions _indexOptions() const;

        std::string _path;
        rocksdb::DB* _db;

        typedef StringMap< boost::shared_ptr<Entry> > Map;
        mutable boost::mutex _mapLock;
        Map _map;

        // private methods that should only be called from the RocksEngine constructor
        
        /**
         * Create Entry's for all non-index column families. See larger comment in .cpp for why
         * this is necessary
         */
        EntryVector _createNonIndexCatalogEntries( const std::vector<std::string>& families );

        /**
         * Generate column family descriptors for the metadata column family corresponding to each
         * Entry in entries. We need to open these first because the metadata column families
         * contain the information needed to open the column families representing indexes.
         */
        CfdVector _generateMetaDataCfds( const EntryVector& entries,
                                         const std::vector<string>& nsVec ) const;

        /**
         * Helper function to the _createIndexOrderings method
         */
        std::map<string, Ordering> _createIndexOrderingsHelper(
                const std::vector<std::string>& namespaces );

        /**
         * Highest level helper function to generate a map from index names to Orderings. This is
         * needed to properly open column families representing indexes, because an Ordering for
         * each such index is needed in order to create the comparator for the column family.
         *
         * @families a vector containing the name of every column family in the database
         */
        std::map<string, Ordering> _createIndexOrderings( const std::vector<string>& namespaces,
                                                          const string& path,
                                                          rocksdb::DB* const db );

        /**
         * Highest level helper function to generate a vector of all ColumnFamilyDescriptors
         * in the database, with proper rocksdb::Options set for each of them.
         */
        CfdVector _createCfds ( const string& path, rocksdb::DB* const db);        

        /**
         * Create a complete Entry object in _map for every ColumnFamilyDescriptor
         */
        void _createEntries( const CfdVector& families, 
                             const std::vector<rocksdb::ColumnFamilyHandle*> handles );
    };
}
