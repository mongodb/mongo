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

#include "rocks_engine.h"

#include <algorithm>

#include <boost/filesystem/operations.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>

#include <rocksdb/cache.h>
#include <rocksdb/compaction_filter.h>
#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/convenience.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/utilities/write_batch_with_index.h>
#include <rocksdb/utilities/checkpoint.h>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/endian.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

#include "rocks_counter_manager.h"
#include "rocks_global_options.h"
#include "rocks_record_store.h"
#include "rocks_recovery_unit.h"
#include "rocks_index.h"
#include "rocks_util.h"

#define ROCKS_TRACE log()

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

        class PrefixDeletingCompactionFilter : public rocksdb::CompactionFilter {
        public:
            explicit PrefixDeletingCompactionFilter(std::unordered_set<uint32_t> droppedPrefixes)
                : _droppedPrefixes(std::move(droppedPrefixes)),
                  _prefixCache(0),
                  _droppedCache(false) {}

            // filter is not called from multiple threads simultaneously
            virtual bool Filter(int level, const rocksdb::Slice& key,
                                const rocksdb::Slice& existing_value, std::string* new_value,
                                bool* value_changed) const {
                uint32_t prefix = 0;
                if (!extractPrefix(key, &prefix)) {
                    // this means there is a key in the database that's shorter than 4 bytes. this
                    // should never happen and this is a corruption. however, it's not compaction
                    // filter's job to report corruption, so we just silently continue
                    return false;
                }
                if (prefix == _prefixCache) {
                    return _droppedCache;
                }
                _prefixCache = prefix;
                _droppedCache = _droppedPrefixes.find(prefix) != _droppedPrefixes.end();
                return _droppedCache;
            }

            virtual const char* Name() const { return "PrefixDeletingCompactionFilter"; }

        private:
            std::unordered_set<uint32_t> _droppedPrefixes;
            mutable uint32_t _prefixCache;
            mutable bool _droppedCache;
        };

        class PrefixDeletingCompactionFilterFactory : public rocksdb::CompactionFilterFactory {
        public:
            explicit PrefixDeletingCompactionFilterFactory(const RocksEngine* engine) : _engine(engine) {}

            virtual std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
                const rocksdb::CompactionFilter::Context& context) override {
                auto droppedPrefixes = _engine->getDroppedPrefixes();
                if (droppedPrefixes.size() == 0) {
                    // no compaction filter needed
                    return std::unique_ptr<rocksdb::CompactionFilter>(nullptr);
                } else {
                    return std::unique_ptr<rocksdb::CompactionFilter>(
                        new PrefixDeletingCompactionFilter(std::move(droppedPrefixes)));
                }
            }

            virtual const char* Name() const override {
                return "PrefixDeletingCompactionFilterFactory";
            }

        private:
            const RocksEngine* _engine;
        };

    }  // anonymous namespace

    // first four bytes are the default prefix 0
    const std::string RocksEngine::kMetadataPrefix("\0\0\0\0metadata-", 12);
    const std::string RocksEngine::kDroppedPrefix("\0\0\0\0droppedprefix-", 18);

    RocksEngine::RocksEngine(const std::string& path, bool durable)
        : _path(path), _durable(durable), _maxPrefix(0) {
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
            _block_cache = rocksdb::NewLRUCache(cacheSizeGB * 1024 * 1024 * 1024LL, 7);
        }
        _maxWriteMBPerSec = rocksGlobalOptions.maxWriteMBPerSec;
        _rateLimiter.reset(
            rocksdb::NewGenericRateLimiter(static_cast<int64_t>(_maxWriteMBPerSec) * 1024 * 1024));
        // open DB
        rocksdb::DB* db;
        auto s = rocksdb::DB::Open(_options(), path, &db);
        invariantRocksOK(s);
        _db.reset(db);

        _counterManager.reset(
            new RocksCounterManager(_db.get(), rocksGlobalOptions.crashSafeCounters));
        _compactionScheduler.reset(new RocksCompactionScheduler(_db.get()));

        // open iterator
        boost::scoped_ptr<rocksdb::Iterator> iter(_db->NewIterator(rocksdb::ReadOptions()));

        // find maxPrefix
        iter->SeekToLast();
        if (iter->Valid()) {
            // otherwise the DB is empty, so we just keep it at 0
            bool ok = extractPrefix(iter->key(), &_maxPrefix);
            // this is DB corruption here
            invariant(ok);
        }

        // load ident to prefix map. also update _maxPrefix if there's any prefix bigger than
        // current _maxPrefix
        {
            boost::mutex::scoped_lock lk(_identPrefixMapMutex);
            for (iter->Seek(kMetadataPrefix);
                 iter->Valid() && iter->key().starts_with(kMetadataPrefix); iter->Next()) {
                invariantRocksOK(iter->status());
                rocksdb::Slice ident(iter->key());
                ident.remove_prefix(kMetadataPrefix.size());
                // this could throw DBException, which then means DB corruption. We just let it fly
                // to the caller
                BSONObj identConfig(iter->value().data());
                BSONElement element = identConfig.getField("prefix");

                if (element.eoo() || !element.isNumber()) {
                    log() << "Mongo metadata in RocksDB database is corrupted.";
                    invariant(false);
                }

                uint32_t identPrefix = static_cast<uint32_t>(element.numberInt());
                _identPrefixMap[StringData(ident.data(), ident.size())] = identPrefix;

                _maxPrefix = std::max(_maxPrefix, identPrefix);
            }
        }

        // just to be extra sure. we need this if last collection is oplog -- in that case we
        // reserve prefix+1 for oplog key tracker
        ++_maxPrefix;

        // load dropped prefixes
        {
            rocksdb::WriteBatch wb;
            // we will use this iter to check if prefixes are still alive
            boost::scoped_ptr<rocksdb::Iterator> prefixIter(
                _db->NewIterator(rocksdb::ReadOptions()));
            for (iter->Seek(kDroppedPrefix);
                 iter->Valid() && iter->key().starts_with(kDroppedPrefix); iter->Next()) {
                invariantRocksOK(iter->status());
                rocksdb::Slice prefix(iter->key());
                prefix.remove_prefix(kDroppedPrefix.size());
                prefixIter->Seek(prefix);
                invariantRocksOK(iter->status());
                if (prefixIter->Valid() && prefixIter->key().starts_with(prefix)) {
                    // prefix is still alive, let's instruct the compaction filter to clear it up
                    uint32_t int_prefix;
                    bool ok = extractPrefix(prefix, &int_prefix);
                    invariant(ok);
                    {
                        boost::mutex::scoped_lock lk(_droppedPrefixesMutex);
                        _droppedPrefixes.insert(int_prefix);
                    }
                } else {
                    // prefix is no longer alive. let's remove the prefix from our dropped prefixes
                    // list
                    wb.Delete(iter->key());
                }
            }
            if (wb.Count() > 0) {
                auto s = _db->Write(rocksdb::WriteOptions(), &wb);
                invariantRocksOK(s);
            }
        }
    }

    RocksEngine::~RocksEngine() { cleanShutdown(); }

    RecoveryUnit* RocksEngine::newRecoveryUnit() {
        return new RocksRecoveryUnit(&_transactionEngine, _db.get(), _counterManager.get(),
                                     _compactionScheduler.get(), _durable);
    }

    Status RocksEngine::createRecordStore(OperationContext* opCtx, const StringData& ns,
                                          const StringData& ident,
                                          const CollectionOptions& options) {
        auto s = _createIdentPrefix(ident);
        if (s.isOK() && NamespaceString::oplog(ns)) {
            _oplogIdent = ident.toString();
            // oplog needs two prefixes, so we also reserve the next one
            uint64_t oplogTrackerPrefix = 0;
            {
                boost::mutex::scoped_lock lk(_identPrefixMapMutex);
                oplogTrackerPrefix = ++_maxPrefix;
            }
            // we also need to write out the new prefix to the database. this is just an
            // optimization
            std::string encodedPrefix(encodePrefix(oplogTrackerPrefix));
            s = rocksToMongoStatus(
                _db->Put(rocksdb::WriteOptions(), encodedPrefix, rocksdb::Slice()));
        }
        return s;
    }

    RecordStore* RocksEngine::getRecordStore(OperationContext* opCtx, const StringData& ns,
                                             const StringData& ident,
                                             const CollectionOptions& options) {
        if (NamespaceString::oplog(ns)) {
            _oplogIdent = ident.toString();
        }
        RocksRecordStore* recordStore =
            options.capped
                ? new RocksRecordStore(
                      ns, ident, _db.get(), _counterManager.get(), _getIdentPrefix(ident), true,
                      options.cappedSize ? options.cappedSize : 4096,  // default size
                      options.cappedMaxDocs ? options.cappedMaxDocs : -1)
                : new RocksRecordStore(ns, ident, _db.get(), _counterManager.get(),
                                       _getIdentPrefix(ident));

        {
            boost::mutex::scoped_lock lk(_identObjectMapMutex);
            _identCollectionMap[ident] = recordStore;
        }
        return recordStore;
    }

    Status RocksEngine::createSortedDataInterface(OperationContext* opCtx, const StringData& ident,
                                                  const IndexDescriptor* desc) {
        return _createIdentPrefix(ident);
    }

    SortedDataInterface* RocksEngine::getSortedDataInterface(OperationContext* opCtx,
                                                             const StringData& ident,
                                                             const IndexDescriptor* desc) {
        RocksIndexBase* index;
        if (desc->unique()) {
            index = new RocksUniqueIndex(_db.get(), _getIdentPrefix(ident), ident.toString(),
                                         Ordering::make(desc->keyPattern()));
        } else {
            index = new RocksStandardIndex(_db.get(), _getIdentPrefix(ident), ident.toString(),
                                           Ordering::make(desc->keyPattern()));
        }

        {
            boost::mutex::scoped_lock lk(_identObjectMapMutex);
            _identIndexMap[ident] = index;
        }
        return index;
    }

    // cannot be rolled back
    Status RocksEngine::dropIdent(OperationContext* opCtx, const StringData& ident) {
        rocksdb::WriteBatch wb;
        wb.Delete(kMetadataPrefix + ident.toString());

        // calculate which prefixes we need to drop
        std::vector<std::string> prefixesToDrop;
        prefixesToDrop.push_back(_getIdentPrefix(ident));
        if (_oplogIdent == ident.toString()) {
            // if we're dropping oplog, we also need to drop keys from RocksOplogKeyTracker (they
            // are stored at prefix+1)
            prefixesToDrop.push_back(rocksGetNextPrefix(prefixesToDrop[0]));
        }

        // We record the fact that we're deleting this prefix. That way we ensure that the prefix is
        // always deleted
        for (const auto& prefix : prefixesToDrop) {
            wb.Put(kDroppedPrefix + prefix, "");
        }

        // we need to make sure this is on disk before starting to delete data in compactions
        rocksdb::WriteOptions syncOptions;
        syncOptions.sync = true;
        auto s = _db->Write(syncOptions, &wb);
        if (!s.ok()) {
            return rocksToMongoStatus(s);
        }

        // remove from map
        {
            boost::mutex::scoped_lock lk(_identPrefixMapMutex);
            _identPrefixMap.erase(ident);
        }

        // instruct compaction filter to start deleting
        {
            boost::mutex::scoped_lock lk(_droppedPrefixesMutex);
            for (const auto& prefix : prefixesToDrop) {
                uint32_t int_prefix;
                bool ok = extractPrefix(prefix, &int_prefix);
                invariant(ok);
                _droppedPrefixes.insert(int_prefix);
            }
        }

        return Status::OK();
    }

    bool RocksEngine::hasIdent(OperationContext* opCtx, const StringData& ident) const {
        boost::mutex::scoped_lock lk(_identPrefixMapMutex);
        return _identPrefixMap.find(ident) != _identPrefixMap.end();
    }

    std::vector<std::string> RocksEngine::getAllIdents(OperationContext* opCtx) const {
        std::vector<std::string> indents;
        for (auto& entry : _identPrefixMap) {
            indents.push_back(entry.first);
        }
        return indents;
    }

    void RocksEngine::cleanShutdown() { _counterManager->sync(); }

    int64_t RocksEngine::getIdentSize(OperationContext* opCtx, const StringData& ident) {
        boost::mutex::scoped_lock lk(_identObjectMapMutex);

        auto indexIter = _identIndexMap.find(ident);
        if (indexIter != _identIndexMap.end()) {
            return static_cast<int64_t>(indexIter->second->getSpaceUsedBytes(opCtx));
        }
        auto collectionIter = _identCollectionMap.find(ident);
        if (collectionIter != _identCollectionMap.end()) {
            return collectionIter->second->storageSize(opCtx);
        }

        // this can only happen if collection or index exists, but it's not opened (i.e.
        // getRecordStore or getSortedDataInterface are not called)
        return 1;
    }

    void RocksEngine::setMaxWriteMBPerSec(int maxWriteMBPerSec) {
        _maxWriteMBPerSec = maxWriteMBPerSec;
        _rateLimiter->SetBytesPerSecond(static_cast<int64_t>(_maxWriteMBPerSec) * 1024 * 1024);
    }

    Status RocksEngine::backup(const std::string& path) {
        rocksdb::Checkpoint* checkpoint;
        auto s = rocksdb::Checkpoint::Create(_db.get(), &checkpoint);
        if (s.ok()) {
            s = checkpoint->CreateCheckpoint(path);
        }
        delete checkpoint;
        return rocksToMongoStatus(s);
    }

    std::unordered_set<uint32_t> RocksEngine::getDroppedPrefixes() const {
        boost::mutex::scoped_lock lk(_droppedPrefixesMutex);
        // this will copy the set. that way compaction filter has its own copy and doesn't need to
        // worry about thread safety
        return _droppedPrefixes;
    }

    // non public api
    Status RocksEngine::_createIdentPrefix(const StringData& ident) {
        uint32_t prefix = 0;
        {
            boost::mutex::scoped_lock lk(_identPrefixMapMutex);
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

        if (s.ok()) {
            // As an optimization, add a key <prefix> to the DB
            std::string encodedPrefix(encodePrefix(prefix));
            s = _db->Put(rocksdb::WriteOptions(), encodedPrefix, rocksdb::Slice());
        }

        return rocksToMongoStatus(s);
    }

    std::string RocksEngine::_getIdentPrefix(const StringData& ident) {
        boost::mutex::scoped_lock lk(_identPrefixMapMutex);
        auto prefixIter = _identPrefixMap.find(ident);
        invariant(prefixIter != _identPrefixMap.end());
        return encodePrefix(prefixIter->second);
    }

    rocksdb::Options RocksEngine::_options() const {
        // default options
        rocksdb::Options options;
        options.rate_limiter = _rateLimiter;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = _block_cache;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        table_options.block_size = 16 * 1024; // 16KB
        table_options.format_version = 2;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        options.write_buffer_size = 64 * 1024 * 1024;  // 64MB
        options.level0_slowdown_writes_trigger = 8;
        options.max_write_buffer_number = 4;
        options.max_background_compactions = 8;
        options.max_background_flushes = 2;
        options.target_file_size_base = 64 * 1024 * 1024; // 64MB
        options.soft_rate_limit = 2.5;
        options.hard_rate_limit = 3;
        options.level_compaction_dynamic_level_bytes = true;
        options.max_bytes_for_level_base = 512 * 1024 * 1024;  // 512 MB
        // This means there is no limit on open files. Make sure to always set ulimit so that it can
        // keep all RocksDB files opened.
        options.max_open_files = -1;
        options.compaction_filter_factory.reset(new PrefixDeletingCompactionFilterFactory(this));
        options.enable_thread_tracking = true;

        options.compression_per_level.resize(3);
        options.compression_per_level[0] = rocksdb::kNoCompression;
        options.compression_per_level[1] = rocksdb::kNoCompression;
        if (rocksGlobalOptions.compression == "snappy") {
            options.compression_per_level[2] = rocksdb::kSnappyCompression;
        } else if (rocksGlobalOptions.compression == "zlib") {
            options.compression_per_level[2] = rocksdb::kZlibCompression;
        } else if (rocksGlobalOptions.compression == "none") {
            options.compression_per_level[2] = rocksdb::kNoCompression;
        } else {
            log() << "Unknown compression, will use default (snappy)";
            options.compression_per_level[2] = rocksdb::kSnappyCompression;
        }

        // create the DB if it's not already present
        options.create_if_missing = true;
        options.wal_dir = _path + "/journal";

        // allow override
        if (!rocksGlobalOptions.configString.empty()) {
            rocksdb::Options base_options(options);
            auto s = rocksdb::GetOptionsFromString(base_options, rocksGlobalOptions.configString, &options);
            if (!s.ok()) {
              log() << "Invalid rocksdbConfigString \"" << rocksGlobalOptions.configString << "\"";
              invariantRocksOK(s);
            }
        }

        return options;
    }
}
