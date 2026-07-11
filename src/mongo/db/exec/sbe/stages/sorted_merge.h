// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/sorted_stream_merger.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Merges the outputs of N children, each of which is sorted. The output is also sorted.
 *
 * This stage is a binding reflector.
 */
class SortedMergeStage final : public PlanStage {
public:
    /**
     * Constructor. Arguments:
     * -inputStages: Array of child stages. Each stage must return results in sorted order.
     * -inputKeys: Element 'i' of this vector describes which slots the sort key can be found in
     *  for child 'i'. Each element of this vector should have the same size as 'dirs'.
     * -dirs: Describes how to interpret the sort key.
     * -inputVals: Similar layout to 'inputKeys' but the slots hold additional values that should
     *  be propagated.
     * -outputVals: Slots where the output should be stored.
     */
    SortedMergeStage(PlanStage::Vector inputStages,
                     // Each element of 'inputKeys' must be the same size as 'dirs'.
                     std::vector<value::SlotVector> inputKeys,
                     // Sort directions. Used to interpret each input key.
                     std::vector<value::SortDirection> dirs,
                     // Each element of 'inputVals' must be the same size as 'outputVals'.
                     std::vector<value::SlotVector> inputVals,
                     value::SlotVector outputVals,
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
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
private:
    const std::vector<value::SlotVector> _inputKeys;
    const std::vector<value::SortDirection> _dirs;

    const std::vector<value::SlotVector> _inputVals;
    const value::SlotVector _outputVals;

    std::vector<value::SwitchAccessor> _outAccessors;

    // Maintains state about merging the results in order. Initialized during prepare().
    boost::optional<SortedStreamMerger<PlanStage>> _merger;
};
}  // namespace mongo::sbe
