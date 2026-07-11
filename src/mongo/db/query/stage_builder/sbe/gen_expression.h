// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {

struct StageBuilderState;
class PlanStageSlots;
class PlanStageReqs;

/**
 * Translates an input Expression into an SBE SbExpr. 'rootSlot' should either be boost::none or
 * a slot with the root document. 'slots' can optionaly be provided as well so that
 * generateExpression() can make use of kField slots when appropriate.
 */
SbExpr generateExpression(StageBuilderState& state,
                          const Expression* expr,
                          boost::optional<SbSlot> rootSlot,
                          const PlanStageSlots& slots);

SbExpr generateExpressionFieldPath(StageBuilderState& state,
                                   const FieldPath& fieldPath,
                                   boost::optional<Variables::Id> variableId,
                                   boost::optional<SbSlot> rootSlot,
                                   const PlanStageSlots& slots,
                                   std::map<Variables::Id, sbe::FrameId>* environment = nullptr);

SbExpr generateExpressionCompare(StageBuilderState& state,
                                 ExpressionCompare::CmpOp op,
                                 SbExpr lhs,
                                 SbExpr rhs);
}  // namespace mongo::stage_builder
