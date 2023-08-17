/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <stack>
#include <utility>
#include <variant>

#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_def.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/stdx/variant.h"

namespace mongo::stage_builder {

/**
 * SbStage contains a PlanStage (_stage) and a vector of slots (_outSlots).
 *
 * _stage can be nullptr or it can point to an SBE PlanStage tree. If _stage is nullptr, the
 * extractStage() method will return a Limit-1/CoScan tree. If _stage is not nullptr, then the
 * extractStage() method will return _stage. SbStage's default constructor initializes
 * _stage to be nullptr.
 *
 * The isNull() method allows callers to check if _state is nullptr. Some helper functions (such
 * as makeLoopJoin()) take advantage of this knowledge and are able to perform optimizations in
 * the case where isNull() == true.
 *
 * The _outSlots vector keeps track of all of the "output" slots that are produced by the current
 * sbe::PlanStage tree (_stage). The _outSlots vector is used by makeLoopJoin() and makeTraverse()
 * to ensure that all of the slots produced by the left side are visible to the right side and are
 * also visible to the parent of the LoopJoinStage/TraverseStage.
 */
class SbStage {
public:
    SbStage() {}

    SbStage(std::unique_ptr<sbe::PlanStage> stage, sbe::value::SlotVector outSlots)
        : _stage(std::move(stage)), _outSlots(std::move(outSlots)) {}

    SbStage(SbStage&& other)
        : _stage(std::move(other._stage)), _outSlots(std::move(other._outSlots)) {}

    SbStage& operator=(SbStage&& other) {
        _stage = std::move(other._stage);
        _outSlots = std::move(other._outSlots);
        return *this;
    }

    bool isNull() const {
        return !_stage;
    }

    std::unique_ptr<sbe::PlanStage> extractStage(PlanNodeId planNodeId) {
        return _stage ? std::move(_stage)
                      : sbe::makeS<sbe::LimitSkipStage>(
                            sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId);
    }

    void setStage(std::unique_ptr<sbe::PlanStage> stage) {
        _stage = std::move(stage);
    }

    const sbe::value::SlotVector& getOutSlots() const {
        return _outSlots;
    }

    sbe::value::SlotVector extractOutSlots() {
        return std::move(_outSlots);
    }

    void setOutSlots(sbe::value::SlotVector outSlots) {
        _outSlots = std::move(outSlots);
    }

    void addOutSlot(sbe::value::SlotId slot) {
        _outSlots.push_back(slot);
    }

private:
    std::unique_ptr<sbe::PlanStage> _stage;
    sbe::value::SlotVector _outSlots;
};

using EvalStage = SbStage;

using SbExprStagePair = std::pair<SbExpr, SbStage>;

using EvalExprStagePair = SbExprStagePair;

}  // namespace mongo::stage_builder
