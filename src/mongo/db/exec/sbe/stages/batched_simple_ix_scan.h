// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

/**
 * A variant of 'SimpleIndexScanStage' that can return the record ids of several index keys in a
 * single call to getNext(). It only supports collections with integer record ids, and it does not
 * output the index key or any projected index key values (so there is no 'indexKeySlot' or 'vars'
 * array).
 *
 * Each call to getNext() reads up to 'maxIndexKeyBatchCount' index keys. When
 * 'maxIndexKeyBatchCount' is greater than 1, 'recordIdSlot' holds a 'value::Int64RecordIdBlock'
 * with one entry per key read (the last batch may be smaller than the maximum). When it is 1,
 * 'recordIdSlot' holds a single RecordId, exactly as 'SimpleIndexScanStage' does.
 *
 * The seek key semantics are the same as the 'SimpleIndexScanStage' semantics, but unlike
 * 'SimpleIndexScanStage' index seeks, the 'BatchedSimpleIndexScanStage' does not have a special
 * case for point query bounds on a unique index. The simple index scan optimizes that case by
 * recognizing it can never produce more than one document, but this stage assumes that the planner
 * will not choose a batched scan for a point query.
 *
 * Batched simple index scans are always forward scans. There is no option for a reverse-direction
 * batched index scan.
 *
 * Debug string representation:
 *
 *   batched_ixscan maxIndexKeyBatchCount recordIdSlot? snapshotIdSlot? indexIdentSlot?
 *                  collectionUuid indexName forward
 *
 *   batched_ixseek lowKey highKey maxIndexKeyBatchCount recordIdSlot? snapshotIdSlot?
 *                  indexIdentSlot? collectionUuid indexName forward
 */
class BatchedSimpleIndexScanStage final : public IndexScanStageBase {
public:
    BatchedSimpleIndexScanStage(UUID collUuid,
                                DatabaseName dbName,
                                std::string_view indexName,
                                size_t maxIndexKeyBatchCount,
                                boost::optional<value::SlotId> recordIdSlot,
                                boost::optional<value::SlotId> snapshotIdSlot,
                                boost::optional<value::SlotId> indexIdentSlot,
                                std::unique_ptr<EExpression> seekKeyLow,
                                std::unique_ptr<EExpression> seekKeyHigh,
                                PlanYieldPolicySBE* yieldPolicy,
                                PlanNodeId planNodeId,
                                bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const override;

    void prepare(CompileCtx& ctx) override;
    void open(bool reOpen) override;
    PlanState getNext() override;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const override;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const override;

protected:
    void doSaveState() override;

private:
    inline SortedDataKeyValueView seek(RecoveryUnit& ru) {
        auto& query = getSeekKeyLow();
        return _cursor->seekForKeyValueView(ru, query.getView());
    }

    const key_string::Value& getSeekKeyLow() const;
    const key_string::Value& getSeekKeyHigh() const;

    void initializeSeekKeyLow();
    void initializeSeekKeyHigh();
    void initializeSeekKeyDefault();

    // Positions the cursor at the beginning of the scan bounds the first time it is called or moves
    // it to the next key on subsequent call. When the key is within bounds after it is advanced,
    // this method populates '_nextKeyString' with the key and returns true. Otherwise, it returns
    // false, indicating that the scan is finished.
    bool advanceToNextKey(RecoveryUnit& ru);

    const size_t _maxIndexKeyBatchCount;

    std::unique_ptr<EExpression> _seekKeyLow;
    std::unique_ptr<EExpression> _seekKeyHigh;

    // Carries the compiled bytecode for the above '_seekKeyLow' and '_seekKeyHigh' expressions.
    std::unique_ptr<vm::CodeFragment> _seekKeyLowCode;
    std::unique_ptr<vm::CodeFragment> _seekKeyHighCode;

    value::OwnedValueAccessor _seekKeyLowHolder;
    value::OwnedValueAccessor _seekKeyHighHolder;
};

}  // namespace mongo::sbe
