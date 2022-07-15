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
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/column_cell.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/index/column_store_sorter.h"
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

    ColumnStoreSorter _sorter;
    BufBuilder _cellBuilder;

    int64_t _keysInserted = 0;
};

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  size_t maxMemoryUsageBytes,
                                                  StringData dbName)
    : _columnsAccess(index),
      _sorter(maxMemoryUsageBytes, dbName, bulkBuilderFileStats(), bulkBuilderTracker()) {
    countNewBuildInStats();
}

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  size_t maxMemoryUsageBytes,
                                                  const IndexStateInfo& stateInfo,
                                                  StringData dbName)
    : _columnsAccess(index),
      _sorter(maxMemoryUsageBytes, dbName, bulkBuilderFileStats(), bulkBuilderTracker()) {
    countResumedBuildInStats();
    // TODO SERVER-66925: Add this support.
    tasserted(6548103, "No support for resuming interrupted columnstore index builds.");
}

Status ColumnStoreAccessMethod::BulkBuilder::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    SharedBufferFragmentBuilder& pooledBuilder,
    const BSONObj& obj,
    const RecordId& rid,
    const InsertDeleteOptions& options,
    const std::function<void()>& saveCursorBeforeWrite,
    const std::function<void()>& restoreCursorAfterWrite) {
    column_keygen::visitCellsForInsert(
        obj, [&](PathView path, const column_keygen::UnencodedCellView& cell) {
            _cellBuilder.reset();
            writeEncodedCell(cell, &_cellBuilder);
            _sorter.add(path, rid, CellView(_cellBuilder.buf(), _cellBuilder.len()));

            ++_keysInserted;
        });

    return Status::OK();
}

// The "multikey" property does not apply to columnstore indexes, because the array key does not
// represent a field in a document and
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
    Timer timer;

    auto ns = _columnsAccess->_indexCatalogEntry->getNSSFromCatalog(opCtx);

    static constexpr char message[] =
        "Index Build: inserting keys from external sorter into columnstore index";
    ProgressMeterHolder pm;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.set(
            CurOp::get(opCtx)->setProgress_inlock(message, _keysInserted, 3 /* secondsBetween */));
    }

    auto builder = _columnsAccess->_store->makeBulkBuilder(opCtx);

    int64_t iterations = 0;
    boost::optional<std::pair<PathValue, RecordId>> previousPathAndRecordId;
    std::unique_ptr<ColumnStoreSorter::Iterator> it(_sorter.done());
    while (it->more()) {
        opCtx->checkForInterrupt();

        auto columnStoreKeyWithValue = it->next();
        const auto& key = columnStoreKeyWithValue.first;

        // In debug mode only, assert that keys are retrieved from the sorter in strictly increasing
        // order.
        if (kDebugBuild) {
            if (previousPathAndRecordId &&
                !(ColumnStoreSorter::Key{previousPathAndRecordId->first,
                                         previousPathAndRecordId->second} < key)) {
                LOGV2_FATAL_NOTRACE(6548100,
                                    "Out-of-order result from sorter for column store bulk loader",
                                    "prevPathName"_attr = previousPathAndRecordId->first,
                                    "prevRecordId"_attr = previousPathAndRecordId->second,
                                    "nextPathName"_attr = key.path,
                                    "nextRecordId"_attr = key.recordId,
                                    "index"_attr = _columnsAccess->_descriptor->indexName());
            }

            // It is not safe to safe to directly store the 'key' object, because it includes a
            // PathView, which may be invalid the next time we read it.
            previousPathAndRecordId.emplace(key.path, key.recordId);
        }

        try {
            writeConflictRetry(opCtx, "addingKey", ns.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                auto& [columnStoreKey, columnStoreValue] = columnStoreKeyWithValue;
                builder->addCell(
                    columnStoreKey.path, columnStoreKey.recordId, columnStoreValue.cell);
                wunit.commit();
            });
        } catch (DBException& e) {
            return e.toStatus();
        }

        // Yield locks every 'yieldIterations' key insertions.
        if (yieldIterations > 0 && (++iterations % yieldIterations == 0)) {
            yield(opCtx, &collection, ns);
        }

        pm.hit();
    }

    pm.finished();

    LOGV2(6548101,
          "Index build: bulk sorter inserted {keysInserted} keys into index {index} on namespace "
          "{namespace} in {duration} seconds",
          "keysInserted"_attr = _keysInserted,
          "index"_attr = _columnsAccess->_descriptor->indexName(),
          logAttrs(ns),
          "duration"_attr = Seconds(timer.seconds()));
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
            [&](PathView path,
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
    column_keygen::visitPathsForDelete(obj, [&](PathView path) {
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
