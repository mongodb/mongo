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
#include "mongo/db/exec/sbe/stages/column_scan.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/index/columns_access_method.h"

namespace mongo {
namespace sbe {
ColumnScanStage::RowstoreScanModeTracker::RowstoreScanModeTracker()
    : _minBatchSize(internalQueryColumnRowstoreScanMinBatchSize.load()),
      _maxBatchSize(std::max(internalQueryColumnRowstoreScanMaxBatchSize.load(), _minBatchSize)),
      _batchSize(_minBatchSize) {}

ColumnScanStage::ColumnScanStage(UUID collectionUuid,
                                 StringData columnIndexName,
                                 std::vector<std::string> paths,
                                 bool densePathIncludedInScan,
                                 std::vector<bool> includeInOutput,
                                 boost::optional<value::SlotId> recordIdSlot,
                                 boost::optional<value::SlotId> reconstuctedRecordSlot,
                                 value::SlotId rowStoreSlot,
                                 std::unique_ptr<EExpression> rowStoreExpr,
                                 std::vector<PathFilter> filteredPaths,
                                 PlanYieldPolicy* yieldPolicy,
                                 PlanNodeId nodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("columnscan"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _collUuid(collectionUuid),
      _columnIndexName(columnIndexName),
      _paths(std::move(paths)),
      _includeInOutput(std::move(includeInOutput)),
      _recordIdSlot(recordIdSlot),
      _reconstructedRecordSlot(reconstuctedRecordSlot),
      _rowStoreSlot(rowStoreSlot),
      _rowStoreExpr(std::move(rowStoreExpr)),
      _filteredPaths(std::move(filteredPaths)),
      _densePathIncludedInScan(densePathIncludedInScan) {
    invariant(_filteredPaths.size() <= _paths.size(),
              "Filtered paths should be a subset of all paths");
    invariant(_paths.size() == _includeInOutput.size());
}

std::unique_ptr<PlanStage> ColumnScanStage::clone() const {
    std::vector<PathFilter> filteredPaths;
    for (const auto& fp : _filteredPaths) {
        filteredPaths.emplace_back(fp.pathIndex, fp.filterExpr->clone(), fp.inputSlotId);
    }
    return std::make_unique<ColumnScanStage>(_collUuid,
                                             _columnIndexName,
                                             _paths,
                                             _densePathIncludedInScan,
                                             _includeInOutput,
                                             _recordIdSlot,
                                             _reconstructedRecordSlot,
                                             _rowStoreSlot,
                                             _rowStoreExpr ? _rowStoreExpr->clone() : nullptr,
                                             std::move(filteredPaths),
                                             _yieldPolicy,
                                             _commonStats.nodeId,
                                             _participateInTrialRunTracking);
}

void ColumnScanStage::prepare(CompileCtx& ctx) {
    ctx.root = this;

    if (_reconstructedRecordSlot) {
        _reconstructedRecordAccessor = std::make_unique<value::OwnedValueAccessor>();
    }
    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    _rowStoreAccessor = std::make_unique<value::OwnedValueAccessor>();
    if (_rowStoreExpr) {
        _rowStoreExprCode = _rowStoreExpr->compile(ctx);
    }

    _filterInputAccessors.resize(_filteredPaths.size());
    for (size_t idx = 0; idx < _filterInputAccessors.size(); ++idx) {
        auto slot = _filteredPaths[idx].inputSlotId;
        auto [it, inserted] = _filterInputAccessorsMap.emplace(slot, &_filterInputAccessors[idx]);
        uassert(6610212, str::stream() << "duplicate slot: " << slot, inserted);
    }
    for (auto& filteredPath : _filteredPaths) {
        _filterExprsCode.emplace_back(filteredPath.filterExpr->compile(ctx));
    }

    tassert(6610200, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    std::tie(_coll, _collName, _catalogEpoch) = acquireCollection(_opCtx, _collUuid);

    auto indexCatalog = _coll->getIndexCatalog();
    auto indexDesc = indexCatalog->findIndexByName(_opCtx, _columnIndexName);
    tassert(6610201,
            str::stream() << "could not find index named '" << _columnIndexName
                          << "' in collection '" << _collName->toStringForErrorMsg() << "'",
            indexDesc);
    _weakIndexCatalogEntry = indexCatalog->getEntryShared(indexDesc);
}

value::SlotAccessor* ColumnScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_reconstructedRecordSlot && slot == *_reconstructedRecordSlot) {
        return _reconstructedRecordAccessor.get();
    }

    if (_recordIdSlot && slot == *_recordIdSlot) {
        return _recordIdAccessor.get();
    }

    if (_rowStoreSlot == slot) {
        return _rowStoreAccessor.get();
    }

    if (auto it = _filterInputAccessorsMap.find(slot); it != _filterInputAccessorsMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ColumnScanStage::doSaveState(bool relinquishCursor) {
    if (_recordIdColumnCursor) {
        _recordIdColumnCursor->makeOwned();
        _recordIdColumnCursor->cursor().save();
    }

    for (auto& cursor : _columnCursors) {
        cursor.makeOwned();
        cursor.cursor().save();
    }

    if (_rowStoreCursor && relinquishCursor) {
        _rowStoreCursor->save();
    }

    if (_rowStoreCursor) {
        _rowStoreCursor->setSaveStorageCursorOnDetachFromOperationContext(!relinquishCursor);
    }

    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->cursor().saveUnpositioned();
    }

    _coll.reset();
}

void ColumnScanStage::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage has not been prepared, then yield recovery is a no-op.
    if (!_collName) {
        return;
    }

    tassert(6610202, "Catalog epoch should be initialized", _catalogEpoch);
    _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);

    auto indexCatalogEntry = _weakIndexCatalogEntry.lock();
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _columnIndexName << "' dropped",
            indexCatalogEntry && !indexCatalogEntry->isDropped());

    if (_rowStoreCursor) {
        if (relinquishCursor) {
            const bool couldRestore = _rowStoreCursor->restore();
            invariant(couldRestore);
        }
    }

    if (_recordIdColumnCursor) {
        _recordIdColumnCursor->cursor().restore();
    }
    for (auto& cursor : _columnCursors) {
        cursor.cursor().restore();
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->cursor().restore();
    }
}

void ColumnScanStage::doDetachFromOperationContext() {
    if (_rowStoreCursor) {
        _rowStoreCursor->detachFromOperationContext();
    }
    if (_recordIdColumnCursor) {
        _recordIdColumnCursor->cursor().detachFromOperationContext();
    }
    for (auto& cursor : _columnCursors) {
        cursor.cursor().detachFromOperationContext();
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->cursor().detachFromOperationContext();
    }
}

void ColumnScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_rowStoreCursor) {
        _rowStoreCursor->reattachToOperationContext(opCtx);
    }
    if (_recordIdColumnCursor) {
        _recordIdColumnCursor->cursor().reattachToOperationContext(opCtx);
    }
    for (auto& cursor : _columnCursors) {
        cursor.cursor().reattachToOperationContext(opCtx);
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->cursor().reattachToOperationContext(opCtx);
    }
}

void ColumnScanStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask ColumnScanStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    _tracker = tracker;
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToStreamingStage;
}

void ColumnScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    invariant(_opCtx);

    if (_open) {
        tassert(6610203, "reopened ColumnScanStage but reOpen=false", reOpen);
        tassert(6610204, "ColumnScanStage is open but _coll is null", _coll);
        tassert(6610205, "ColumnScanStage is open but don't have _rowStoreCursor", _rowStoreCursor);
    } else {
        tassert(6610206, "first open to ColumnScanStage but reOpen=true", !reOpen);
        if (!_coll) {
            // We're being opened after 'close()'. We need to re-acquire '_coll' in this case and
            // make some validity checks (the collection has not been dropped, renamed, etc.).
            tassert(
                6610207, "ColumnScanStage is not open but have _rowStoreCursor", !_rowStoreCursor);
            tassert(6610208, "Collection name should be initialized", _collName);
            tassert(6610209, "Catalog epoch should be initialized", _catalogEpoch);
            _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);
        }
    }

    if (!_rowStoreCursor) {
        _rowStoreCursor = _coll->getCursor(_opCtx, true /* forward */);
    }

    if (_columnCursors.empty()) {
        auto entry = _weakIndexCatalogEntry.lock();
        tassert(6610210,
                str::stream() << "expected IndexCatalogEntry for index named: " << _columnIndexName,
                static_cast<bool>(entry));

        auto iam = static_cast<ColumnStoreAccessMethod*>(entry->accessMethod());

        for (size_t i = 0; i < _paths.size(); i++) {
            _columnCursors.emplace_back(
                iam->storage()->newCursor(_opCtx, _paths[i]),
                _specificStats.cursorStats.emplace_back(_paths[i], _includeInOutput[i]));
        }

        // The dense _recordId column is only needed if there are no filters and no dense field is
        // being scanned already for the query.
        if (_filteredPaths.empty() && !_densePathIncludedInScan) {
            _recordIdColumnCursor = std::make_unique<ColumnCursor>(
                iam->storage()->newCursor(_opCtx, ColumnStore::kRowIdPath),
                _specificStats.cursorStats.emplace_back(ColumnStore::kRowIdPath.toString(),
                                                        false /*includeInOutput*/));
        }
    }

    _rowId = ColumnStore::kNullRowId;
    _open = true;
}

TranslatedCell ColumnScanStage::translateCell(PathView path, const SplitCellView& splitCellView) {
    SplitCellView::Cursor<value::ColumnStoreEncoder> cellCursor =
        splitCellView.subcellValuesGenerator<value::ColumnStoreEncoder>(&_encoder);
    return TranslatedCell{splitCellView.arrInfo, path, std::move(cellCursor)};
}

void ColumnScanStage::readParentsIntoObj(StringData path,
                                         value::Object* outObj,
                                         StringDataSet* pathsReadSetOut) {
    auto parent = ColumnStore::getParentPath(path);

    // If a top-level path doesn't exist, it just doesn't exist. It can't exist in some places
    // within a document but not others. No further inspection is necessary.
    if (!parent) {
        return;
    }

    if (pathsReadSetOut->contains(*parent)) {
        // We've already read the parent in, so skip it.
        return;
    }

    // Create the parent path cursor if necessary.

    // First we try to emplace a nullptr, so that we avoid creating the cursor when we don't have
    // to.
    auto [it, inserted] = _parentPathCursors.try_emplace(*parent, nullptr);

    // If we inserted a new entry, replace the null with an actual cursor.
    if (inserted) {
        invariant(!it->second);
        auto entry = _weakIndexCatalogEntry.lock();
        tassert(6610211,
                str::stream() << "expected IndexCatalogEntry for index named: " << _columnIndexName,
                static_cast<bool>(entry));
        auto iam = static_cast<ColumnStoreAccessMethod*>(entry->accessMethod());

        it->second =
            std::make_unique<ColumnCursor>(iam->storage()->newCursor(_opCtx, *parent),
                                           _specificStats.parentCursorStats.emplace_back(
                                               parent->toString(), false /* includeInOutput */));
    }

    boost::optional<SplitCellView> splitCellView;
    if (auto optCell = it->second->seekExact(_rowId)) {
        splitCellView = SplitCellView::parse(optCell->value);
    }

    pathsReadSetOut->insert(*parent);
    if (!splitCellView || splitCellView->isSparse) {
        // We need this cell's parent too.
        readParentsIntoObj(*parent, outObj, pathsReadSetOut);
    }

    if (splitCellView) {
        auto translatedCell = translateCell(*parent, *splitCellView);
        addCellToObject(translatedCell, *outObj);
    }
}

// The result of the filter predicate will be the same regardless of sparseness or sub objects,
// therefore we don't look at the parents and don't consult the row store. The filter expression for
// each path should incorporate cell traversal of the cell passed to it.
bool ColumnScanStage::checkFilter(CellView cell, size_t filterIndex, FieldIndex numPathParts) {
    auto splitCellView = SplitCellView::parse(cell);
    value::CsiCell csiCell{&splitCellView, &_encoder, numPathParts};
    _filterInputAccessors[filterIndex].reset(value::TypeTags::csiCell,
                                             sbe::value::bitcastFrom<const void*>(&csiCell));

    return _bytecode.runPredicate(_filterExprsCode[filterIndex].get());
}

RowId ColumnScanStage::findNextRowIdForFilteredColumns() {
    invariant(!_filteredPaths.empty());

    // Initialize 'targetRecordId' from the filtered cursor we are currently iterating.
    RowId targetRowId;
    {
        auto& cursor = cursorForFilteredPath(_filteredPaths[_nextUnmatched]);
        if (!cursor.lastCell()) {
            return ColumnStore::kNullRowId;  // Have exhausted one of the columns.
        }
        targetRowId = cursor.lastCell()->rid;
    }

    size_t matchedSinceAdvance = 0;
    // The loop will terminate because when 'matchedSinceAdvance' is reset the 'targetRecordId'
    // is guaranteed to advance. It will do no more than N 'next()' calls across all cursors,
    // where N is the number of records (might do fewer, if for some columns there are missing
    // values). The number of seeks and filter checks depends on the selectivity of the filters.
    while (matchedSinceAdvance < _filteredPaths.size()) {
        auto& cursor = cursorForFilteredPath(_filteredPaths[_nextUnmatched]);

        // Avoid seeking into the column that we started with.
        auto& result = cursor.lastCell();
        if (result && result->rid < targetRowId) {
            result = cursor.seekAtOrPast(targetRowId);
        }
        if (!result) {
            return ColumnStore::kNullRowId;
        }

        if (result->rid > targetRowId) {
            // The column skipped ahead - have to restart at this new record ID.
            matchedSinceAdvance = 0;
            targetRowId = result->rid;
        }

        if (!checkFilter(result->value, _nextUnmatched, cursor.numPathParts())) {
            // Advance the column until find a match and restart at this new record ID.
            do {
                result = cursor.next();
                if (!result) {
                    return ColumnStore::kNullRowId;
                }
            } while (!checkFilter(result->value, _nextUnmatched, cursor.numPathParts()));
            matchedSinceAdvance = 0;
            invariant(result->rid > targetRowId);
            targetRowId = result->rid;
        }
        ++matchedSinceAdvance;
        _nextUnmatched = (_nextUnmatched + 1) % _filteredPaths.size();
    }
    invariant(targetRowId != ColumnStore::kNullRowId);

    // Ensure that _all_ cursors have caugth up with the filtered record ID. Some of the cursors
    // might skip ahead, which would mean the column is missing a value for this 'recordId'.
    for (auto& cursor : _columnCursors) {
        const auto& result = cursor.lastCell();
        if (result && result->rid < targetRowId) {
            cursor.seekAtOrPast(targetRowId);
        }
    }

    return targetRowId;
}

RowId ColumnScanStage::findMinRowId() const {
    if (_recordIdColumnCursor) {
        // The cursor of the dense _recordId column cannot be ahead of any other (there are no
        // filters on it to move it forward arbitrarily), so it's always at the minimum.
        auto& result = _recordIdColumnCursor->lastCell();
        if (!result) {
            return ColumnStore::kNullRowId;
        }
        return result->rid;
    }

    auto recordId = ColumnStore::kNullRowId;
    for (const auto& cursor : _columnCursors) {
        const auto& result = cursor.lastCell();
        if (result && (recordId == ColumnStore::kNullRowId || result->rid < recordId)) {
            recordId = result->rid;
        }
    }
    return recordId;
}

/**
 * When called for the first time, initializes all the column cursors to the beginning of their
 * respective columns. On subsequent calls, if path filters are present, forwards all cursors to
 * their next filter match. If no filters are present, cursors are stepped forward passed the
 * current _rowId, if necessary: there may be gaps in columns, putting one cursor far ahead of the
 * others in past cursor advancement.
 *
 * Returns the lowest RowId across cursors if there are no filtered paths; otherwise the RowId of
 * the current cursors' position where all filters match.
 */
RowId ColumnScanStage::advanceColumnCursors(bool reset) {
    if (_rowId == ColumnStore::kNullRowId) {
        if (_recordIdColumnCursor) {
            _recordIdColumnCursor->seekAtOrPast(ColumnStore::kNullRowId);
        }
        for (auto& columnCursor : _columnCursors) {
            columnCursor.seekAtOrPast(ColumnStore::kNullRowId);
        }
        return _filteredPaths.empty() ? findMinRowId() : findNextRowIdForFilteredColumns();
    }

    if (reset) {
        if (_recordIdColumnCursor) {
            _recordIdColumnCursor->seekAtOrPast(_rowId);
        }
        for (auto& cursor : _columnCursors) {
            cursor.seekAtOrPast(_rowId);
        }
    }

    if (!_filteredPaths.empty()) {
        // Nudge forward the "active" filtered cursor. The remaining ones will be synchronized
        // by 'findNextRecordIdForFilteredColumns()'.
        cursorForFilteredPath(_filteredPaths[_nextUnmatched]).next();
        return findNextRowIdForFilteredColumns();
    }

    /* no filtered paths */

    // In absence of filters all cursors iterate forward on their own. Some of the cursors might
    // be ahead of the current '_rowId' because there are gaps in their columns: don't move them,
    // unless they are at '_rowId' and therefore their values have been consumed.
    // While at it, compute the new min row ID.
    auto nextRowId = ColumnStore::kNullRowId;

    if (_recordIdColumnCursor) {
        tassert(6859101,
                "The dense _recordId cursor should always be at the current minimum record ID",
                _recordIdColumnCursor->lastCell()->rid == _rowId);
        auto cell = _recordIdColumnCursor->next();
        if (!cell) {
            return ColumnStore::kNullRowId;
        }
        nextRowId = cell->rid;
    }
    for (auto& cursor : _columnCursors) {
        auto& cell = cursor.lastCell();
        if (!cell) {
            continue;  // this column has been exhausted
        }
        if (cell->rid == _rowId) {
            cell = cursor.next();
        }
        if (cell && (nextRowId == ColumnStore::kNullRowId || cell->rid < nextRowId)) {
            tassert(6859102,
                    "The dense _recordId cursor should have the next lowest record ID",
                    !_recordIdColumnCursor);
            nextRowId = cell->rid;
        }
    }
    return nextRowId;
}

PlanState ColumnScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call next() on a storage cursor so do not bother saving our internal
    // state in case it yields as the state will be completely overwritten after the next()
    // call.
    disableSlotAccess();

    checkForInterrupt(_opCtx);

    if (_scanTracker.isScanningRowstore()) {
        _scanTracker.track();

        auto record = _rowStoreCursor->next();
        if (!record) {
            return trackPlanState(PlanState::IS_EOF);
        }
        ++_specificStats.numRowStoreScans;

        _rowId = record->id.getLong();
        processRecordFromRowstore(*record);
    } else {
        // When we are scanning the row store, we let the column cursors fall behind, so on exiting
        // from the scan mode, they must be reset to the current _rowId, before they can advance to
        // the next.
        _rowId = advanceColumnCursors(_scanTracker.isFinishingScan() /* reset */);

        if (_rowId == ColumnStore::kNullRowId) {
            return trackPlanState(PlanState::IS_EOF);
        }

        // Attempt to reconstruct the object from the index. If we cannot do this, we'll fetch the
        // corresponding record from the row store and will enter the row store scanning mode for
        // the next few records.
        bool canReconstructFromIndex = true;

        auto [outTag, outVal] = value::makeNewObject();
        auto& outObj = *value::bitcastTo<value::Object*>(outVal);
        value::ValueGuard materializedObjGuard(outTag, outVal);

        StringDataSet pathsRead;
        for (size_t i = 0; i < _columnCursors.size(); ++i) {
            if (!_includeInOutput[i]) {
                continue;
            }
            auto& cursor = _columnCursors[i];
            auto& lastCell = cursor.lastCell();

            boost::optional<SplitCellView> splitCellView;
            if (lastCell && lastCell->rid == _rowId) {
                splitCellView = SplitCellView::parse(lastCell->value);
            }

            const auto& path = cursor.path();

            if (splitCellView &&
                (splitCellView->hasSubPaths || splitCellView->hasDuplicateFields)) {
                canReconstructFromIndex = false;
                break;
            } else {
                if (!splitCellView || splitCellView->isSparse) {
                    // Must read in the parent information first.
                    readParentsIntoObj(path, &outObj, &pathsRead);
                }
                if (splitCellView) {
                    auto translatedCell = translateCell(path, *splitCellView);
                    addCellToObject(translatedCell, outObj);
                    pathsRead.insert(path);
                }
            }
        }

        if (!canReconstructFromIndex) {
            ++_specificStats.numRowStoreFetches;

            // Enter the row store scanning mode for the next few reads.
            if (_filteredPaths.empty()) {
                _scanTracker.startNextBatch();
            }

            // It's possible that we are reading a long range of records that cannot be served from
            // the index. We could add more state tracking to keep scanning the row store even when
            // doing the checkpoint at the end of a batch, but we don't think it's helpful because
            // the hit of adjusting the column cursors and attempting to reconstruct the object is
            // likely much bigger than seek() vs next() on the row store.
            auto record = _rowStoreCursor->seekExact(RecordId(_rowId));
            tassert(6859103, "Row store must be in sync with the index", record);

            processRecordFromRowstore(*record);
        } else {
            _scanTracker.reset();

            if (_reconstructedRecordAccessor) {
                _reconstructedRecordAccessor->reset(true, outTag, outVal);
            }
            materializedObjGuard.reset();
        }
    }

    if (_recordIdAccessor) {
        _recordId = RecordId(_rowId);
        _recordIdAccessor->reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
    }

    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
        // If we're collecting execution stats during multi-planning and reached the end of the
        // trial period because we've performed enough physical reads, bail out from the trial
        // run by raising a special exception to signal a runtime planner that this candidate
        // plan has completed its trial run early. Note that a trial period is executed only
        // once per a PlanStage tree, and once completed never run again on the same tree.
        _tracker = nullptr;
        uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in scan");
    }

    return trackPlanState(PlanState::ADVANCED);
}

void ColumnScanStage::processRecordFromRowstore(const Record& record) {
    _rowStoreAccessor->reset(
        false, value::TypeTags::bsonObject, value::bitcastFrom<const char*>(record.data.data()));

    if (_reconstructedRecordAccessor) {
        if (_rowStoreExpr) {
            auto [owned, tag, val] = _bytecode.run(_rowStoreExprCode.get());
            _reconstructedRecordAccessor->reset(owned, tag, val);
        } else {
            _reconstructedRecordAccessor->reset(
                false,
                value::TypeTags::bsonObject,
                value::bitcastFrom<const char*>(record.data.data()));
        }
    }
}

void ColumnScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _rowStoreCursor.reset();
    _coll.reset();
    _columnCursors.clear();
    _parentPathCursors.clear();
    _recordIdColumnCursor.reset();
    _open = false;
}

std::unique_ptr<PlanStageStats> ColumnScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ColumnScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("columnIndexName", _columnIndexName);
        bob.appendNumber("numRowStoreFetches",
                         static_cast<long long>(_specificStats.numRowStoreFetches));
        bob.appendNumber("numRowStoreScans",
                         static_cast<long long>(_specificStats.numRowStoreScans));
        BSONObjBuilder columns(bob.subobjStart("columns"));
        for (const ColumnScanStats::CursorStats& cursorStat : _specificStats.cursorStats) {
            StringData path = cursorStat.path;
            if (path == ColumnStore::kRowIdPath) {
                path = "<<RowId Column>>";
            }
            BSONObjBuilder column(columns.subobjStart(path));
            column.appendBool("usedInOutput", cursorStat.includeInOutput);
            column.appendNumber("numNexts", static_cast<long long>(cursorStat.numNexts));
            column.appendNumber("numSeeks", static_cast<long long>(cursorStat.numSeeks));
            column.done();
        }
        columns.done();

        BSONObjBuilder parentColumns(bob.subobjStart("parentColumns"));
        for (const ColumnScanStats::CursorStats& cursorStat : _specificStats.parentCursorStats) {
            StringData path = cursorStat.path;
            BSONObjBuilder column(parentColumns.subobjStart(path));
            column.appendNumber("numNexts", static_cast<long long>(cursorStat.numNexts));
            column.appendNumber("numSeeks", static_cast<long long>(cursorStat.numSeeks));
            column.done();
        }
        parentColumns.done();

        bob.append("paths", _paths);

        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* ColumnScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> ColumnScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    if (_reconstructedRecordSlot) {
        DebugPrinter::addIdentifier(ret, _reconstructedRecordSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    // Print out paths.
    ret.emplace_back(DebugPrinter::Block("paths[`"));
    for (size_t idx = 0; idx < _paths.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        ret.emplace_back(str::stream() << "\"" << _paths[idx] << "\"");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print out output paths.
    ret.emplace_back(DebugPrinter::Block("outputs[`"));
    bool first = true;
    for (size_t idx = 0; idx < _paths.size(); ++idx) {
        if (_includeInOutput[idx]) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            } else {
                first = false;
            }

            ret.emplace_back(str::stream() << "\"" << _paths[idx] << "\"");
        }
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print out per-path filters.
    ret.emplace_back(DebugPrinter::Block("pathFilters[`"));
    for (size_t idx = 0; idx < _filteredPaths.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`;"));
        }

        ret.emplace_back(str::stream() << "\"" << _paths[_filteredPaths[idx].pathIndex] << "\": ");
        DebugPrinter::addIdentifier(ret, _filteredPaths[idx].inputSlotId);
        ret.emplace_back(DebugPrinter::Block("`,"));
        DebugPrinter::addBlocks(ret, _filteredPaths[idx].filterExpr->debugPrint());
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print out rowStoreExpression as [rowStoreSlot, rowStoreExpression]
    ret.emplace_back(DebugPrinter::Block("rowStoreExpr[`"));
    if (_rowStoreExpr) {
        DebugPrinter::addIdentifier(ret, _rowStoreSlot);
        ret.emplace_back(DebugPrinter::Block("`,"));
        DebugPrinter::addBlocks(ret, _rowStoreExpr->debugPrint());
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _columnIndexName);
    ret.emplace_back("`\"");

    return ret;
}

size_t ColumnScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_paths);
    size += size_estimator::estimate(_specificStats);
    return size;
}
}  // namespace sbe
}  // namespace mongo
