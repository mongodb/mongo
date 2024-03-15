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

#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index/bulk_builder_common.h"
#include "mongo/db/index/column_cell.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/index/column_store_sorter.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_gen.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
namespace {
inline void inc(int64_t* counter) {
    if (counter)
        ++*counter;
};

inline void dec(int64_t* counter) {
    if (counter)
        --*counter;
};
}  // namespace

ColumnStoreAccessMethod::ColumnStoreAccessMethod(IndexCatalogEntry* ice,
                                                 std::unique_ptr<ColumnStore> store)
    : _store(std::move(store)),
      _keyGen(ice->descriptor()->keyPattern(), ice->descriptor()->pathProjection()) {}

class ColumnStoreAccessMethod::BulkBuilder final
    : public BulkBuilderCommon<ColumnStoreAccessMethod::BulkBuilder> {
public:
    BulkBuilder(ColumnStoreAccessMethod* index,
                const IndexCatalogEntry* entry,
                size_t maxMemoryUsageBytes,
                const DatabaseName& dbName);

    BulkBuilder(ColumnStoreAccessMethod* index,
                const IndexCatalogEntry* entry,
                size_t maxMemoryUsageBytes,
                const IndexStateInfo& stateInfo,
                const DatabaseName& dbName);

    //
    // Generic APIs
    //

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  const IndexCatalogEntry* entry,
                  const BSONObj& obj,
                  const RecordId& rid,
                  const InsertDeleteOptions& options,
                  const OnSuppressedErrorFn& onSuppressedError,
                  const ShouldRelaxConstraintsFn& shouldRelaxConstraints) final;

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    int64_t getKeysInserted() const;

    IndexStateInfo persistDataForShutdown() final;
    std::unique_ptr<ColumnStoreSorter::Iterator> finalizeSort();

    std::unique_ptr<ColumnStore::BulkBuilder> setUpBulkInserter(OperationContext* opCtx,
                                                                const IndexCatalogEntry* entry,
                                                                bool dupsAllowed);
    void debugEnsureSorted(const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data);

    bool duplicateCheck(OperationContext* opCtx,
                        const IndexCatalogEntry* entry,
                        const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
                        bool dupsAllowed,
                        const RecordIdHandlerFn& onDuplicateRecord);

    void insertKey(std::unique_ptr<ColumnStore::BulkBuilder>& inserter,
                   const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data);

    Status keyCommitted(const KeyHandlerFn& onDuplicateKeyInserted,
                        const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
                        bool isDup);

private:
    ColumnStoreAccessMethod* const _columnsAccess;

    ColumnStoreSorter _sorter;
    BufBuilder _cellBuilder;

    boost::optional<std::pair<PathValue, RowId>> _previousPathAndRowId;
};

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  const IndexCatalogEntry* entry,
                                                  size_t maxMemoryUsageBytes,
                                                  const DatabaseName& dbName)
    : BulkBuilderCommon(0,
                        "Index Build: inserting keys from external sorter into columnstore index",
                        entry->descriptor()->indexName()),
      _columnsAccess(index),
      _sorter(maxMemoryUsageBytes, dbName, bulkBuilderFileStats(), bulkBuilderTracker()) {
    countNewBuildInStats();
}

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  const IndexCatalogEntry* entry,
                                                  size_t maxMemoryUsageBytes,
                                                  const IndexStateInfo& stateInfo,
                                                  const DatabaseName& dbName)
    : BulkBuilderCommon(stateInfo.getNumKeys().value_or(0),
                        "Index Build: inserting keys from external sorter into columnstore index",
                        entry->descriptor()->indexName()),
      _columnsAccess(index),
      _sorter(maxMemoryUsageBytes,
              dbName,
              bulkBuilderFileStats(),
              stateInfo.getFileName()->toString(),
              *stateInfo.getRanges(),
              bulkBuilderTracker()) {
    countResumedBuildInStats();
}

Status ColumnStoreAccessMethod::BulkBuilder::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexCatalogEntry* entry,
    const BSONObj& obj,
    const RecordId& rid,
    const InsertDeleteOptions& options,
    const OnSuppressedErrorFn& onSuppressedError,
    const ShouldRelaxConstraintsFn& shouldRelaxConstraints) {
    _columnsAccess->_keyGen.visitCellsForInsert(
        obj, [&](PathView path, const column_keygen::UnencodedCellView& cell) {
            _cellBuilder.reset();
            column_keygen::writeEncodedCell(cell, &_cellBuilder);
            tassert(6762300, "RecordID cannot be a string for column store indexes", !rid.isStr());
            _sorter.add(path, rid.getLong(), CellView(_cellBuilder.buf(), _cellBuilder.len()));

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

IndexStateInfo ColumnStoreAccessMethod::BulkBuilder::persistDataForShutdown() {
    auto state = _sorter.persistDataForShutdown();

    IndexStateInfo stateInfo;
    stateInfo.setFileName(StringData(state.fileName));
    stateInfo.setNumKeys(_keysInserted);
    stateInfo.setRanges(std::move(state.ranges));

    return stateInfo;
}

std::unique_ptr<ColumnStoreSorter::Iterator> ColumnStoreAccessMethod::BulkBuilder::finalizeSort() {
    return std::unique_ptr<ColumnStoreSorter::Iterator>(_sorter.done());
}

std::unique_ptr<ColumnStore::BulkBuilder> ColumnStoreAccessMethod::BulkBuilder::setUpBulkInserter(
    OperationContext* opCtx, const IndexCatalogEntry* entry, bool dupsAllowed) {
    _ns = entry->getNSSFromCatalog(opCtx);
    return _columnsAccess->_store->makeBulkBuilder(opCtx);
}

void ColumnStoreAccessMethod::BulkBuilder::debugEnsureSorted(
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data) {
    // In debug mode only, assert that keys are retrieved from the sorter in strictly
    // increasing order.
    const auto& key = data.first;
    if (_previousPathAndRowId &&
        !(ColumnStoreSorter::Key{_previousPathAndRowId->first, _previousPathAndRowId->second} <
          key)) {
        LOGV2_FATAL_NOTRACE(6548100,
                            "Out-of-order result from sorter for column store bulk loader",
                            "prevPathName"_attr = _previousPathAndRowId->first,
                            "prevRecordId"_attr = _previousPathAndRowId->second,
                            "nextPathName"_attr = key.path,
                            "nextRecordId"_attr = key.rowId,
                            "index"_attr = _indexName);
    }
    // It is not safe to safe to directly store the 'key' object, because it includes a
    // PathView, which may be invalid the next time we read it.
    _previousPathAndRowId.emplace(key.path, key.rowId);
}

bool ColumnStoreAccessMethod::BulkBuilder::duplicateCheck(
    OperationContext* opCtx,
    const IndexCatalogEntry* entry,
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
    bool dupsAllowed,
    const RecordIdHandlerFn& onDuplicateRecord) {
    // no duplicates in a columnstore index
    return false;
}

void ColumnStoreAccessMethod::BulkBuilder::insertKey(
    std::unique_ptr<ColumnStore::BulkBuilder>& inserter,
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data) {

    auto& [columnStoreKey, columnStoreValue] = data;
    inserter->addCell(columnStoreKey.path, columnStoreKey.rowId, columnStoreValue.cell);
}

Status ColumnStoreAccessMethod::BulkBuilder::keyCommitted(
    const KeyHandlerFn& onDuplicateKeyInserted,
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
    bool isDup) {
    // nothing to do for columnstore indexes
    return Status::OK();
}

void ColumnStoreAccessMethod::_visitCellsForIndexInsert(
    OperationContext* opCtx,
    PooledFragmentBuilder& buf,
    const std::vector<BsonRecord>& bsonRecords,
    function_ref<void(StringData, const BsonRecord&)> cb) const {
    _keyGen.visitCellsForInsert(
        bsonRecords,
        [&](StringData path, const BsonRecord& rec, const column_keygen::UnencodedCellView& cell) {
            if (!rec.ts.isNull()) {
                uassertStatusOK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(rec.ts));
            }
            buf.reset();
            column_keygen::writeEncodedCell(cell, &buf);
            tassert(
                6597800, "RecordID cannot be a string for column store indexes", !rec.id.isStr());
            cb(path, rec);
        });
}

Status ColumnStoreAccessMethod::insert(OperationContext* opCtx,
                                       SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const CollectionPtr& coll,
                                       const IndexCatalogEntry* entry,
                                       const std::vector<BsonRecord>& bsonRecords,
                                       const InsertDeleteOptions& options,
                                       int64_t* keysInsertedOut) {
    try {
        PooledFragmentBuilder buf(pooledBufferBuilder);
        // We cannot write to the index during its initial build phase, so we defer this insert as a
        // "side write" to be applied after the build completes.
        if (entry->isHybridBuilding()) {
            auto columnChanges = StorageExecutionContext::get(opCtx).columnChanges();
            _visitCellsForIndexInsert(
                opCtx, buf, bsonRecords, [&](StringData path, const BsonRecord& rec) {
                    columnChanges->emplace_back(
                        path.toString(),
                        CellView{buf.buf(), size_t(buf.len())}.toString(),
                        rec.id,
                        column_keygen::ColumnKeyGenerator::DiffAction::kInsert);
                });
            int64_t inserted = 0;
            int64_t deleted = 0;
            ON_BLOCK_EXIT([keysInsertedOut, inserted, deleted] {
                if (keysInsertedOut) {
                    *keysInsertedOut += inserted;
                }
                invariant(deleted == 0);
            });
            uassertStatusOK(entry->indexBuildInterceptor()->sideWrite(
                opCtx, entry, *columnChanges, &inserted, &deleted));
            return Status::OK();
        } else {
            auto cursor = _store->newWriteCursor(opCtx);
            _visitCellsForIndexInsert(
                opCtx, buf, bsonRecords, [&](StringData path, const BsonRecord& rec) {
                    cursor->insert(path, rec.id.getLong(), CellView{buf.buf(), size_t(buf.len())});
                    inc(keysInsertedOut);
                });
            return Status::OK();
        }
    } catch (const AssertionException& ex) {
        return ex.toStatus();
    }
}

void ColumnStoreAccessMethod::remove(OperationContext* opCtx,
                                     SharedBufferFragmentBuilder& pooledBufferBuilder,
                                     const CollectionPtr& coll,
                                     const IndexCatalogEntry* entry,
                                     const BSONObj& obj,
                                     const RecordId& rid,
                                     bool logIfError,
                                     const InsertDeleteOptions& options,
                                     int64_t* keysDeletedOut,
                                     CheckRecordId checkRecordId) {
    if (entry->isHybridBuilding()) {
        auto columnChanges = StorageExecutionContext::get(opCtx).columnChanges();
        _keyGen.visitPathsForDelete(obj, [&](StringData path) {
            columnChanges->emplace_back(path.toString(),
                                        "",  // No cell content is necessary to describe a deletion.
                                        rid,
                                        column_keygen::ColumnKeyGenerator::DiffAction::kDelete);
        });
        int64_t inserted = 0;
        int64_t removed = 0;
        fassert(6597801,
                entry->indexBuildInterceptor()->sideWrite(
                    opCtx, entry, *columnChanges, &inserted, &removed));
        if (keysDeletedOut) {
            *keysDeletedOut += removed;
        }
        invariant(inserted == 0);
    } else {
        auto cursor = _store->newWriteCursor(opCtx);
        _keyGen.visitPathsForDelete(obj, [&](PathView path) {
            tassert(6762301, "RecordID cannot be a string for column store indexes", !rid.isStr());
            cursor->remove(path, rid.getLong());
            inc(keysDeletedOut);
        });
    }
}

Status ColumnStoreAccessMethod::update(OperationContext* opCtx,
                                       SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const BSONObj& oldDoc,
                                       const BSONObj& newDoc,
                                       const RecordId& rid,
                                       const CollectionPtr& coll,
                                       const IndexCatalogEntry* entry,
                                       const InsertDeleteOptions& options,
                                       int64_t* keysInsertedOut,
                                       int64_t* keysDeletedOut) {
    PooledFragmentBuilder buf(pooledBufferBuilder);

    if (entry->isHybridBuilding()) {
        auto columnChanges = StorageExecutionContext::get(opCtx).columnChanges();
        _keyGen.visitDiffForUpdate(
            oldDoc,
            newDoc,
            [&](column_keygen::ColumnKeyGenerator::DiffAction diffAction,
                StringData path,
                const column_keygen::UnencodedCellView* cell) {
                if (diffAction == column_keygen::ColumnKeyGenerator::DiffAction::kDelete) {
                    columnChanges->emplace_back(
                        path.toString(),
                        "",  // No cell content is necessary to describe a deletion.
                        rid,
                        diffAction);
                    return;
                }

                // kInsert and kUpdate are handled almost identically. If we switch to using
                // `overwrite=true` cursors in WT, we could consider making them the same,
                // although that might disadvantage other implementations of the storage engine
                // API.
                buf.reset();
                column_keygen::writeEncodedCell(*cell, &buf);

                columnChanges->emplace_back(path.toString(),
                                            CellView{buf.buf(), size_t(buf.len())}.toString(),
                                            rid,
                                            diffAction);
            });

        // Create a "side write" that records the changes made to this document during the bulk
        // build, so that they can be applied when the bulk builder finishes. It is possible that an
        // update does not result in any changes when there is a "columnstoreProjection" on the
        // index that excludes all the changed fields.
        int64_t inserted = 0;
        int64_t deleted = 0;
        if (columnChanges->size() > 0) {
            uassertStatusOK(entry->indexBuildInterceptor()->sideWrite(
                opCtx, entry, *columnChanges, &inserted, &deleted));
        }
        if (keysInsertedOut) {
            *keysInsertedOut += inserted;
        }
        if (keysDeletedOut) {
            *keysDeletedOut += deleted;
        }
    } else {
        auto cursor = _store->newWriteCursor(opCtx);
        _keyGen.visitDiffForUpdate(
            oldDoc,
            newDoc,
            [&](column_keygen::ColumnKeyGenerator::DiffAction diffAction,
                StringData path,
                const column_keygen::UnencodedCellView* cell) {
                if (diffAction == column_keygen::ColumnKeyGenerator::DiffAction::kDelete) {
                    tassert(6762302,
                            "RecordID cannot be a string for column store indexes",
                            !rid.isStr());
                    cursor->remove(path, rid.getLong());
                    inc(keysDeletedOut);
                    return;
                }

                // kInsert and kUpdate are handled almost identically. If we switch to using
                // `overwrite=true` cursors in WT, we could consider making them the same, although
                // that might disadvantage other implementations of the storage engine API.
                buf.reset();
                column_keygen::writeEncodedCell(*cell, &buf);

                const auto method =
                    diffAction == column_keygen::ColumnKeyGenerator::DiffAction::kInsert
                    ? &ColumnStore::WriteCursor::insert
                    : &ColumnStore::WriteCursor::update;
                tassert(
                    6762303, "RecordID cannot be a string for column store indexes", !rid.isStr());
                (cursor.get()->*method)(
                    path, rid.getLong(), CellView{buf.buf(), size_t(buf.len())});

                inc(keysInsertedOut);
            });
    }
    return Status::OK();
}  // namespace mongo

Status ColumnStoreAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return Status::OK();
}

IndexValidateResults ColumnStoreAccessMethod::validate(OperationContext* opCtx, bool full) const {
    return _store->validate(opCtx, full);
}

int64_t ColumnStoreAccessMethod::numKeys(OperationContext* opCtx) const {
    return _store->numEntries(opCtx);
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

StatusWith<int64_t> ColumnStoreAccessMethod::compact(OperationContext* opCtx,
                                                     const CompactOptions& options) {
    return _store->compact(opCtx, options);
}


std::unique_ptr<IndexAccessMethod::BulkBuilder> ColumnStoreAccessMethod::initiateBulk(
    const IndexCatalogEntry* entry,
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    const DatabaseName& dbName) {
    return (stateInfo && stateInfo->getFileName())
        ? std::make_unique<BulkBuilder>(this, entry, maxMemoryUsageBytes, *stateInfo, dbName)
        : std::make_unique<BulkBuilder>(this, entry, maxMemoryUsageBytes, dbName);
}

std::shared_ptr<Ident> ColumnStoreAccessMethod::getSharedIdent() const {
    return _store->getSharedIdent();
}

void ColumnStoreAccessMethod::setIdent(std::shared_ptr<Ident> ident) {
    _store->setIdent(std::move(ident));
}

Status ColumnStoreAccessMethod::applyIndexBuildSideWrite(OperationContext* opCtx,
                                                         const CollectionPtr& coll,
                                                         const IndexCatalogEntry* entry,
                                                         const BSONObj& operation,
                                                         const InsertDeleteOptions& unusedOptions,
                                                         KeyHandlerFn&& unusedFn,
                                                         int64_t* keysInserted,
                                                         int64_t* keysDeleted) {
    const IndexBuildInterceptor::Op opType = operation.getStringField("op") == "i"_sd
        ? IndexBuildInterceptor::Op::kInsert
        : operation.getStringField("op") == "d"_sd ? IndexBuildInterceptor::Op::kDelete
                                                   : IndexBuildInterceptor::Op::kUpdate;

    RecordId rid = RecordId::deserializeToken(operation.getField("rid"));

    CellView cell = operation.getStringField("cell");
    PathView path = operation.getStringField("path");

    auto cursor = _store->newWriteCursor(opCtx);

    tassert(6597803, "RecordID cannot be a string for column store indexes", !rid.isStr());
    switch (opType) {
        case IndexBuildInterceptor::Op::kInsert:
            cursor->insert(path, rid.getLong(), cell);
            inc(keysInserted);
            shard_role_details::getRecoveryUnit(opCtx)->onRollback(
                [keysInserted](OperationContext*) { dec(keysInserted); });
            break;
        case IndexBuildInterceptor::Op::kDelete:
            cursor->remove(path, rid.getLong());
            inc(keysDeleted);
            shard_role_details::getRecoveryUnit(opCtx)->onRollback(
                [keysDeleted](OperationContext*) { dec(keysDeleted); });
            break;
        case IndexBuildInterceptor::Op::kUpdate:
            cursor->update(path, rid.getLong(), cell);
            inc(keysInserted);
            shard_role_details::getRecoveryUnit(opCtx)->onRollback(
                [keysInserted](OperationContext*) { dec(keysInserted); });
            break;
    }

    return Status::OK();
}

// static
bool ColumnStoreAccessMethod::supportsBlockCompressor(StringData compressor) {
    static const std::set<StringData> kSupportedCompressors = {
        "none"_sd, "snappy"_sd, "zlib"_sd, "zstd"_sd};
    return kSupportedCompressors.count(compressor) > 0;
}
}  // namespace mongo
