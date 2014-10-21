// kv_sorted_data_impl.h

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

#pragma once

#include <atomic>

#include "mongo/bson/ordering.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/index_entry_comparison.h"

namespace mongo {

    class KVDictionary;
    class IndexDescriptor;
    class OperationContext;
    class KVSortedDataImpl;

    /**
     * Dummy implementation for now.  We'd need a KVDictionaryBuilder to
     * do better, let's worry about that later.
     */
    class KVSortedDataBuilderImpl : public SortedDataBuilderInterface {
        KVSortedDataImpl *_impl;
        OperationContext *_txn;
        bool _dupsAllowed;

    public:
        KVSortedDataBuilderImpl(KVSortedDataImpl *impl, OperationContext *txn, bool dupsAllowed)
            : _impl(impl),
              _txn(txn),
              _dupsAllowed(dupsAllowed)
        {}
        virtual Status addKey(const BSONObj& key, const DiskLoc& loc);
    };

    /**
     * Generic implementation of the SortedDataInterface using a KVDictionary.
     */
    class KVSortedDataImpl : public SortedDataInterface {
        MONGO_DISALLOW_COPYING( KVSortedDataImpl );
    public:
        KVSortedDataImpl( KVDictionary* db, OperationContext* opCtx, const IndexDescriptor *desc );

        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed);

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const DiskLoc& loc,
                              bool dupsAllowed);

        virtual void unindex(OperationContext* txn, const BSONObj& key, const DiskLoc& loc, bool dupsAllowed);

        virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const DiskLoc& loc);

        virtual void fullValidate(OperationContext* txn, long long* numKeysOut) const;

        virtual bool isEmpty(OperationContext* txn);

        virtual Status touch(OperationContext* txn) const;

        virtual long long numEntries(OperationContext* txn) const;

        virtual Cursor* newCursor(OperationContext* txn, int direction = 1) const;

        virtual Status initAsEmpty(OperationContext* txn);

        virtual long long getSpaceUsedBytes( OperationContext* txn ) const;

    private:
        // The KVDictionary interface used to store index keys, which map to empty values.
        boost::scoped_ptr<KVDictionary> _db;
    };

} // namespace mongo
