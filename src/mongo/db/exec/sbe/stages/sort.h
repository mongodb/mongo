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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/trial_run_progress_tracker.h"

namespace mongo::sbe {
class SortStage final : public PlanStage {
public:
    SortStage(std::unique_ptr<PlanStage> input,
              value::SlotVector obs,
              std::vector<value::SortDirection> dirs,
              value::SlotVector vals,
              size_t limit,
              TrialRunProgressTracker* tracker);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats() const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

private:
    using TableType = std::
        multimap<value::MaterializedRow, value::MaterializedRow, value::MaterializedRowComparator>;

    using SortKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using SortValueAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

    const value::SlotVector _obs;
    const std::vector<value::SortDirection> _dirs;
    const value::SlotVector _vals;
    const size_t _limit;

    std::vector<value::SlotAccessor*> _inKeyAccessors;
    std::vector<value::SlotAccessor*> _inValueAccessors;

    value::SlotMap<std::unique_ptr<value::SlotAccessor>> _outAccessors;

    TableType _st;
    TableType::iterator _stIt;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunProgressTracker* _tracker{nullptr};
};
}  // namespace mongo::sbe
