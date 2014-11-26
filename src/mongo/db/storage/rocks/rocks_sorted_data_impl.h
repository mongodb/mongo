// rocks_sorted_data_impl.h

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

#include "mongo/db/storage/sorted_data_interface.h"

#include <atomic>

#include <rocksdb/db.h>

#include "mongo/bson/ordering.h"
#include "mongo/db/storage/index_entry_comparison.h"

#pragma once

namespace rocksdb {
    class ColumnFamilyHandle;
    class DB;
}

namespace mongo {

    class RocksRecoveryUnit;

    class RocksSortedDataBuilderImpl : public SortedDataBuilderInterface {
    public:
        virtual Status addKey(const BSONObj& key, const RecordId& loc) = 0;
    };

    /**
     * Rocks implementation of the SortedDataInterface. Each index is stored as a single column
     * family. Each mapping from a BSONObj to a RecordId is stored as the key of a key-value pair
     * in the column family. Consequently, each value in the database is simply an empty string.
     * This is done because RocksDB only supports unique keys, and because RocksDB can take a custom
     * comparator to use when ordering keys. We use a custom comparator which orders keys based
     * first upon the BSONObj in the key, and uses the RecordId as a tiebreaker.
     */
    class RocksSortedDataImpl : public SortedDataInterface {
        MONGO_DISALLOW_COPYING( RocksSortedDataImpl );
    public:
        RocksSortedDataImpl(rocksdb::DB* db, boost::shared_ptr<rocksdb::ColumnFamilyHandle> cf,
                            std::string ident, Ordering order);

        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed);

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const RecordId& loc,
                              bool dupsAllowed);

        virtual void unindex(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                             bool dupsAllowed);

        virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const RecordId& loc);

        virtual void fullValidate(OperationContext* txn, bool full, long long* numKeysOut,
                                  BSONObjBuilder* output) const;

        virtual bool isEmpty(OperationContext* txn);

        virtual Status touch(OperationContext* txn) const;

        virtual long long numEntries(OperationContext* txn) const;

        virtual Cursor* newCursor(OperationContext* txn, int direction) const;

        virtual Status initAsEmpty(OperationContext* txn);

        virtual long long getSpaceUsedBytes( OperationContext* txn ) const;

        //rocks specific

        // ownership passes to caller. Bare because we need to pass the bare pointer to the
        // rocksdb::Options class
        static rocksdb::Comparator* newRocksComparator( const Ordering& order );

    private:
        typedef RecordId RecordId;

        rocksdb::DB* _db; // not owned

        // Each index is stored as a single column family, so this stores the handle to the
        // relevant column family
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> _columnFamily;

        std::string _ident;

        // used to construct RocksCursors
        const Ordering _order;

        std::atomic<long long> _numEntries;

        const std::string _numEntriesKey;

    };

} // namespace mongo
