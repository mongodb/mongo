/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
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
               PlanYieldPolicy* yieldPolicy,
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
    std::vector<DebugPrinter::Block> debugPrint() const override;
    size_t estimateCompileTimeSize() const override;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override;

private:
    UUID _collectionUuid;
    DatabaseName _dbName;

    const std::shared_ptr<FetchStageState> _state;

    CollectionRef _coll;

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
