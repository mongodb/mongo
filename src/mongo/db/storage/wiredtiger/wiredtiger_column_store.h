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

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/column_store.h"

namespace mongo {

class WiredTigerColumnStore final : public ColumnStore {
public:
    class WriteCursor;
    class Cursor;
    class BulkBuilder;

    static StatusWith<std::string> generateCreateString(const std::string& engineName,
                                                        const NamespaceString& collectionNamespace,
                                                        const IndexDescriptor& desc);

    static Status create(OperationContext* opCtx,
                         const std::string& uri,
                         const std::string& config);

    WiredTigerColumnStore(OperationContext* ctx,
                          const std::string& uri,
                          StringData ident,
                          const IndexDescriptor* desc,
                          bool readOnly = false);
    ~WiredTigerColumnStore() = default;

    //
    // CRUD
    //
    std::unique_ptr<ColumnStore::WriteCursor> newWriteCursor(OperationContext*) override;
    void insert(OperationContext*, PathView, RecordId, CellView) override;
    void remove(OperationContext*, PathView, RecordId) override;
    void update(OperationContext*, PathView, RecordId, CellView) override;
    std::unique_ptr<ColumnStore::Cursor> newCursor(OperationContext*) const override;

    std::unique_ptr<ColumnStore::BulkBuilder> makeBulkBuilder(OperationContext* opCtx) override;

    //
    // Whole ColumnStore ops
    //
    Status compact(OperationContext* opCtx) override;
    void fullValidate(OperationContext* opCtx,
                      int64_t* numKeysOut,
                      IndexValidateResults* fullResults) const override;

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* output,
                           double scale) const override;

    long long getSpaceUsedBytes(OperationContext* opCtx) const override;
    long long getFreeStorageBytes(OperationContext* opCtx) const override;

    bool isEmpty(OperationContext* opCtx) override;

    static std::string makeKey_ForTest(PathView path, RecordId id) {
        return makeKey(path, id);
    }

private:
    const std::string& uri() const {
        return _uri;
    }

    static std::string& makeKey(std::string& buffer, PathView, RecordId);
    static std::string makeKey(PathView path, RecordId rid) {
        std::string out;
        makeKey(out, path, rid);
        return out;
    }

    std::string _uri;
    uint64_t _tableId;
    const IndexDescriptor* _desc;
    const std::string _indexName;
};
}  // namespace mongo
