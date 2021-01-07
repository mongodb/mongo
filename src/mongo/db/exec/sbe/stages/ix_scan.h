/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/ordering.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/sbe/stages/lock_acquisition_callback.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo::sbe {
/**
 * A stage that iterates the entries of a collection index, starting from a bound specified by the
 * value in 'seekKeySlotLow' and ending (via IS_EOF) with the 'seekKeySlotHigh' bound. (An
 * unspecified 'seekKeySlotHigh' scans to the end of the index. Leaving both bounds unspecified
 * scans the index from beginning to end.)
 *
 * The input 'seekKeySlotLow' and 'seekKeySlotHigh' slots get read as part of the open (or re-open)
 * call. A common use case for an IndexScanStage is to place it as the inner child of LoopJoinStage.
 * The outer side of the LoopJoinStage determines the bounds, and the inner IndexScanStage iterates
 * through all the entries within those bounds.
 *
 * The "output" slots are
 *   - 'recordSlot': the "KeyString" representing the index entry,
 *   - 'recordIdSlot': a reference that can be used to fetch the entire document, and
 *   - 'vars': one slot for each value in the index key that should be "projected" out of the entry.
 *
 * The 'indexKeysToInclude' bitset determines which values are included in the projection based
 * on their order in the index pattern. The number of bits set in 'indexKeysToInclude' must be
 * the same as the number of slots in the 'vars' SlotVector.
 */
class IndexScanStage final : public PlanStage {
public:
    IndexScanStage(const NamespaceStringOrUUID& name,
                   std::string_view indexName,
                   bool forward,
                   boost::optional<value::SlotId> recordSlot,
                   boost::optional<value::SlotId> recordIdSlot,
                   IndexKeysInclusionSet indexKeysToInclude,
                   value::SlotVector vars,
                   boost::optional<value::SlotId> seekKeySlotLow,
                   boost::optional<value::SlotId> seekKeySlotHigh,
                   PlanYieldPolicy* yieldPolicy,
                   PlanNodeId nodeId,
                   LockAcquisitionCallback lockAcquisitionCallback);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doDetachFromTrialRunTracker() override;
    void doAttachToTrialRunTracker(TrialRunTracker* tracker) override;

private:
    const NamespaceStringOrUUID _name;
    const std::string _indexName;
    const bool _forward;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const IndexKeysInclusionSet _indexKeysToInclude;
    const value::SlotVector _vars;
    const boost::optional<value::SlotId> _seekKeySlotLow;
    const boost::optional<value::SlotId> _seekKeySlotHigh;

    LockAcquisitionCallback _lockAcquisitionCallback;

    std::unique_ptr<value::ViewOfValueAccessor> _recordAccessor;
    std::unique_ptr<value::ViewOfValueAccessor> _recordIdAccessor;

    // One accessor and slot for each key component that this stage will bind from an index entry's
    // KeyString. The accessors are in the same order as the key components they bind to.
    std::vector<value::ViewOfValueAccessor> _accessors;
    value::SlotAccessorMap _accessorMap;

    value::SlotAccessor* _seekKeyLowAccessor{nullptr};
    value::SlotAccessor* _seekKeyHiAccessor{nullptr};

    KeyString::Value _startPoint;
    KeyString::Value* _seekKeyLow{nullptr};
    KeyString::Value* _seekKeyHi{nullptr};

    std::unique_ptr<SortedDataInterface::Cursor> _cursor;
    std::weak_ptr<const IndexCatalogEntry> _weakIndexCatalogEntry;
    boost::optional<Ordering> _ordering{boost::none};
    boost::optional<AutoGetCollectionForReadMaybeLockFree> _coll;
    boost::optional<KeyStringEntry> _nextRecord;

    // This buffer stores values that are projected out of the index entry. Values in the
    // '_accessors' list that are pointers point to data in this buffer.
    BufBuilder _valuesBuffer;

    bool _open{false};
    bool _firstGetNext{true};
    IndexScanStats _specificStats;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};
};
}  // namespace mongo::sbe
