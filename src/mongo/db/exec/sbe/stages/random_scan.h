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
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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
                    PlanYieldPolicy* yieldPolicy,
                    PlanNodeId nodeId,
                    // Optional arguments:
                    bool participateInTrialRunTracking = true);
    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    RandomScanStage(std::shared_ptr<ScanStageBaseState> state,
                    PlanYieldPolicy* yieldPolicy,
                    PlanNodeId nodeId,
                    bool participateInTrialRunTracking);

    std::unique_ptr<PlanStage> clone() const final;
    PlanState getNext() final;
    inline void close() final {
        closeShared();
        _randomCursor.reset();
    }
    void prepare(CompileCtx& ctx) final {
        prepareShared(ctx);
    }
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    std::vector<DebugPrinter::Block> debugPrint(const DebugPrintInfo& debugPrintInfo) const final;

private:
    void scanResetState(bool reOpen) {
        _randomCursor = _coll.getPtr()->getRecordStore()->getRandomCursor(
            _opCtx, *shard_role_details::getRecoveryUnit(_opCtx));
    }
    inline RecordCursor* getActiveCursor() const {
        return _randomCursor.get();
    }
    std::unique_ptr<RecordCursor> _randomCursor;
};  // class RandomScanStage
}  // namespace sbe
}  // namespace mongo
