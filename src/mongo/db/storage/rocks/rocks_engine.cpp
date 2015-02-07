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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/rocks/rocks_engine.h"

#include <boost/filesystem/operations.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>

#include <rocksdb/cache.h>
#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/rocks/rocks_index.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

#define ROCKS_TRACE log()

#define ROCKS_STATUS_OK( s ) if ( !( s ).ok() ) { error() << "rocks error: " << ( s ).ToString(); \
    invariant( false ); }

namespace mongo {

    using boost::shared_ptr;

    const std::string RocksEngine::kOrderingPrefix("indexordering-");
    const std::string RocksEngine::kCollectionPrefix("collection-");

    RocksEngine::RocksEngine(const std::string& path, bool durable)
        : _path(path),
          _durable(durable) {

        { // create block cache
            uint64_t cacheSizeGB = 0;
            ProcessInfo pi;
            unsigned long long memSizeMB = pi.getMemSizeMB();
            if (memSizeMB > 0) {
                double cacheMB = memSizeMB / 2;
                cacheSizeGB = static_cast<uint64_t>(cacheMB / 1024);
            }
            if (cacheSizeGB < 1) {
                cacheSizeGB = 1;
            }
            _block_cache = rocksdb::NewLRUCache(cacheSizeGB * 1024 * 1024 * 1024LL);
        }

        auto columnFamilyNames = _loadColumnFamilies();       // vector of column family names
        std::unordered_map<std::string, Ordering> orderings;  // column family name -> Ordering
        std::set<std::string> collections;                    // set of collection names

        if (columnFamilyNames.empty()) {  // new DB
            columnFamilyNames.push_back(rocksdb::kDefaultColumnFamilyName);
        } else {  // existing DB
            // open DB in read-only mode to load metadata
            rocksdb::DB* dbReadOnly;
            auto s = rocksdb::DB::OpenForReadOnly(_dbOptions(), path, &dbReadOnly);
            ROCKS_STATUS_OK(s);
            auto itr = dbReadOnly->NewIterator(rocksdb::ReadOptions());
            orderings = _loadOrderingMetaData(itr);
            collections = _loadCollections(itr);
            delete itr;
            delete dbReadOnly;
        }

        std::vector<rocksdb::ColumnFamilyDescriptor> columnFamilies;
        std::set<std::string> toDropColumnFamily;

        for (const auto& cf : columnFamilyNames) {
            if (cf == rocksdb::kDefaultColumnFamilyName) {
                columnFamilies.emplace_back(cf, _defaultCFOptions());
                continue;
            }
            auto orderings_iter = orderings.find(cf);
            auto collections_iter = collections.find(cf);
            bool isIndex = orderings_iter != orderings.end();
            bool isCollection = collections_iter != collections.end();
            invariant(!isIndex || !isCollection);
            if (isIndex) {
                columnFamilies.emplace_back(cf, _indexOptions(orderings_iter->second));
            } else if (isCollection) {
                columnFamilies.emplace_back(cf, _collectionOptions());
            } else {
                // TODO support this from inside of rocksdb, by using
                // Options::drop_unopened_column_families.
                // This can happen because write and createColumnFamily are not atomic
                toDropColumnFamily.insert(cf);
                columnFamilies.emplace_back(cf, _collectionOptions());
            }
        }

        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        rocksdb::DB* db;
        auto s = rocksdb::DB::Open(_dbOptions(), path, columnFamilies, &handles, &db);
        ROCKS_STATUS_OK(s);
        invariant(handles.size() == columnFamilies.size());
        for (size_t i = 0; i < handles.size(); ++i) {
            if (toDropColumnFamily.find(columnFamilies[i].name) != toDropColumnFamily.end()) {
                db->DropColumnFamily(handles[i]);
                delete handles[i];
            } else if (columnFamilyNames[i] == rocksdb::kDefaultColumnFamilyName) {
                // we will not be needing this
                delete handles[i];
            } else {
                _identColumnFamilyMap[columnFamilies[i].name].reset(handles[i]);
            }
        }
        _db.reset(db);
    }

    RocksEngine::~RocksEngine() {}

    RecoveryUnit* RocksEngine::newRecoveryUnit() {
        return new RocksRecoveryUnit(&_transactionEngine, _db.get(), _durable);
    }

    Status RocksEngine::createRecordStore(OperationContext* opCtx,
                                          StringData ns,
                                          StringData ident,
                                          const CollectionOptions& options) {
        if (_existsColumnFamily(ident)) {
            return Status::OK();
        }
        _db->Put(rocksdb::WriteOptions(), kCollectionPrefix + ident.toString(), rocksdb::Slice());
        return _createColumnFamily(_collectionOptions(), ident);
    }

    RecordStore* RocksEngine::getRecordStore(OperationContext* opCtx, StringData ns,
                                             StringData ident,
                                             const CollectionOptions& options) {
        auto columnFamily = _getColumnFamily(ident);
        if (options.capped) {
            return new RocksRecordStore(
                ns, ident, _db.get(), columnFamily, true,
                options.cappedSize ? options.cappedSize : 4096,  // default size
                options.cappedMaxDocs ? options.cappedMaxDocs : -1);
        } else {
            return new RocksRecordStore(ns, ident, _db.get(), columnFamily);
        }
    }

    Status RocksEngine::createSortedDataInterface(OperationContext* opCtx, StringData ident,
                                                  const IndexDescriptor* desc) {
        if (_existsColumnFamily(ident)) {
            return Status::OK();
        }
        auto keyPattern = desc->keyPattern();

        _db->Put(rocksdb::WriteOptions(), kOrderingPrefix + ident.toString(),
                 rocksdb::Slice(keyPattern.objdata(), keyPattern.objsize()));
        return _createColumnFamily(_indexOptions(Ordering::make(keyPattern)), ident);
    }

    SortedDataInterface* RocksEngine::getSortedDataInterface(OperationContext* opCtx,
                                                             StringData ident,
                                                             const IndexDescriptor* desc) {
        if (desc->unique()) {
            return new RocksUniqueIndex(_db.get(), _getColumnFamily(ident), ident.toString(),
                                        Ordering::make(desc->keyPattern()));
        } else {
            return new RocksStandardIndex(_db.get(), _getColumnFamily(ident), ident.toString(),
                                          Ordering::make(desc->keyPattern()));
        }
    }

    Status RocksEngine::dropIdent(OperationContext* opCtx, StringData ident) {
        rocksdb::WriteBatch wb;
        // TODO is there a more efficient way?
        wb.Delete(kOrderingPrefix + ident.toString());
        wb.Delete(kCollectionPrefix + ident.toString());
        auto s = _db->Write(rocksdb::WriteOptions(), &wb);
        if (!s.ok()) {
            return toMongoStatus(s);
        }
        return _dropColumnFamily(ident);
    }

    std::vector<std::string> RocksEngine::getAllIdents( OperationContext* opCtx ) const {
        std::vector<std::string> indents;
        for (auto& entry : _identColumnFamilyMap) {
            indents.push_back(entry.first);
        }
        return indents;
    }

    // non public api

    bool RocksEngine::_existsColumnFamily(StringData ident) {
        boost::mutex::scoped_lock lk(_identColumnFamilyMapMutex);
        return _identColumnFamilyMap.find(ident) != _identColumnFamilyMap.end();
    }

    Status RocksEngine::_createColumnFamily(const rocksdb::ColumnFamilyOptions& options,
                                            StringData ident) {
        rocksdb::ColumnFamilyHandle* cf;
        auto s = _db->CreateColumnFamily(options, ident.toString(), &cf);
        if (!s.ok()) {
            return toMongoStatus(s);
        }
        boost::mutex::scoped_lock lk(_identColumnFamilyMapMutex);
        _identColumnFamilyMap[ident].reset(cf);
        return Status::OK();
    }

    Status RocksEngine::_dropColumnFamily(StringData ident) {
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily;
        {
            boost::mutex::scoped_lock lk(_identColumnFamilyMapMutex);
            auto cf_iter = _identColumnFamilyMap.find(ident);
            if (cf_iter == _identColumnFamilyMap.end()) {
                return Status(ErrorCodes::InternalError, "Not found");
            }
            columnFamily = cf_iter->second;
            _identColumnFamilyMap.erase(cf_iter);
        }
        auto s = _db->DropColumnFamily(columnFamily.get());
        return toMongoStatus(s);
    }

    boost::shared_ptr<rocksdb::ColumnFamilyHandle> RocksEngine::_getColumnFamily(
        StringData ident) {
        {
            boost::mutex::scoped_lock lk(_identColumnFamilyMapMutex);
            auto cf_iter = _identColumnFamilyMap.find(ident);
            invariant(cf_iter != _identColumnFamilyMap.end());
            return cf_iter->second;
        }
    }

    std::unordered_map<std::string, Ordering> RocksEngine::_loadOrderingMetaData(
        rocksdb::Iterator* itr) {
        std::unordered_map<std::string, Ordering> orderings;
        for (itr->Seek(kOrderingPrefix); itr->Valid(); itr->Next()) {
            rocksdb::Slice key(itr->key());
            if (!key.starts_with(kOrderingPrefix)) {
                break;
            }
            key.remove_prefix(kOrderingPrefix.size());
            std::string value(itr->value().ToString());
            orderings.insert({key.ToString(), Ordering::make(BSONObj(value.c_str()))});
        }
        ROCKS_STATUS_OK(itr->status());
        return orderings;
    }

    std::set<std::string> RocksEngine::_loadCollections(rocksdb::Iterator* itr) {
        std::set<std::string> collections;
        for (itr->Seek(kCollectionPrefix); itr->Valid() ; itr->Next()) {
            rocksdb::Slice key(itr->key());
            if (!key.starts_with(kCollectionPrefix)) {
                break;
            }
            key.remove_prefix(kCollectionPrefix.size());
            collections.insert(key.ToString());
        }
        ROCKS_STATUS_OK(itr->status());
        return collections;
    }

    std::vector<std::string> RocksEngine::_loadColumnFamilies() {
        std::vector<std::string> names;
        if (boost::filesystem::exists(_path)) {
            rocksdb::Status s = rocksdb::DB::ListColumnFamilies(_dbOptions(), _path, &names);

            if (s.IsIOError()) {
                // DNE, this means the directory exists but is empty, which is fine
                // because it means no rocks database exists yet
            } else {
                ROCKS_STATUS_OK(s);
            }
        }

        return names;
    }

    rocksdb::Options RocksEngine::_dbOptions() {
        rocksdb::Options options(rocksdb::DBOptions(), _defaultCFOptions());

        options.max_background_compactions = 4;
        options.max_background_flushes = 4;

        // create the DB if it's not already present
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        options.wal_dir = _path + "/journal";
        options.max_total_wal_size = 1 << 30;  // 1GB

        return options;
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_defaultCFOptions() {
        // TODO pass or set appropriate options for default CF.
        rocksdb::ColumnFamilyOptions options;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = _block_cache;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
        options.max_write_buffer_number = 4;
        return options;
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_collectionOptions() {
        return _defaultCFOptions();
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_indexOptions(const Ordering& order) {
        return _defaultCFOptions();
    }

    Status toMongoStatus( rocksdb::Status s ) {
        if ( s.ok() )
            return Status::OK();
        else
            return Status( ErrorCodes::InternalError, s.ToString() );
    }
}
