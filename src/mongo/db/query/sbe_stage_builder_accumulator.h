/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
/**
 * Translates the argument Expression of an AccumulationExpression carried by the
 * AccumulationStatement. The 'stage' parameter provides the input subtree to build on top of.
 */
std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildArgument(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    EvalStage stage,
    boost::optional<sbe::value::SlotId> optionalRootSlot,
    PlanNodeId planNodeId);

/**
 * Translates an input AccumulationStatement into an SBE EExpression for accumulation expressions.
 * The 'stage' parameter provides the input subtree to build on top of.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> argExpr);

/**
 * Translates an input AccumulationStatement into an SBE EExpression that represents an
 * AccumulationStatement's finalization step. The 'stage' parameter provides the input subtree to
 * build on top of.
 */
std::unique_ptr<sbe::EExpression> buildFinalize(StageBuilderState& state,
                                                const AccumulationStatement& acc,
                                                const sbe::value::SlotVector& aggSlots);
}  // namespace mongo::stage_builder
