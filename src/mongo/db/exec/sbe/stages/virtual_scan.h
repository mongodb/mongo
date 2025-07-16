/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mongo::sbe {
/**
 * VirtualScanStage stores the array 'arrVal' and getNext() returns each value from the array.
 *
 * This stage mimics the resource management behavior like an actual scan stage. getNext() and
 * doSaveState() release the memory of the returned values. This is useful to expose the potential
 * memory misuse bugs such as heap-use-after-free and memory leaks.
 */
class VirtualScanStage final : public PlanStage {
public:
    explicit VirtualScanStage(PlanNodeId planNodeId,
                              value::SlotId out,
                              value::TypeTags arrTag,
                              value::Value arrVal,
                              PlanYieldPolicy* yieldPolicy = nullptr,
                              bool participateInTrialRunTracking = true);

    ~VirtualScanStage() final;

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;

    value::SlotAccessor* getAccessor(sbe::CompileCtx& ctx, sbe::value::SlotId slot) final;

    void open(bool reOpen) final;

    PlanState getNext() final;

    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doSaveState() final;

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

private:
    const value::SlotId _outField;

    value::TypeTags _arrTag;
    value::Value _arrVal;

    std::unique_ptr<value::ViewOfValueAccessor> _outFieldOutputAccessor;

    // Keeps track of an index for the array values, and an index for the next value to release.
    size_t _index{0};
    size_t _releaseIndex{0};

    // Stores the values in std::vector instead of value::Array allows to release memory from values
    // individually. This also avoid releasing memory twice due to value::Array::~Array().
    std::vector<std::pair<value::TypeTags, sbe::value::Value>> _values;
};
}  // namespace mongo::sbe
