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

#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/column_store.h"

namespace mongo {

/**
 * A ColumnStore index implementation for WiredTiger.
 *
 * Two documents in a RecordStore, for example:
 *
 * RID 1 : { _id: ObjectID(...), version: 2, author: { first: "Bob", last: "Adly" } }
 * RID 2 : { _id: ObjectID(...), version: 3, author: { first: "Bob", last: "Adly" }, viewed: true }
 *
 * would look something like this in a ColumnStore:
 *
 * { _id\01, { vals: [ ObjectId("...") ] } }
 * { _id\02, { vals: [ ObjectId("...") ] } }
 * { author\01, { flags: [HAS_SUBPATHS] } }
 * { author\02, { flags: [HAS_SUBPATHS] } }
 * { author.first\01, { vals: [ "Bob" ] } }
 * { author.first\02, { vals: [ "Bob" ] } }
 * { author.last\01, { vals: [ "Adly" ] } }
 * { author.last\02, { vals: [ "Adly" ] } }
 * { version\01, { vals: [ 2 ] } }
 * { version\02, { vals: [ 3 ] } }
 * { viewed\02, { vals: [ true ] } }
 * { \xFF1, { flags: [HAS_SUBPATHS] } }
 * { \xFF2, { flags: [HAS_SUBPATHS] } }
 *
 */
class WiredTigerColumnStore final : public ColumnStore {
public:
    class WriteCursor;
    class Cursor;
    class BulkBuilder;

    static StatusWith<std::string> generateCreateString(const std::string& engineName,
                                                        const NamespaceString& collectionNamespace,
                                                        const IndexDescriptor& desc,
                                                        bool isLogged);

    static Status create(OperationContext* opCtx,
                         const std::string& uri,
                         const std::string& config);

    WiredTigerColumnStore(OperationContext* ctx,
                          const std::string& uri,
                          StringData ident,
                          const IndexDescriptor* desc,
                          bool isLogged);
    ~WiredTigerColumnStore() = default;

    //
    // CRUD
    //
    std::unique_ptr<ColumnStore::WriteCursor> newWriteCursor(OperationContext*) override;
    void insert(OperationContext*, PathView, RowId, CellView) override;
    void remove(OperationContext*, PathView, RowId) override;
    void update(OperationContext*, PathView, RowId, CellView) override;
    std::unique_ptr<ColumnStore::Cursor> newCursor(OperationContext*) const override;

    std::unique_ptr<ColumnStore::BulkBuilder> makeBulkBuilder(OperationContext* opCtx) override;

    //
    // Whole ColumnStore ops
    //
    StatusWith<int64_t> compact(OperationContext* opCtx, const CompactOptions& options) override;
    IndexValidateResults validate(OperationContext* opCtx, bool full) const override;

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* output,
                           double scale) const override;

    long long getSpaceUsedBytes(OperationContext* opCtx) const override;
    long long getFreeStorageBytes(OperationContext* opCtx) const override;

    bool isEmpty(OperationContext* opCtx) override;
    int64_t numEntries(OperationContext* opCtx) const override;

    static std::string makeKey_ForTest(PathView path, RowId id) {
        return makeKey(path, id);
    }

    const std::string& indexName() const {
        return _indexName;
    }

private:
    const std::string& uri() const {
        return _uri;
    }

    static std::string makeKey(PathView path, RowId rid) {
        std::string out;
        makeKeyInBuffer(out, path, rid);
        return out;
    }

    /**
     * Sets 'buffer' to the column key (path/rid). Then returns a reference to the newly set
     * 'buffer'.
     */
    static std::string& makeKeyInBuffer(std::string& buffer, PathView, RowId);

    std::string _uri;
    uint64_t _tableId;
    const IndexDescriptor* _desc;
    const std::string _indexName;
    bool _isLogged;
};
}  // namespace mongo
