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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/index/columns_access_method.h"

namespace mongo {
namespace sbe {
ColumnScanStage::ColumnScanStage(UUID collectionUuid,
                                 StringData columnIndexName,
                                 std::vector<std::string> paths,
                                 std::vector<bool> includeInOutput,
                                 boost::optional<value::SlotId> recordIdSlot,
                                 boost::optional<value::SlotId> reconstuctedRecordSlot,
                                 value::SlotId rowStoreSlot,
                                 std::unique_ptr<EExpression> rowStoreExpr,
                                 std::vector<PathFilter> filteredPaths,
                                 PlanYieldPolicy* yieldPolicy,
                                 PlanNodeId nodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("COLUMN_SCAN"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _collUuid(collectionUuid),
      _columnIndexName(columnIndexName),
      _paths(std::move(paths)),
      _includeInOutput(std::move(includeInOutput)),
      _recordIdSlot(recordIdSlot),
      _reconstructedRecordSlot(reconstuctedRecordSlot),
      _rowStoreSlot(rowStoreSlot),
      _rowStoreExpr(std::move(rowStoreExpr)),
      _filteredPaths(std::move(filteredPaths)) {
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
                          << "' in collection '" << _collName << "'",
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
    if (_denseColumnCursor) {
        _denseColumnCursor->makeOwned();
        _denseColumnCursor->cursor().save();
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

    if (_denseColumnCursor) {
        _denseColumnCursor->cursor().restore();
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
    if (_denseColumnCursor) {
        _denseColumnCursor->cursor().detachFromOperationContext();
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
    if (_denseColumnCursor) {
        _denseColumnCursor->cursor().reattachToOperationContext(opCtx);
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

        // The dense _recordId column is only needed if there are no filters (TODO SERVER-68377:
        // eventually we can avoid including this column for the cases where a known dense column
        // such as _id is being read anyway).
        if (_filteredPaths.empty()) {
            _denseColumnCursor = std::make_unique<ColumnCursor>(
                iam->storage()->newCursor(_opCtx, ColumnStore::kRowIdPath),
                _specificStats.cursorStats.emplace_back(ColumnStore::kRowIdPath.toString(),
                                                        false /*includeInOutput*/));
        }
        for (size_t i = 0; i < _paths.size(); i++) {
            _columnCursors.emplace_back(
                iam->storage()->newCursor(_opCtx, _paths[i]),
                _specificStats.cursorStats.emplace_back(_paths[i], _includeInOutput[i]));
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
// therefore we don't look at the parents and don't consult the row store.
// (TODO SERVER-68792) Refactor the iteration over values into its own type.
bool ColumnScanStage::checkFilter(CellView cell, size_t filterIndex, const PathValue& path) {
    auto splitCellView = SplitCellView::parse(cell);
    auto translatedCell = translateCell(path, splitCellView);

    if (!translatedCell.moreValues()) {
        return false;
    }

    if (translatedCell.arrInfo.empty()) {
        // Have a single non-nested value -- evaluate the filter on it.
        // (TODO SERVER-68792) Could we avoid copying by using a non-owning accessor? Same question
        // for other locations in this function when the predicate is evaluated immediately after
        // setting the slot.
        auto [tag, val] = translatedCell.nextValue();
        _filterInputAccessors[filterIndex].reset(tag, val);
        return _bytecode.runPredicate(_filterExprsCode[filterIndex].get());
    } else {
        ArrInfoReader arrInfoReader{translatedCell.arrInfo};
        int depth = 0;

        // Avoid allocating memory for this stack except in the rare case of deeply nested
        // documents.
        std::stack<bool, absl::InlinedVector<bool, 64>> inArray;
        while (arrInfoReader.moreExplicitComponents()) {
            switch (arrInfoReader.takeNextChar()) {
                case '{': {
                    // We consider as nested only the arrays that are elements of other arrays. When
                    // there is an array of objects and some of the fields of these objects are
                    // arrays, the latter aren't nested.
                    inArray.push(false);
                    break;
                }
                case '[': {
                    // A '[' can be followed by a number if there are objects in the array, that
                    // should be retrieved from other paths when reconstructing the record. We can
                    // ignore them as they don't contribute to the values.
                    (void)arrInfoReader.takeNumber();
                    if (!inArray.empty() && inArray.top()) {
                        depth++;
                    }
                    inArray.push(true);
                    break;
                }
                case '+': {
                    // Indicates elements in arrays that are objects that don't have the path. These
                    // objects don't contribute to the cell's values, so we can ignore them.
                    (void)arrInfoReader.takeNumber();
                    break;
                }
                case '|': {
                    auto repeats = arrInfoReader.takeNumber();
                    for (size_t i = 0; i < repeats + 1; i++) {
                        auto [tag, val] = translatedCell.nextValue();
                        if (depth == 0) {
                            _filterInputAccessors[filterIndex].reset(tag, val);
                            if (_bytecode.runPredicate(_filterExprsCode[filterIndex].get())) {
                                return true;
                            }
                        }
                    }
                    break;
                }
                case 'o': {
                    // Indicates the start of a nested object inside the cell. We don't need to
                    // track this info because the nested objects don't contribute to the values in
                    // the cell.
                    (void)arrInfoReader.takeNumber();
                    break;
                }
                case ']': {
                    invariant(inArray.size() > 0 && inArray.top());
                    inArray.pop();
                    if (inArray.size() > 0 && inArray.top()) {
                        invariant(depth > 0);
                        depth--;
                    }

                    // Closing an array implicitly closes all objects on the path between it and the
                    // previous array.
                    while (inArray.size() > 0 && !inArray.top()) {
                        inArray.pop();
                    }
                }
            }
        }
        if (depth == 0) {
            // For the remaining values the depth isn't going to change so we don't need to advance
            // the value iterator if the values are too deep.
            while (translatedCell.moreValues()) {
                auto [tag, val] = translatedCell.nextValue();
                _filterInputAccessors[filterIndex].reset(tag, val);
                if (_bytecode.runPredicate(_filterExprsCode[filterIndex].get())) {
                    return true;
                }
            }
        }
    }

    // None of the values matched the filter.
    return false;
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

        if (!checkFilter(result->value, _nextUnmatched, cursor.path())) {
            // Advance the column until find a match and restart at this new record ID.
            do {
                result = cursor.next();
                if (!result) {
                    return ColumnStore::kNullRowId;
                }
            } while (!checkFilter(result->value, _nextUnmatched, cursor.path()));
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
    if (_denseColumnCursor) {
        // The cursor of the dense column cannot be ahead of any other, so it's always at the
        // minimum.
        auto& result = _denseColumnCursor->lastCell();
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

RowId ColumnScanStage::advanceCursors() {
    if (_rowId == ColumnStore::kNullRowId) {
        if (_denseColumnCursor) {
            _denseColumnCursor->seekAtOrPast(ColumnStore::kNullRowId);
        }
        for (auto& columnCursor : _columnCursors) {
            columnCursor.seekAtOrPast(ColumnStore::kNullRowId);
        }
        return _filteredPaths.empty() ? findMinRowId() : findNextRowIdForFilteredColumns();
    }

    if (!_filteredPaths.empty()) {
        // Nudge forward the "active" filtered cursor. The remaining ones will be synchronized
        // by 'findNextRecordIdForFilteredColumns()'.
        cursorForFilteredPath(_filteredPaths[_nextUnmatched]).next();
        return findNextRowIdForFilteredColumns();
    }

    // In absence of filters all cursors iterate forward on their own. Some of the cursors might
    // be ahead of the current '_rowId' because there are gaps in their columns - don't move them
    // but only those that are at '_rowId' and therefore their values have been consumed.
    // While at it, compute the new min row ID. auto nextRecordId = RecordId();
    auto nextRowId = ColumnStore::kNullRowId;
    if (_denseColumnCursor) {
        invariant(_denseColumnCursor->lastCell()->rid == _rowId,
                  "Dense cursor should always be at the current minimum record ID");
        auto cell = _denseColumnCursor->next();
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
            invariant(!_denseColumnCursor, "Dense cursor should have the next lowest record ID");
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

    _rowId = advanceCursors();
    if (_rowId == ColumnStore::kNullRowId) {
        return trackPlanState(PlanState::IS_EOF);
    }

    bool useRowStore = false;

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

        if (!useRowStore) {
            if (splitCellView &&
                (splitCellView->hasSubPaths || splitCellView->hasDuplicateFields)) {
                useRowStore = true;
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
    }

    if (useRowStore) {
        ++_specificStats.numRowStoreFetches;
        // TODO: In some cases we can avoid calling seek() on the row store cursor, and instead do
        // a next() which should be much cheaper.
        auto record = _rowStoreCursor->seekExact(RecordId(_rowId));

        // If there's no record, the index is out of sync with the row store.
        invariant(record);

        _rowStoreAccessor->reset(false,
                                 value::TypeTags::bsonObject,
                                 value::bitcastFrom<const char*>(record->data.data()));

        if (_reconstructedRecordAccessor) {
            // TODO: in absence of record expression set the reconstructed record to be the same
            // as the record, retrieved from the row store.
            invariant(_rowStoreExpr);
            auto [owned, tag, val] = _bytecode.run(_rowStoreExprCode.get());
            _reconstructedRecordAccessor->reset(owned, tag, val);
        }
    } else {
        if (_reconstructedRecordAccessor) {
            _reconstructedRecordAccessor->reset(true, outTag, outVal);
        }
        materializedObjGuard.reset();
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

void ColumnScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _rowStoreCursor.reset();
    _coll.reset();
    _columnCursors.clear();
    _parentPathCursors.clear();
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
        BSONObjBuilder columns(bob.subobjStart("columns"));
        for (ColumnScanStats::CursorStats cursorStat : _specificStats.cursorStats) {
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
        for (ColumnScanStats::CursorStats cursorStat : _specificStats.parentCursorStats) {
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
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _paths.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        ret.emplace_back(str::stream() << "\"" << _paths[idx] << "\"");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print out per-path filters (if any).
    if (!_filteredPaths.empty()) {
        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t idx = 0; idx < _filteredPaths.size(); ++idx) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`;"));
            }

            ret.emplace_back(str::stream()
                             << "\"" << _paths[_filteredPaths[idx].pathIndex] << "\": ");
            DebugPrinter::addIdentifier(ret, _filteredPaths[idx].inputSlotId);
            ret.emplace_back(DebugPrinter::Block("`,"));
            DebugPrinter::addBlocks(ret, _filteredPaths[idx].filterExpr->debugPrint());
        }
        ret.emplace_back(DebugPrinter::Block("`]"));
    }

    if (_rowStoreExpr) {
        ret.emplace_back(DebugPrinter::Block("[`"));
        DebugPrinter::addIdentifier(ret, _rowStoreSlot);
        ret.emplace_back(DebugPrinter::Block("`,"));
        DebugPrinter::addBlocks(ret, _rowStoreExpr->debugPrint());
        ret.emplace_back(DebugPrinter::Block("`]"));
    }

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
