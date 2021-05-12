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

#include <queue>

#include "mongo/db/exec/sbe/stages/stages.h"

namespace mongo::sbe {
class UnionStage final : public PlanStage {
public:
    UnionStage(std::vector<std::unique_ptr<PlanStage>> inputStages,
               std::vector<value::SlotVector> inputVals,
               value::SlotVector outputVals,
               PlanNodeId planNodeId);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

private:
    struct UnionBranch {
        PlanStage* stage{nullptr};
        const bool reOpen{false};
        bool isOpen{false};

        void open() {
            if (!isOpen) {
                stage->open(reOpen);
                isOpen = true;
            }
        }

        void close() {
            if (isOpen) {
                stage->close();
                isOpen = false;
            }
        }
    };

    void clearBranches();

    const std::vector<value::SlotVector> _inputVals;
    const value::SlotVector _outputVals;
    std::vector<value::SwitchAccessor> _outValueAccessors;
    std::queue<UnionBranch> _remainingBranchesToDrain;
    PlanStage* _currentStage{nullptr};
    size_t _currentStageIndex{0};
};
}  // namespace mongo::sbe
