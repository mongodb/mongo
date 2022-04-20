/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/storage/column_store.h"

namespace mongo {

class ColumnStoreAccessMethod : public IndexAccessMethod {
    ColumnStoreAccessMethod(const ColumnStoreAccessMethod&) = delete;
    ColumnStoreAccessMethod& operator=(const ColumnStoreAccessMethod&) = delete;

public:
    //
    // Column-specific APIs
    //


    ColumnStoreAccessMethod(IndexCatalogEntry* ice, std::unique_ptr<ColumnStore>);


    //
    // Generic IndexAccessMethod APIs
    //

    Status insert(OperationContext* opCtx,
                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                  const CollectionPtr& coll,
                  const std::vector<BsonRecord>& bsonRecords,
                  const InsertDeleteOptions& options,
                  int64_t* numInserted) final;

    void remove(OperationContext* opCtx,
                SharedBufferFragmentBuilder& pooledBufferBuilder,
                const CollectionPtr& coll,
                const BSONObj& obj,
                const RecordId& loc,
                bool logIfError,
                const InsertDeleteOptions& options,
                int64_t* numDeleted,
                CheckRecordId checkRecordId) final;
    Status update(OperationContext* opCtx,
                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                  const BSONObj& oldDoc,
                  const BSONObj& newDoc,
                  const RecordId& loc,
                  const CollectionPtr& coll,
                  const InsertDeleteOptions& options,
                  int64_t* numInserted,
                  int64_t* numDeleted) final;

    Status initializeAsEmpty(OperationContext* opCtx) final;

    void validate(OperationContext* opCtx,
                  int64_t* numKeys,
                  IndexValidateResults* fullResults) const final;

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* result,
                           double scale) const final;

    long long getSpaceUsedBytes(OperationContext* opCtx) const final;

    long long getFreeStorageBytes(OperationContext* opCtx) const final;

    Status compact(OperationContext* opCtx) final;

    std::unique_ptr<IndexAccessMethod::BulkBuilder> initiateBulk(
        size_t maxMemoryUsageBytes,
        const boost::optional<IndexStateInfo>& stateInfo,
        StringData dbName) final;

    Ident* getIdentPtr() const final;

    const ColumnStore* storage() const {
        return _store.get();
    }

    class BulkBuilder;

private:
    const std::unique_ptr<ColumnStore> _store;
    IndexCatalogEntry* const _indexCatalogEntry;  // owned by IndexCatalog
    const IndexDescriptor* const _descriptor;
};
}  // namespace mongo
