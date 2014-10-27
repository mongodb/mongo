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
#include <memory>

#include <boost/optional.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include <rocksdb/status.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/storage/kv/kv_engine.h"
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
        RocksEngine( const std::string& path );
        virtual ~RocksEngine();

        virtual RecoveryUnit* newRecoveryUnit() override;

        virtual Status createRecordStore(OperationContext* opCtx,
                                         const StringData& ns,
                                         const StringData& ident,
                                         const CollectionOptions& options) override;

        virtual RecordStore* getRecordStore(OperationContext* opCtx, const StringData& ns,
                                            const StringData& ident,
                                            const CollectionOptions& options) override;

        virtual Status dropRecordStore(OperationContext* opCtx, const StringData& ident) override;

        virtual Status createSortedDataInterface(OperationContext* opCtx, const StringData& ident,
                                                 const IndexDescriptor* desc) override;

        virtual SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                            const StringData& ident,
                                                            const IndexDescriptor* desc) override;

        virtual Status dropSortedDataInterface(OperationContext* opCtx,
                                               const StringData& ident) override;

        virtual bool supportsDocLocking() const override {
            return true;
        }

        // rocks specific api

        rocksdb::DB* getDB() { return _db.get(); }
        const rocksdb::DB* getDB() const { return _db.get(); }

        /**
         * Returns a ReadOptions object that uses the snapshot contained in opCtx
         */
        static rocksdb::ReadOptions readOptionsWithSnapshot( OperationContext* opCtx );

        static rocksdb::Options dbOptions();

    private:
        bool _existsColumnFamily(const StringData& ident);
        Status _createColumnFamily(const rocksdb::ColumnFamilyOptions& options,
                                   const StringData& ident);
        Status _dropColumnFamily(const StringData& ident);
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> _getColumnFamily(const StringData& ident);

        std::unordered_map<std::string, Ordering> _loadOrderingMetaData(rocksdb::Iterator* itr);
        std::set<std::string> _loadCollections(rocksdb::Iterator* itr);
        std::vector<std::string> _loadColumnFamilies();

        rocksdb::ColumnFamilyOptions _collectionOptions() const;
        rocksdb::ColumnFamilyOptions _indexOptions(const Ordering& order) const;

        static rocksdb::ColumnFamilyOptions _defaultCFOptions();

        std::string _path;
        boost::scoped_ptr<rocksdb::DB> _db;
        boost::scoped_ptr<rocksdb::Comparator> _collectionComparator;

        // Default column family is owned by the rocksdb::DB instance.
        rocksdb::ColumnFamilyHandle* _defaultHandle;

        mutable boost::mutex _identColumnFamilyMapMutex;
        typedef StringMap<boost::shared_ptr<rocksdb::ColumnFamilyHandle> > IdentColumnFamilyMap;
        IdentColumnFamilyMap _identColumnFamilyMap;

        static const std::string kOrderingPrefix;
        static const std::string kCollectionPrefix;
    };

    Status toMongoStatus( rocksdb::Status s );
}
