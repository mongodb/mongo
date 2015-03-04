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
#include <boost/scoped_ptr.hpp>

#include <rocksdb/cache.h>
#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/convenience.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_global_options.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/rocks/rocks_index.h"
#include "mongo/platform/endian.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

#define ROCKS_TRACE log()

#define ROCKS_STATUS_OK( s ) if ( !( s ).ok() ) { error() << "rocks error: " << ( s ).ToString(); \
    invariant( false ); }

namespace mongo {

    namespace {
        // we encode prefixes in big endian because we want to quickly jump to the max prefix
        // (iter->SeekToLast())
        bool extractPrefix(const rocksdb::Slice& slice, uint32_t* prefix) {
            if (slice.size() < sizeof(uint32_t)) {
                return false;
            }
            *prefix = endian::bigToNative(*reinterpret_cast<const uint32_t*>(slice.data()));
            return true;
        }

        std::string encodePrefix(uint32_t prefix) {
            uint32_t bigEndianPrefix = endian::nativeToBig(prefix);
            return std::string(reinterpret_cast<const char*>(&bigEndianPrefix), sizeof(uint32_t));
        }
    }  // anonymous namespace

    // first four bytes are the default prefix 0
    const std::string RocksEngine::kMetadataPrefix("\0\0\0\0metadata-", 12);

    RocksEngine::RocksEngine(const std::string& path, bool durable)
        : _path(path), _durable(durable) {
        {  // create block cache
            uint64_t cacheSizeGB = rocksGlobalOptions.cacheSizeGB;
            if (cacheSizeGB == 0) {
                ProcessInfo pi;
                unsigned long long memSizeMB = pi.getMemSizeMB();
                if (memSizeMB > 0) {
                    double cacheMB = memSizeMB / 2;
                    cacheSizeGB = static_cast<uint64_t>(cacheMB / 1024);
                }
                if (cacheSizeGB < 1) {
                    cacheSizeGB = 1;
                }
            }
            _block_cache = rocksdb::NewLRUCache(cacheSizeGB * 1024 * 1024 * 1024LL);
        }
        // open DB
        rocksdb::DB* db;
        auto s = rocksdb::DB::Open(_options(), path, &db);
        ROCKS_STATUS_OK(s);
        _db.reset(db);

        // open iterator
        boost::scoped_ptr<rocksdb::Iterator> _iter(_db->NewIterator(rocksdb::ReadOptions()));

        // find maxPrefix
        _maxPrefix = 0;
        _iter->SeekToLast();
        if (_iter->Valid()) {
            // otherwise the DB is empty, so we just keep it at 0
            bool ok = extractPrefix(_iter->key(), &_maxPrefix);
            // this is DB corruption here
            invariant(ok);
        }

        // load ident to prefix map
        {
            boost::lock_guard<boost::mutex> lk(_identPrefixMapMutex);
            for (_iter->Seek(kMetadataPrefix);
                 _iter->Valid() && _iter->key().starts_with(kMetadataPrefix); _iter->Next()) {
                rocksdb::Slice ident(_iter->key());
                ident.remove_prefix(kMetadataPrefix.size());
                // this could throw DBException, which then means DB corruption. We just let it fly
                // to the caller
                BSONObj identConfig(_iter->value().data());
                BSONElement element = identConfig.getField("prefix");
                // TODO: SERVER-16979 Correctly handle errors returned by RocksDB
                // This is DB corruption
                invariant(!element.eoo() || !element.isNumber());
                uint32_t identPrefix = static_cast<uint32_t>(element.numberInt());
                _identPrefixMap[StringData(ident.data(), ident.size())] = identPrefix;
            }
        }
    }

    RocksEngine::~RocksEngine() {}

    RecoveryUnit* RocksEngine::newRecoveryUnit() {
        return new RocksRecoveryUnit(&_transactionEngine, _db.get(), _durable);
    }

    Status RocksEngine::createRecordStore(OperationContext* opCtx, StringData ns, StringData ident,
                                          const CollectionOptions& options) {
        return _createIdentPrefix(ident);
    }

    RecordStore* RocksEngine::getRecordStore(OperationContext* opCtx, StringData ns,
                                             StringData ident, const CollectionOptions& options) {
        if (options.capped) {
            return new RocksRecordStore(
                ns, ident, _db.get(), _getIdentPrefix(ident), true,
                options.cappedSize ? options.cappedSize : 4096,  // default size
                options.cappedMaxDocs ? options.cappedMaxDocs : -1);
        } else {
            return new RocksRecordStore(ns, ident, _db.get(), _getIdentPrefix(ident));
        }
    }

    Status RocksEngine::createSortedDataInterface(OperationContext* opCtx, StringData ident,
                                                  const IndexDescriptor* desc) {
        return _createIdentPrefix(ident);
    }

    SortedDataInterface* RocksEngine::getSortedDataInterface(OperationContext* opCtx,
                                                             StringData ident,
                                                             const IndexDescriptor* desc) {
        if (desc->unique()) {
            return new RocksUniqueIndex(_db.get(), _getIdentPrefix(ident), ident.toString(),
                                        Ordering::make(desc->keyPattern()));
        } else {
            return new RocksStandardIndex(_db.get(), _getIdentPrefix(ident), ident.toString(),
                                          Ordering::make(desc->keyPattern()));
        }
    }

    Status RocksEngine::dropIdent(OperationContext* opCtx, StringData ident) {
        // TODO optimize this using CompactionFilterV2
        rocksdb::WriteBatch wb;
        wb.Delete(kMetadataPrefix + ident.toString());

        std::string prefix = _getIdentPrefix(ident);
        rocksdb::Slice prefixSlice(prefix.data(), prefix.size());

        boost::scoped_ptr<rocksdb::Iterator> _iter(_db->NewIterator(rocksdb::ReadOptions()));
        for (_iter->Seek(prefixSlice); _iter->Valid() && _iter->key().starts_with(prefixSlice);
             _iter->Next()) {
            ROCKS_STATUS_OK(_iter->status());
            wb.Delete(_iter->key());
        }
        auto s = _db->Write(rocksdb::WriteOptions(), &wb);
        if (!s.ok()) {
          return toMongoStatus(s);
        }

        {
            boost::lock_guard<boost::mutex> lk(_identPrefixMapMutex);
            _identPrefixMap.erase(ident);
        }

        return Status::OK();
    }

    bool RocksEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
        boost::lock_guard<boost::mutex> lk(_identPrefixMapMutex);
        return _identPrefixMap.find(ident) != _identPrefixMap.end();
    }

    std::vector<std::string> RocksEngine::getAllIdents(OperationContext* opCtx) const {
        std::vector<std::string> indents;
        for (auto& entry : _identPrefixMap) {
            indents.push_back(entry.first);
        }
        return indents;
    }

    // non public api
    Status RocksEngine::_createIdentPrefix(StringData ident) {
        uint32_t prefix = 0;
        {
            boost::lock_guard<boost::mutex> lk(_identPrefixMapMutex);
            if (_identPrefixMap.find(ident) != _identPrefixMap.end()) {
                // already exists
                return Status::OK();
            }

            prefix = ++_maxPrefix;
            _identPrefixMap[ident] = prefix;
        }

        BSONObjBuilder builder;
        builder.append("prefix", static_cast<int32_t>(prefix));
        BSONObj config = builder.obj();

        auto s = _db->Put(rocksdb::WriteOptions(), kMetadataPrefix + ident.toString(),
                          rocksdb::Slice(config.objdata(), config.objsize()));

        return toMongoStatus(s);
    }

    std::string RocksEngine::_getIdentPrefix(StringData ident) {
        boost::lock_guard<boost::mutex> lk(_identPrefixMapMutex);
        auto prefixIter = _identPrefixMap.find(ident);
        invariant(prefixIter != _identPrefixMap.end());
        return encodePrefix(prefixIter->second);
    }

    rocksdb::Options RocksEngine::_options() const {
        // default options
        rocksdb::Options options;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = _block_cache;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        table_options.format_version = 2;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        options.write_buffer_size = 128 * 1024 * 1024;  // 128MB
        options.max_write_buffer_number = 4;
        options.max_background_compactions = 8;
        options.max_background_flushes = 4;
        options.target_file_size_base = 64 * 1024 * 1024; // 64MB
        options.soft_rate_limit = 2;
        options.hard_rate_limit = 3;
        options.max_bytes_for_level_base = 512 * 1024 * 1024;  // 512 MB
        options.max_open_files = 20000;

        if (rocksGlobalOptions.compression == "snappy") {
            options.compression = rocksdb::kSnappyCompression;
        } else if (rocksGlobalOptions.compression == "zlib") {
            options.compression = rocksdb::kZlibCompression;
        } else if (rocksGlobalOptions.compression == "none") {
            options.compression = rocksdb::kNoCompression;
        } else {
            log() << "Unknown compression, will use default (snappy)";
        }

        // create the DB if it's not already present
        options.create_if_missing = true;
        options.wal_dir = _path + "/journal";

        // allow override
        if (!rocksGlobalOptions.configString.empty()) {
            rocksdb::Options base_options(options);
            rocksdb::GetOptionsFromString(base_options, rocksGlobalOptions.configString, &options);
        }

        return options;
    }

    Status toMongoStatus( rocksdb::Status s ) {
        if ( s.ok() )
            return Status::OK();
        else
            return Status( ErrorCodes::InternalError, s.ToString() );
    }
}
