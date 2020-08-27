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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/id_generators.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo::stage_builder {
/**
 * Translates an input Expression into an SBE EExpression, along with a chain of PlanStages whose
 * output will be necessary to evaluate the EExpression. The 'stage' input will be attached to the
 * end of the resulting chain of PlanStages.
 *
 * Note that any slot whose value must be visible to the parent of the PlanStage output by this
 * function should be included in the 'relevantSlots' list. Some stages (notably LoopJoin) do not
 * forward all of the slots visible to them to their parents; they need an explicit list of which
 * slots to forward.
 *
 * The 'relevantSlots' is an input/output parameter. Execution of this function may add additional
 * relevant slots to the list.
 */
std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::PlanStage>>
generateExpression(OperationContext* opCtx,
                   Expression* expr,
                   std::unique_ptr<sbe::PlanStage> stage,
                   sbe::value::SlotIdGenerator* slotIdGenerator,
                   sbe::value::FrameIdGenerator* frameIdGenerator,
                   sbe::value::SlotId inputVar,
                   sbe::RuntimeEnvironment* env,
                   sbe::value::SlotVector* relevantSlots = nullptr);

/**
 * Generate an EExpression that converts a value (contained in a variable bound to 'branchRef') that
 * can be of any type to a Boolean value based on MQL's definition of truth for the branch of any
 * logical expression.
 */
std::unique_ptr<sbe::EExpression> generateExpressionForLogicBranch(sbe::EVariable branchRef);
}  // namespace mongo::stage_builder
