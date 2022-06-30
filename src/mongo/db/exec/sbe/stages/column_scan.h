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

#include "mongo/config.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/storage/column_store.h"

namespace mongo {
namespace sbe {
/**
 * A stage that scans provided columnar index. A set of paths is retrieved from the index and values
 * are put in output slots.
 */
class ColumnScanStage final : public PlanStage {
public:
    ColumnScanStage(UUID collectionUuid,
                    StringData columnIndexName,
                    value::SlotVector fieldSlots,
                    std::vector<std::string> paths,
                    boost::optional<value::SlotId> recordSlot,
                    boost::optional<value::SlotId> recordIdSlot,
                    std::unique_ptr<EExpression> internalExpr,
                    std::vector<std::unique_ptr<EExpression>> pathExprs,
                    value::SlotId internalSlot,
                    PlanYieldPolicy* yieldPolicy,
                    PlanNodeId planNodeId,
                    bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doSaveState(bool relinquishCursor) override;
    void doRestoreState(bool relinquishCursor) override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doDetachFromTrialRunTracker() override;
    TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) override;

private:
    /**
     * A representation of a cursor for one column.
     * This object also maintains statistics for how many times this column was accessed.
     */
    class ColumnCursor {
    public:
        /**
         * The '_stats' object must outlive this 'ColumnCursor'.
         */
        ColumnCursor(std::unique_ptr<ColumnStore::CursorForPath> cursor,
                     ColumnScanStats::CursorStats& stats)
            : _cursor(std::move(cursor)), _stats(stats) {}


        boost::optional<FullCellView>& next() {
            // TODO For some reason the destructor of 'lastCell' is not called
            // on my local asan build unless we explicitly reset it. Maybe
            // the same compiler bug Nikita ran into?
            _lastCell.reset();
            _lastCell = _cursor->next();
            clearOwned();
            ++_stats.numNexts;
            return _lastCell;
        }

        boost::optional<FullCellView>& seekAtOrPast(RecordId id) {
            _lastCell.reset();
            _lastCell = _cursor->seekAtOrPast(id);
            clearOwned();
            ++_stats.numSeeks;
            return _lastCell;
        }

        boost::optional<FullCellView>& seekExact(RecordId id) {
            _lastCell.reset();
            _lastCell = _cursor->seekExact(id);
            clearOwned();
            ++_stats.numSeeks;
            return _lastCell;
        }

        const PathValue& path() const {
            return _cursor->path();
        }

        /*
         * Copies any data owned by the storage engine into a locally owned buffer.
         */
        void makeOwned() {
            if (_lastCell && _pathOwned.empty() && _cellOwned.empty()) {
                _pathOwned.insert(
                    _pathOwned.begin(), _lastCell->path.begin(), _lastCell->path.end());
                _lastCell->path = StringData(_pathOwned);

                _cellOwned.insert(
                    _cellOwned.begin(), _lastCell->value.begin(), _lastCell->value.end());
                _lastCell->value = StringData(_cellOwned.data(), _cellOwned.size());
            }
        }
        ColumnStore::CursorForPath& cursor() {
            return *_cursor;
        }
        bool includeInOutput() const {
            return _stats.includeInOutput;
        }
        boost::optional<FullCellView>& lastCell() {
            return _lastCell;
        }

        size_t numNexts() const {
            return _stats.numNexts;
        }

        size_t numSeeks() const {
            return _stats.numSeeks;
        }

    private:
        void clearOwned() {
            _pathOwned.clear();
            _cellOwned.clear();
        }

        std::unique_ptr<ColumnStore::CursorForPath> _cursor;

        boost::optional<FullCellView> _lastCell;

        // These members are used to store owned copies of the path and the cell data when preparing
        // for yield.
        std::string _pathOwned;
        std::vector<char> _cellOwned;

        // The _stats must outlive this.
        ColumnScanStats::CursorStats& _stats;
    };

    void readParentsIntoObj(StringData path,
                            value::Object* out,
                            StringDataSet* pathsReadSetOut,
                            bool first = true);

    const UUID _collUuid;
    const std::string _columnIndexName;
    const value::SlotVector _fieldSlots;
    const std::vector<std::string> _paths;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;

    // An optional expression used to reconstruct the output document.
    const std::unique_ptr<EExpression> _recordExpr;
    // Expressions to get values from the row store document.
    const std::vector<std::unique_ptr<EExpression>> _pathExprs;
    // An internal slot that points to the row store document.
    const value::SlotId _rowStoreSlot;

    std::vector<value::OwnedValueAccessor> _outputFields;
    value::SlotAccessorMap _outputFieldsMap;
    std::unique_ptr<value::OwnedValueAccessor> _recordAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _recordIdAccessor;

    std::unique_ptr<vm::CodeFragment> _recordExprCode;
    std::vector<std::unique_ptr<vm::CodeFragment>> _pathExprsCode;
    std::unique_ptr<value::OwnedValueAccessor> _rowStoreAccessor;

    vm::ByteCode _bytecode;

    // These members are default constructed to boost::none and are initialized when 'prepare()'
    // is called. Once they are set, they are never modified again.
    boost::optional<NamespaceString> _collName;
    boost::optional<uint64_t> _catalogEpoch;

    CollectionPtr _coll;

    std::weak_ptr<const IndexCatalogEntry> _weakIndexCatalogEntry;
    std::unique_ptr<SeekableRecordCursor> _rowStoreCursor;

    std::vector<ColumnCursor> _columnCursors;
    StringMap<std::unique_ptr<ColumnCursor>> _parentPathCursors;

    RecordId _recordId;

    bool _open{false};

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    ColumnScanStats _specificStats;
};
}  // namespace sbe
}  // namespace mongo
