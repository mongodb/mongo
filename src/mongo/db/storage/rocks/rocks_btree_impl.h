// rocks_btree_impl.h

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

#include <rocksdb/db.h>

#include "mongo/db/storage/index_entry_comparison.h"

#pragma once

namespace rocksdb {
    class ColumnFamilyHandle;
    class DB;
}

namespace mongo {

    class RocksRecoveryUnit;

    class RocksBtreeBuilderImpl : public SortedDataBuilderInterface {
    public:
        virtual Status addKey(const BSONObj& key, const DiskLoc& loc) = 0;
        virtual unsigned long long commit(bool mayInterrupt) = 0;
    };

    class RocksBtreeImpl : public SortedDataInterface {
    public:
        RocksBtreeImpl( rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf );

        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn,
                                                      bool dupsAllowed);

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const DiskLoc& loc,
                              bool dupsAllowed);

        virtual bool unindex(OperationContext* txn,
                             const BSONObj& key,
                             const DiskLoc& loc);

        virtual Status dupKeyCheck(OperationContext* txn,
                                   const BSONObj& key,
                                   const DiskLoc& loc);

        virtual void fullValidate(OperationContext* txn, long long* numKeysOut);

        virtual bool isEmpty();

        virtual Status touch(OperationContext* txn) const;

        virtual Cursor* newCursor(OperationContext* txn, int direction) const;

        virtual Status initAsEmpty(OperationContext* txn);

    private:
        typedef DiskLoc RecordId;

        RocksRecoveryUnit* _getRecoveryUnit( OperationContext* opCtx ) const;

        rocksdb::DB* _db;
        rocksdb::ColumnFamilyHandle* _columnFamily;

        /**
         * Creates an error code message out of a key
         */
        string dupKeyError(const BSONObj& key) const;
    };

    /**
     * Extends the functionality of IndexKeyEntry to better interact with the rocksdb api.
     * Namely, it is necessary to support conversion from and to a rocksdb::Slice and 
     * rocksdb::SliceParts.
     */
    struct RocksIndexEntry: public IndexKeyEntry {

        /**
         * Constructs a RocksIndexEntry. Currently (7/7/14), strips field names from key,
         * but this may change
         */
        RocksIndexEntry( const BSONObj& key, const DiskLoc loc, bool stripFieldNames = true );

        /**
         * Constructs a RocksIndexEntry from a Slice. 
         */
        RocksIndexEntry( const rocksdb::Slice& slice );

        ~RocksIndexEntry() { }

        /**
         * Returns a string representation of _sliced
         */
        string asString() const;

        rocksdb::SliceParts sliceParts() const { return rocksdb::SliceParts( _sliced, 2 ); }

        int size() const { return _key.objsize() + sizeof( DiskLoc ); }

        rocksdb::Slice _sliced[2];
    };
}
