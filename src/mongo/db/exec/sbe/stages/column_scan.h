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
 * A stage that scans provided columnar index.
 *
 * Currently the stage produces an object into the 'recordSlot' such that accessing any of the given
 * paths in it would be equivalent to accessing the paths in the corresponding object from the
 * associated row store. In the future the stage will be extended to produce separate outputs for
 * each path without materializing this intermediate object unless requested by the client.
 *
 * Debug string representation:
 *
 *  COLUMN_SCAN recordSlot|none recordIdSlot|none [path_1, ..., path_n] collectionUuid indexName
 */
class ColumnScanStage final : public PlanStage {
public:
    ColumnScanStage(UUID collectionUuid,
                    StringData columnIndexName,
                    std::vector<std::string> paths,
                    boost::optional<value::SlotId> recordIdSlot,
                    boost::optional<value::SlotId> reconstructedRecordSlot,
                    value::SlotId rowStoreSlot,
                    std::unique_ptr<EExpression> rowStoreExpr,
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

    void readParentsIntoObj(StringData path, value::Object* out, StringDataSet* pathsReadSetOut);

    // The columnar index this stage is scanning and the associated row store collection.
    const UUID _collUuid;
    const std::string _columnIndexName;
    CollectionPtr _coll;
    boost::optional<NamespaceString> _collName;  // These two members are initialized in 'prepare()'
    boost::optional<uint64_t> _catalogEpoch;     // and are not changed afterwards.
    std::weak_ptr<const IndexCatalogEntry> _weakIndexCatalogEntry;

    // Paths to be read from the index.
    const std::vector<std::string> _paths;

    // The record id in the row store that is used to connect the per-path entries in the columnar
    // index and to retrieve the full record from the row store, if necessary. Because we put into
    // the slot the address of record id, we must guarantee that its lifetime is as long as the
    // stage's.
    RecordId _recordId;
    const boost::optional<value::SlotId> _recordIdSlot;

    // The object that is equivalent to the record from the associated row store when accessing
    // the provided paths. The object might be reconstructed from the index or it might be retrieved
    // from the row store (in which case it can be transformed with '_rowStoreExpr').
    // It's optional because in the future the stage will expose slots with results for individual
    // paths which would make materializing the reconstructed record unnecesary in many cases.
    const boost::optional<value::SlotId> _reconstructedRecordSlot;

    // Sometimes, populating the outputs from the index isn't possible and instead the full record
    // is retrieved from the collection this index is for, that is, from the associated "row store".
    // This full record is placed into the '_rowStoreSlot' and can be transformed using
    // '_rowStoreExpr' before producing the outputs. The client is responsible for ensuring that the
    // outputs after the transformation still satisfy the equivalence requirement for accessing the
    // paths on them vs on the original record.
    const value::SlotId _rowStoreSlot;
    const std::unique_ptr<EExpression> _rowStoreExpr;

    std::unique_ptr<value::OwnedValueAccessor> _reconstructedRecordAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _recordIdAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _rowStoreAccessor;

    vm::ByteCode _bytecode;
    std::unique_ptr<vm::CodeFragment> _rowStoreExprCode;

    // Cursors to simultaneously read from the sections of the index for each path (and, possibly,
    // auxiliary sections) and from the row store.
    std::vector<ColumnCursor> _columnCursors;
    StringMap<std::unique_ptr<ColumnCursor>> _parentPathCursors;
    std::unique_ptr<SeekableRecordCursor> _rowStoreCursor;

    bool _open{false};

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    ColumnScanStats _specificStats;
};
}  // namespace sbe
}  // namespace mongo
