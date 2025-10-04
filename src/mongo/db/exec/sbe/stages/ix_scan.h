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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

/**
 * An abstract index scan stage class to share common code for 'SimpleIndexScanStage' and
 * 'GenericIndexScanStage'.
 *
 * The "output" slots are
 *   - 'indexKeySlot': the "KeyString" representing the index entry,
 *   - 'recordIdSlot': a reference that can be used to fetch the entire document,
 *   - 'snapshotIdSlot': the storage snapshot that this index scan is reading from,
 *   - 'indexIdentSlot': the ident of the index being read from, and
 *   - 'vars': one slot for each value in the index key that should be "projected" out of the entry.
 *
 * The 'indexKeysToInclude' bitset determines which values are included in the projection based
 * on their order in the index pattern. The number of bits set in 'indexKeysToInclude' must be
 * the same as the number of slots in the 'vars' SlotVector.
 *
 * The 'forward' flag indicates the direction of the index scan, which can be either forwards or
 * backwards.
 */
class IndexScanStageBase : public PlanStage {
public:
    IndexScanStageBase(StringData stageType,
                       UUID collUuid,
                       DatabaseName dbName,
                       StringData indexName,
                       bool forward,
                       boost::optional<value::SlotId> indexKeySlot,
                       boost::optional<value::SlotId> recordIdSlot,
                       boost::optional<value::SlotId> snapshotIdSlot,
                       boost::optional<value::SlotId> indexIdentSlot,
                       IndexKeysInclusionSet indexKeysToInclude,
                       value::SlotVector vars,
                       PlanYieldPolicy* yieldPolicy,
                       PlanNodeId planNodeId,
                       bool participateInTrialRunTracking = true);

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const override;
    const SpecificStats* getSpecificStats() const final;
    std::string getIndexName() const;

protected:
    /**
     * Keeps track of what this index scan is currently doing so that it can do the right thing on
     * the next call to getNext().
     */
    enum class ScanState {
        // Need to seek for next key.
        kNeedSeek,
        // Retrieving the next key.
        kScanning,
        // The index scan is finished.
        kFinished
    };

    // Seeks and returns the first/next index KeyStringEntry or boost::none if no such key exists.
    virtual SortedDataKeyValueView seek(RecoveryUnit& ru) = 0;
    // Returns true if the 'key' is within the bounds and false otherwise. Implementations may set
    // state internally to reflect whether the scan is done, or whether a new seek point should be
    // used.
    virtual bool validateKey(const SortedDataKeyValueView& key) = 0;

    void doSaveState() override;
    void doRestoreState() final;
    void doDetachFromOperationContext() final;
    void doAttachToOperationContext(OperationContext* opCtx) final;
    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override;

    /**
     * When this stage is re-opened after being closed, or during yield recovery, called to verify
     * that the index (and the index's collection) remain valid. If any validity check fails, throws
     * a UserException that terminates execution of the query.
     */
    void restoreCollectionAndIndex();
    // Bumps '_specificStats.numReads' and calls base class trackRead().
    void trackIndexRead();
    // Shares the common code for PlanStage::prepare() implementation.
    void prepareImpl(CompileCtx& ctx);
    // Shares the common code for PlanStage::open() implementation.
    void openImpl(bool reOpen);
    // Shares the common code for PlanStage::estimateCompileTimeSize() implementation.
    size_t estimateCompileTimeSizeImpl() const;
    // Shares the common code for PlanStage::debugPrint() implementation.
    void debugPrintImpl(std::vector<DebugPrinter::Block>&) const;

    const UUID _collUuid;
    const DatabaseName _dbName;
    const std::string _indexName;
    const bool _forward;
    const boost::optional<value::SlotId> _indexKeySlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const boost::optional<value::SlotId> _snapshotIdSlot;
    const boost::optional<value::SlotId> _indexIdentSlot;
    const IndexKeysInclusionSet _indexKeysToInclude;
    const value::SlotVector _vars;

    vm::ByteCode _bytecode;

    CollectionRef _coll;

    value::OwnedValueAccessor _recordAccessor;
    value::OwnedValueAccessor _recordIdAccessor;
    value::OwnedValueAccessor _snapshotIdAccessor;

    value::OwnedValueAccessor _indexIdentAccessor;
    value::ViewOfValueAccessor _indexIdentViewAccessor;

    // This field holds the latest snapshot ID that we've received from the recovery unit of the
    // operation. This field gets initialized by prepare(), and it gets updated each time
    // doRestoreState() is called.
    uint64_t _latestSnapshotId{0};

    // One accessor and slot for each key component that this stage will bind from an index entry's
    // KeyString. The accessors are in the same order as the key components they bind to.
    std::vector<value::OwnedValueAccessor> _accessors;
    value::SlotAccessorMap _accessorMap;

    std::unique_ptr<SortedDataInterface::Cursor> _cursor;
    const IndexCatalogEntry* _entry{nullptr};
    boost::optional<Ordering> _ordering{boost::none};
    SortedDataKeyValueView _nextKeyString;
    value::KeyStringEntry _key;

    // This buffer stores values that are projected out of the index entry. Values in the
    // '_accessors' list that are pointers point to data in this buffer.
    BufBuilder _valuesBuffer;

    bool _open{false};
    bool _uniqueIndex{false};
    ScanState _scanState = ScanState::kNeedSeek;
    IndexScanStats _specificStats;
};

/**
 * A stage that iterates the entries of a collection index, starting from a bound specified by the
 * value in 'seekKeyLow' and ending (via IS_EOF) with the 'seekKeyHigh' bound. (A null 'seekKeyHigh'
 * scans to the end of the index. Leaving both bounds as null scans the index from beginning to
 * end.)
 *
 * The input 'seekKeyLow' and 'seekKeyHigh' EExpressions get evaluated as part of the open
 * (or re-open) call. See 'IndexScanStageBase' above for additional information.
 *
 * Debug string representation:
 *
 *   ixscan indexKeySlot? recordIdSlot? snapshotIdSlot? indexIdentSlot?
 *          [slot_1 = fieldNo_1, ..., slot2 = fieldNo_n] collectionUuid indexName forward
 *
 *   ixseek lowKey highKey indexKeySlot? recordIdSlot? snapshotIdSlot? indexIdentSlot?
 *          [slot_1 = fieldNo_1, ..., slot2 = fieldNo_n] collectionUuid indexName forward
 */
class SimpleIndexScanStage final : public IndexScanStageBase {
public:
    SimpleIndexScanStage(UUID collUuid,
                         DatabaseName dbName,
                         StringData indexName,
                         bool forward,
                         boost::optional<value::SlotId> indexKeySlot,
                         boost::optional<value::SlotId> recordIdSlot,
                         boost::optional<value::SlotId> snapshotIdSlot,
                         boost::optional<value::SlotId> indexIdentSlot,
                         IndexKeysInclusionSet indexKeysToInclude,
                         value::SlotVector vars,
                         std::unique_ptr<EExpression> seekKeyLow,
                         std::unique_ptr<EExpression> seekKeyHigh,
                         PlanYieldPolicy* yieldPolicy,
                         PlanNodeId planNodeId,
                         bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const override;

    void prepare(CompileCtx& ctx) override;
    void open(bool reOpen) override;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const override;
    std::vector<DebugPrinter::Block> debugPrint() const override;
    size_t estimateCompileTimeSize() const override;

protected:
    void doSaveState() override;
    SortedDataKeyValueView seek(RecoveryUnit& ru) override;
    bool validateKey(const SortedDataKeyValueView& key) override;

private:
    const key_string::Value& getSeekKeyLow() const;
    const key_string::Value& getSeekKeyHigh() const;

    std::unique_ptr<EExpression> _seekKeyLow;
    std::unique_ptr<EExpression> _seekKeyHigh;

    // Carries the compiled bytecode for the above '_seekKeyLow' and '_seekKeyHigh'.
    std::unique_ptr<vm::CodeFragment> _seekKeyLowCode;
    std::unique_ptr<vm::CodeFragment> _seekKeyHighCode;

    value::OwnedValueAccessor _seekKeyLowHolder;
    value::OwnedValueAccessor _seekKeyHighHolder;

    bool _pointBound{false};
};

/**
 * A stage that finds all keys of a collection index within the given 'IndexBounds'.
 * The index bounds can't be easily resolved to a small set of intervals in advance to use
 * 'SimpleIndexScanStage', thus this implements a runtime algorithm using the 'IndexBoundsChecker'
 * to calculate a seek point and seek to the beginning of the next interval.
 *
 * The input 'params.indexBounds' EExpression gets evaluated as part of the open (or re-open) call.
 *
 * See comments for IndexScanStageBase above for more detail.
 *
 * Debug string representation:
 *
 *   ixscan_generic indexBounds indexKeySlot? recordIdSlot? snapshotIdSlot? indexIdentSlot?
 *                  [slot_1 = fieldNo_1, ..., slot2 = fieldNo_n] collectionUuid indexName forward
 */
struct GenericIndexScanStageParams {
    std::unique_ptr<EExpression> indexBounds;
    const BSONObj keyPattern;
    const int direction;
    const key_string::Version version;
    const Ordering ord;
};
class GenericIndexScanStage final : public IndexScanStageBase {
public:
    GenericIndexScanStage(UUID collUuid,
                          DatabaseName dbName,
                          StringData indexName,
                          GenericIndexScanStageParams params,
                          boost::optional<value::SlotId> indexKeySlot,
                          boost::optional<value::SlotId> recordIdSlot,
                          boost::optional<value::SlotId> snapshotIdSlot,
                          boost::optional<value::SlotId> indexIdentSlot,
                          IndexKeysInclusionSet indexKeysToInclude,
                          value::SlotVector vars,
                          PlanYieldPolicy* yieldPolicy,
                          PlanNodeId planNodeId,
                          bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const override;

    void prepare(CompileCtx& ctx) override;
    void open(bool reOpen) override;

    std::vector<DebugPrinter::Block> debugPrint() const override;
    size_t estimateCompileTimeSize() const override;

protected:
    SortedDataKeyValueView seek(RecoveryUnit& ru) override;
    bool validateKey(const SortedDataKeyValueView& key) override;

    const GenericIndexScanStageParams _params;

    BufBuilder _keyBuffer;
    IndexSeekPoint _seekPoint;
    std::unique_ptr<vm::CodeFragment> _indexBoundsCode;
    boost::optional<IndexBoundsChecker> _checker;
    // The end position for current range, empty if currently not in a valid range.
    key_string::Builder _endKey;
};
}  // namespace mongo::sbe
