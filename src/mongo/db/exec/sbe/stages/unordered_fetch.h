// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

/**
 * Consumes record ids from its child (via the 'seekSlot') and reads the corresponding documents
 * from the collection, as the regular 'FetchStage' does, but collects the input record ids in
 * batches instead of reading each record id immediately before fetching its record. Each batch is
 * sorted by record id, which can improve read efficiency but means that the stage does not
 * guarantee any particular order for output documents. Duplicate record ids are not removed.
 *
 * In the common case, each 'getNext()' consumes one record id from the current batch and populates
 * the 'resultSlot' with the corresponding document. If the record id is not found in the
 * collection, which can happen when a yield causes the fetch to target a newer snapshot than the
 * original index read or as a result of a prepare conflict, the 'getNext()' keeps consuming record
 * ids until it finds a document.
 *
 * When there are no batched record ids, a 'getNext()' triggers the stage to read a batch of up to
 * 'maxRecordIdBatchCount' new record ids from its child. The stage is designed to read
 * 'Int64RecordIdBlock' values from a 'BatchedSimpleIndexScanStage' child stage but can also read
 * individual record ids. The child stage's batch size need not match this stage's
 * 'maxRecordIdBatchCount' value.
 *
 * The 'scanFieldNames' refer to top-level fields to be extracted from each document and are paired
 * positionally with the 'scanFieldSlots' that are populated with the extracted values. The
 * 'recordIdSlot' and 'resultSlot' are populated with the current record id and corresponding
 * document, respectively. This stage does not expose any of its child's slots.
 *
 * Support for yielding is future work. The stage saves and restores its cursor but does not
 * regularly yield and cannot verify that a document's index key is still within range if its record
 * id was read before a yield.
 *
 * Debug representation:
 *   unordered_fetch seekSlot resultSlot recordIdSlot [slot = field, ...] maxRecordIdBatchCount
 *   @collectionUuid child
 */
class UnorderedFetchStage final : public PlanStage {
public:
    UnorderedFetchStage(std::unique_ptr<PlanStage> child,
                        UUID collectionUuid,
                        DatabaseName dbName,
                        value::SlotId seekSlot,
                        value::SlotId resultSlot,
                        value::SlotId recordIdSlot,
                        std::vector<std::string> scanFieldNames,
                        value::SlotVector scanFieldSlots,
                        size_t maxRecordIdBatchCount,
                        PlanYieldPolicySBE* yieldPolicy,
                        PlanNodeId nodeId,
                        bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const override;
    void prepare(CompileCtx& ctx) override;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) override;
    void open(bool reOpen) override;
    PlanState getNext() override;
    void close() override;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const override;
    const SpecificStats* getSpecificStats() const override;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const override;
    size_t estimateCompileTimeSize() const override;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;

private:
    /**
     * Empties the current batch of record ids in '_recordIdBatch' and populates it with a new batch
     * by reading from the child stage. If '_recordIdBatch' is empty after this method returns, it
     * indicates that the input stream of record ids is exhausted.
     *
     * When the child stage provides a block of record ids, this stage takes ownership of it in
     * '_pendingRecordIds', so that any of the block's record ids that did not fit into the current
     * batch can be added to the next one, even if the child stage is invalidated by a yield.
     */
    void fillBatch();

    void resetBufferedState();

    const UUID _collectionUuid;
    const DatabaseName _dbName;
    const value::SlotId _seekSlot;
    const value::SlotId _resultSlot;
    const value::SlotId _recordIdSlot;
    const StringListSet _scanFieldNames;
    const value::SlotVector _scanFieldSlots;

    const size_t _maxRecordIdBatchCount;

    boost::optional<CollectionAcquisition> _coll;

    // Populated by the child stage with a batch of record ids.
    value::SlotAccessor* _seekAccessor{nullptr};

    value::OwnedValueAccessor _recordAccessor;
    value::OwnedValueAccessor _recordIdAccessor;

    // Stores the record id of the most recently fetched document so that it can be viewed via the
    // 'recordIdSlot' accessor.
    RecordId _seekRid;

    absl::InlinedVector<value::OwnedValueAccessor, 4> _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    std::vector<int64_t> _recordIdBatch;
    size_t _numIdsConsumedFromBatch{0};

    // Moved here from the '_seekAccessor' when the child stage provides record ids as a batch.
    //
    // The child's accessor retains an unowned view of the block after the move, so the block must
    // stay alive until the child replaces it on its next 'getNext()' call, even after all of its
    // record ids have been consumed. Freeing it any earlier could leave the child with a dangling
    // view.
    std::unique_ptr<const value::Int64RecordIdBlock> _pendingRecordIds;
    size_t _numPendingIdsConsumed{0};

    // True when there are no remaining record ids to read from the child stage.
    bool _inputIdsExhausted{false};

    FetchStats _specificStats;
    size_t _batches{0};
    size_t _rids{0};
};

}  // namespace mongo::sbe
