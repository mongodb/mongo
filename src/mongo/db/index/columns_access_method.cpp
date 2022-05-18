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


#include "mongo/db/index/columns_access_method.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/column_cell.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/progress_meter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
namespace {
inline void inc(int64_t* counter) {
    if (counter)
        ++*counter;
};
}  // namespace

ColumnStoreAccessMethod::ColumnStoreAccessMethod(IndexCatalogEntry* ice,
                                                 std::unique_ptr<ColumnStore> store)
    : _store(std::move(store)), _indexCatalogEntry(ice), _descriptor(ice->descriptor()) {}

class ColumnStoreAccessMethod::BulkBuilder final : public IndexAccessMethod::BulkBuilder {
public:
    BulkBuilder(ColumnStoreAccessMethod* index, size_t maxMemoryUsageBytes, StringData dbName);

    BulkBuilder(ColumnStoreAccessMethod* index,
                size_t maxMemoryUsageBytes,
                const IndexStateInfo& stateInfo,
                StringData dbName);


    //
    // Generic APIs
    //

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  SharedBufferFragmentBuilder& pooledBuilder,
                  const BSONObj& obj,
                  const RecordId& rid,
                  const InsertDeleteOptions& options,
                  const std::function<void()>& saveCursorBeforeWrite,
                  const std::function<void()>& restoreCursorAfterWrite) final;

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    int64_t getKeysInserted() const;

    mongo::IndexStateInfo persistDataForShutdown() final;

    Status commit(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  bool dupsAllowed,
                  int32_t yieldIterations,
                  const KeyHandlerFn& onDuplicateKeyInserted,
                  const RecordIdHandlerFn& onDuplicateRecord) final;

private:
    ColumnStoreAccessMethod* const _columnsAccess;
    // For now we'll just collect all the docs to insert before inserting them.
    // TODO SERVER-65481 Do an actual optimized bulk insert with sorting.
    std::list<BSONObj> _ownedObjects;
    std::vector<BsonRecord> _deferredInserts;
    int64_t _keysInserted = 0;
};

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  size_t maxMemoryUsageBytes,
                                                  StringData dbName)
    : _columnsAccess(index) {}

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  size_t maxMemoryUsageBytes,
                                                  const IndexStateInfo& stateInfo,
                                                  StringData dbName)
    : _columnsAccess(index) {}

Status ColumnStoreAccessMethod::BulkBuilder::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    SharedBufferFragmentBuilder& pooledBuilder,
    const BSONObj& obj,
    const RecordId& rid,
    const InsertDeleteOptions& options,
    const std::function<void()>& saveCursorBeforeWrite,
    const std::function<void()>& restoreCursorAfterWrite) {
    // TODO SERVER-65481 Do an actual optimized bulk insert with sorting.
    _ownedObjects.push_back(obj.getOwned());
    BsonRecord record;
    record.docPtr = &_ownedObjects.back();
    record.id = rid;
    _deferredInserts.push_back(record);
    return Status::OK();
}

const MultikeyPaths& ColumnStoreAccessMethod::BulkBuilder::getMultikeyPaths() const {
    const static MultikeyPaths empty;
    return empty;
}

bool ColumnStoreAccessMethod::BulkBuilder::isMultikey() const {
    return false;
}

int64_t ColumnStoreAccessMethod::BulkBuilder::getKeysInserted() const {
    return _keysInserted;
}

mongo::IndexStateInfo ColumnStoreAccessMethod::BulkBuilder::persistDataForShutdown() {
    uasserted(ErrorCodes::NotImplemented,
              "ColumnStoreAccessMethod::BulkBuilder::persistDataForShutdown()");
}

Status ColumnStoreAccessMethod::BulkBuilder::commit(OperationContext* opCtx,
                                                    const CollectionPtr& collection,
                                                    bool dupsAllowed,
                                                    int32_t yieldIterations,
                                                    const KeyHandlerFn& onDuplicateKeyInserted,
                                                    const RecordIdHandlerFn& onDuplicateRecord) {
    static constexpr size_t kBufferBlockSize = 1024;
    SharedBufferFragmentBuilder pooledBufferBuilder(kBufferBlockSize);

    WriteUnitOfWork wunit(opCtx);
    auto status = _columnsAccess->insert(opCtx,
                                         pooledBufferBuilder,
                                         collection,
                                         _deferredInserts,
                                         InsertDeleteOptions{},
                                         &_keysInserted);
    if (!status.isOK()) {
        return status;
    }

    wunit.commit();
    return Status::OK();
}

Status ColumnStoreAccessMethod::insert(OperationContext* opCtx,
                                       SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const CollectionPtr& coll,
                                       const std::vector<BsonRecord>& bsonRecords,
                                       const InsertDeleteOptions& options,
                                       int64_t* keysInsertedOut) {
    try {
        PooledFragmentBuilder buf(pooledBufferBuilder);
        auto cursor = _store->newWriteCursor(opCtx);
        column_keygen::visitCellsForInsert(
            bsonRecords,
            [&](StringData path,
                const BsonRecord& rec,
                const column_keygen::UnencodedCellView& cell) {
                if (!rec.ts.isNull()) {
                    uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(rec.ts));
                }

                buf.reset();
                column_keygen::writeEncodedCell(cell, &buf);
                cursor->insert(path, rec.id, CellView{buf.buf(), size_t(buf.len())});

                inc(keysInsertedOut);
            });
        return Status::OK();
    } catch (const AssertionException& ex) {
        return ex.toStatus();
    }
}

void ColumnStoreAccessMethod::remove(OperationContext* opCtx,
                                     SharedBufferFragmentBuilder& pooledBufferBuilder,
                                     const CollectionPtr& coll,
                                     const BSONObj& obj,
                                     const RecordId& rid,
                                     bool logIfError,
                                     const InsertDeleteOptions& options,
                                     int64_t* keysDeletedOut,
                                     CheckRecordId checkRecordId) {
    auto cursor = _store->newWriteCursor(opCtx);
    column_keygen::visitPathsForDelete(obj, [&](StringData path) {
        cursor->remove(path, rid);
        inc(keysDeletedOut);
    });
}

Status ColumnStoreAccessMethod::update(OperationContext* opCtx,
                                       SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const BSONObj& oldDoc,
                                       const BSONObj& newDoc,
                                       const RecordId& rid,
                                       const CollectionPtr& coll,
                                       const InsertDeleteOptions& options,
                                       int64_t* keysInsertedOut,
                                       int64_t* keysDeletedOut) {
    PooledFragmentBuilder buf(pooledBufferBuilder);
    auto cursor = _store->newWriteCursor(opCtx);
    column_keygen::visitDiffForUpdate(
        oldDoc,
        newDoc,
        [&](column_keygen::DiffAction diffAction,
            StringData path,
            const column_keygen::UnencodedCellView* cell) {
            if (diffAction == column_keygen::DiffAction::kDelete) {
                cursor->remove(path, rid);
                inc(keysDeletedOut);
                return;
            }

            // kInsert and kUpdate are handled almost identically. If we switch to using
            // `overwrite=true` cursors in WT, we could consider making them the same, although that
            // might disadvantage other implementations of the storage engine API.
            buf.reset();
            column_keygen::writeEncodedCell(*cell, &buf);

            const auto method = diffAction == column_keygen::DiffAction::kInsert
                ? &ColumnStore::WriteCursor::insert
                : &ColumnStore::WriteCursor::update;
            (cursor.get()->*method)(path, rid, CellView{buf.buf(), size_t(buf.len())});

            inc(keysInsertedOut);
        });
    return Status::OK();
}

Status ColumnStoreAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return Status::OK();
}

void ColumnStoreAccessMethod::validate(OperationContext* opCtx,
                                       int64_t* numKeys,
                                       IndexValidateResults* fullResults) const {
    _store->fullValidate(opCtx, numKeys, fullResults);
}

bool ColumnStoreAccessMethod::appendCustomStats(OperationContext* opCtx,
                                                BSONObjBuilder* result,
                                                double scale) const {
    return _store->appendCustomStats(opCtx, result, scale);
}

long long ColumnStoreAccessMethod::getSpaceUsedBytes(OperationContext* opCtx) const {
    return _store->getSpaceUsedBytes(opCtx);
}

long long ColumnStoreAccessMethod::getFreeStorageBytes(OperationContext* opCtx) const {
    return _store->getFreeStorageBytes(opCtx);
}

Status ColumnStoreAccessMethod::compact(OperationContext* opCtx) {
    return _store->compact(opCtx);
}


std::unique_ptr<IndexAccessMethod::BulkBuilder> ColumnStoreAccessMethod::initiateBulk(
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    StringData dbName) {
    // TODO support resuming an index build.
    invariant(!stateInfo);
    return std::make_unique<BulkBuilder>(this, maxMemoryUsageBytes, dbName);
}

Ident* ColumnStoreAccessMethod::getIdentPtr() const {
    return _store.get();
}
}  // namespace mongo
