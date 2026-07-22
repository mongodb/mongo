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
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

class RandomScanStage final : public ScanStageBaseImpl<RandomScanStage> {
    friend class ScanStageBaseImpl<RandomScanStage>;

public:
    RandomScanStage(UUID collUuid,
                    DatabaseName dbName,
                    boost::optional<value::SlotId> recordSlot,
                    boost::optional<value::SlotId> recordIdSlot,
                    boost::optional<value::SlotId> snapshotIdSlot,
                    boost::optional<value::SlotId> indexIdentSlot,
                    boost::optional<value::SlotId> indexKeySlot,
                    boost::optional<value::SlotId> indexKeyPatternSlot,
                    std::vector<std::string> scanFieldNames,
                    value::SlotVector scanFieldSlots,
                    PlanYieldPolicySBE* yieldPolicy,
                    PlanNodeId nodeId,
                    // Optional arguments:
                    bool participateInTrialRunTracking = true);
    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    RandomScanStage(std::shared_ptr<ScanStageBaseState> state,
                    PlanYieldPolicySBE* yieldPolicy,
                    PlanNodeId nodeId,
                    bool participateInTrialRunTracking);

    std::unique_ptr<PlanStage> clone() const final;
    inline void close() final {
        closeShared();
        _randomCursor.reset();
    }
    void prepare(CompileCtx& ctx) final {
        prepareShared(ctx);
    }
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;

private:
    void scanResetState(bool reOpen) {
        _randomCursor = _coll->getCollectionPtr()->getRecordStore()->getRandomCursor(
            _opCtx, *shard_role_details::getRecoveryUnit(_opCtx));
    }
    inline RecordCursor* getActiveCursor() const {
        return _randomCursor.get();
    }
    void getNextHangFailPoint() {
        // no-op
    }
    bool pastEnd() const {
        return false;
    }
    boost::optional<Record> getNextInternal() {
        return _randomCursor->next();
    }
    std::unique_ptr<RecordCursor> _randomCursor;
};  // class RandomScanStage
}  // namespace sbe
}  // namespace mongo
