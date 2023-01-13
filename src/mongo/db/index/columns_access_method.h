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

#include "mongo/db/index/column_cell.h"
#include "mongo/db/index/column_key_generator.h"
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

    const column_keygen::ColumnKeyGenerator& getKeyGen() const {
        return _keyGen;
    }

    /**
     * Returns a pointer to the ColumnstoreProjection owned by the underlying ColumnKeyGenerator.
     */
    const ColumnStoreProjection* getColumnstoreProjection() const {
        return _keyGen.getColumnstoreProjection();
    }

    //
    // Generic IndexAccessMethod APIs
    //

    Status insert(OperationContext* opCtx,
                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                  const CollectionPtr& coll,
                  const std::vector<BsonRecord>& bsonRecords,
                  const InsertDeleteOptions& options,
                  int64_t* keysInsertedOut) final;

    void remove(OperationContext* opCtx,
                SharedBufferFragmentBuilder& pooledBufferBuilder,
                const CollectionPtr& coll,
                const BSONObj& obj,
                const RecordId& rid,
                bool logIfError,
                const InsertDeleteOptions& options,
                int64_t* keysDeletedOut,
                CheckRecordId checkRecordId) final;

    Status update(OperationContext* opCtx,
                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                  const BSONObj& oldDoc,
                  const BSONObj& newDoc,
                  const RecordId& rid,
                  const CollectionPtr& coll,
                  const InsertDeleteOptions& options,
                  int64_t* keysInsertedOut,
                  int64_t* keysDeletedOut) final;

    Status applyIndexBuildSideWrite(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    const BSONObj& operation,
                                    const InsertDeleteOptions& unusedOptions,
                                    KeyHandlerFn&& unusedFn,
                                    int64_t* keysInserted,
                                    int64_t* keysDeleted) final;

    Status initializeAsEmpty(OperationContext* opCtx) final;

    IndexValidateResults validate(OperationContext* opCtx, bool full) const final;

    int64_t numKeys(OperationContext* opCtx) const final;

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

    std::shared_ptr<Ident> getSharedIdent() const final;

    void setIdent(std::shared_ptr<Ident> ident) final;

    const ColumnStore* storage() const {
        return _store.get();
    }

    ColumnStore* writableStorage() const {
        return _store.get();
    }

    class BulkBuilder;

    const std::string& indexName() const {
        return _descriptor->indexName();
    }

    /**
     * Returns true iff 'compressor' is a recognized name of a block compression module that is
     * supported for use with the column store index.
     *
     * Actual support for the module depends on the storage engine, however. This method does _not_
     * guarantee that creating a column store index with 'compressor' will always work.
     */
    static bool supportsBlockCompressor(StringData compressor);

private:
    void _visitCellsForIndexInsert(OperationContext* opCtx,
                                   PooledFragmentBuilder& pooledFragmentBuilder,
                                   const std::vector<BsonRecord>& bsonRecords,
                                   function_ref<void(StringData, const BsonRecord&)> cb) const;

    const std::unique_ptr<ColumnStore> _store;
    IndexCatalogEntry* const _indexCatalogEntry;  // owned by IndexCatalog
    const IndexDescriptor* const _descriptor;
    const column_keygen::ColumnKeyGenerator _keyGen;
};
}  // namespace mongo
