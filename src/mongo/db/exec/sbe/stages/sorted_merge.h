/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/sorted_stream_merger.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <cstddef>
#include <memory>
#include <queue>
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
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

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
