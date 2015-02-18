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
#include <memory>

#include <boost/optional.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include <rocksdb/cache.h>
#include <rocksdb/status.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/rocks/rocks_transaction.h"
#include "mongo/util/string_map.h"

namespace rocksdb {
    class ColumnFamilyHandle;
    struct ColumnFamilyDescriptor;
    struct ColumnFamilyOptions;
    class DB;
    class Comparator;
    class Iterator;
    struct Options;
    struct ReadOptions;
}

namespace mongo {

    struct CollectionOptions;

    class RocksEngine : public KVEngine {
        MONGO_DISALLOW_COPYING( RocksEngine );
    public:
        RocksEngine(const std::string& path, bool durable);
        virtual ~RocksEngine();

        virtual RecoveryUnit* newRecoveryUnit() override;

        virtual Status createRecordStore(OperationContext* opCtx,
                                         StringData ns,
                                         StringData ident,
                                         const CollectionOptions& options) override;

        virtual RecordStore* getRecordStore(OperationContext* opCtx, StringData ns,
                                            StringData ident,
                                            const CollectionOptions& options) override;

        virtual Status createSortedDataInterface(OperationContext* opCtx, StringData ident,
                                                 const IndexDescriptor* desc) override;

        virtual SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                            StringData ident,
                                                            const IndexDescriptor* desc) override;

        virtual Status dropIdent(OperationContext* opCtx, StringData ident) override;

        virtual bool hasIdent(OperationContext* opCtx, StringData ident) const override;

        virtual std::vector<std::string> getAllIdents( OperationContext* opCtx ) const override;

        virtual bool supportsDocLocking() const override {
            return true;
        }

        virtual bool supportsDirectoryPerDB() const override {
            return false;
        }

        virtual bool isDurable() const override { return _durable; }

        virtual int64_t getIdentSize(OperationContext* opCtx,
                                      StringData ident) {
          // TODO: return correct size.
          return 1;
        }

        virtual Status repairIdent(OperationContext* opCtx,
                                    StringData ident) {
            return Status::OK();
        }

        virtual void cleanShutdown() {}

        /**
         * Initializes a background job to remove excess documents in the oplog collections.
         * This applies to the capped collections in the local.oplog.* namespaces (specifically
         * local.oplog.rs for replica sets and local.oplog.$main for master/slave replication).
         * Returns true if a background job is running for the namespace.
         */
        static bool initRsOplogBackgroundThread(StringData ns);

        // rocks specific api

        rocksdb::DB* getDB() { return _db.get(); }
        const rocksdb::DB* getDB() const { return _db.get(); }

    private:
        Status _createIdentPrefix(StringData ident);
        std::string _getIdentPrefix(StringData ident);

        rocksdb::Options _options() const;

        std::string _path;
        boost::scoped_ptr<rocksdb::DB> _db;
        std::shared_ptr<rocksdb::Cache> _block_cache;

        const bool _durable;

        // ident prefix map stores mapping from ident to a prefix (uint32_t)
        mutable boost::mutex _identPrefixMapMutex;
        typedef StringMap<uint32_t> IdentPrefixMap;
        IdentPrefixMap _identPrefixMap;

        // protected by _identPrefixMapMutex
        uint32_t _maxPrefix;

        // This is for concurrency control
        RocksTransactionEngine _transactionEngine;

        static const std::string kMetadataPrefix;
    };

    Status toMongoStatus( rocksdb::Status s );
}
