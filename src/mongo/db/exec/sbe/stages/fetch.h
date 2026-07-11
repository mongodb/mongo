// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/scan_helpers.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

struct FetchCallbacks {
    FetchCallbacks(IndexKeyCorruptionCheckCallback indexKeyCorruptionCheck = nullptr,
                   IndexKeyConsistencyCheckCallback indexKeyConsistencyCheck = nullptr,
                   ScanOpenCallback scanOpen = nullptr)
        : indexKeyCorruptionCheckCallback(std::move(indexKeyCorruptionCheck)),
          indexKeyConsistencyCheckCallback(std::move(indexKeyConsistencyCheck)),
          scanOpenCallback(std::move(scanOpen)) {}

    IndexKeyCorruptionCheckCallback indexKeyCorruptionCheckCallback = nullptr;
    IndexKeyConsistencyCheckCallback indexKeyConsistencyCheckCallback = nullptr;
    ScanOpenCallback scanOpenCallback = nullptr;
};

struct FetchStageState {
    // Input slots
    value::SlotId seekSlot;
    boost::optional<value::SlotId> inSnapshotIdSlot;
    boost::optional<value::SlotId> inIndexIdentSlot;
    boost::optional<value::SlotId> inIndexKeySlot;
    boost::optional<value::SlotId> inIndexKeyPatternSlot;

    // Output slots
    value::SlotId resultSlot;
    value::SlotId recordIdSlot;
    StringListSet scanFieldNames;
    value::SlotVector scanFieldSlots;

    FetchCallbacks fetchCallbacks;
};

/**
 * Seeks for the RecordID in `seekSlot`, providing the full record as output in `resultSlot` and the
 * RecordID in `recordIdSlot`. Any extracted fields can be specified in `scanFieldNames`, with
 * fields correlating to slots in `scanFieldSlots`
 *
 * This stage needs to detect whether a yield has caused the storage snapshot to advance since the
 * index key was obtained from storage. When the snapshot has indeed advanced, the key may no longer
 * be consistent with the 'RecordStore' and we must verify at runtime that no such inconsistency
 * exists. This requires FetchStage to know the value of the index key, the identity of the index
 * from which it was obtained, and the id of the storage snapshot from which it was obtained. This
 * information is made available to the seek stage via 'snapshotIdSlot', 'indexIdentSlot',
 * 'indexKeySlot', and 'indexKeyPatternSlot'.
 *
 * Debug string representation:
 *
 *   fetch seekSlot resultSlot recordIdSlot snapshotIdSlot? indexIdentSlot? indexKeySlot?
 *         indexKeyPatternSlot? [slot1 = fieldName1, ... slot_n = fieldName_n] collUuid childStage
 */
class FetchStage final : public PlanStage {
public:
    FetchStage(std::unique_ptr<PlanStage> child,
               UUID collectionUuid,
               DatabaseName dbName,
               std::shared_ptr<FetchStageState> state,
               PlanYieldPolicySBE* yieldPolicy,
               PlanNodeId nodeId,
               bool participateInTrialRunTracking);


    std::unique_ptr<PlanStage> clone() const override;
    void prepare(CompileCtx& ctx) override;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) override;

    void open(bool reOpen) override;
    PlanState getNext() override;
    void close() override;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const override;
    const SpecificStats* getSpecificStats() const override;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const override;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;

private:
    UUID _collectionUuid;
    DatabaseName _dbName;

    const std::shared_ptr<FetchStageState> _state;

    boost::optional<CollectionAcquisition> _coll;

    value::SlotAccessor* _seekRecordIdAccessor{nullptr};

    value::OwnedValueAccessor _recordAccessor;
    value::OwnedValueAccessor _recordIdAccessor;
    RecordId _seekRid;

    value::SlotAccessor* _snapshotIdAccessor{nullptr};
    value::SlotAccessor* _indexIdentAccessor{nullptr};
    value::SlotAccessor* _indexKeyAccessor{nullptr};
    value::SlotAccessor* _indexKeyPatternAccessor{nullptr};

    absl::InlinedVector<value::OwnedValueAccessor, 4> _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    // Used for index key corruption checks.
    StringMap<const IndexCatalogEntry*> _indexCatalogEntryMap;

    FetchStats _specificStats;
};
}  // namespace sbe
}  // namespace mongo
