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
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

namespace mongo::stage_builder {
class PlanStageSlots;

/**
 * Generates an SbExpr implementing a query projection. The 'inputSlot' defines a slot to read
 * the input document from. 'slots' can optionaly be provided as well so that generateExpression()
 * can make use of kField slots when appropriate.
 */
SbExpr generateProjection(StageBuilderState& state,
                          const projection_ast::Projection* projection,
                          SbExpr inputExpr,
                          const PlanStageSlots* slots = nullptr);

SbExpr generateProjection(StageBuilderState& state,
                          projection_ast::ProjectType type,
                          std::vector<std::string> paths,
                          std::vector<ProjectNode> nodes,
                          SbExpr inputExpr,
                          const PlanStageSlots* slots = nullptr);

SbExpr generateProjectionWithInputFields(StageBuilderState& state,
                                         const projection_ast::Projection* projection,
                                         SbExpr inputExpr,
                                         const sbe::MakeObjInputPlan& inputPlan,
                                         const PlanStageSlots* slots);

SbExpr generateProjectionWithInputFields(StageBuilderState& state,
                                         projection_ast::ProjectType type,
                                         std::vector<std::string> paths,
                                         std::vector<ProjectNode> nodes,
                                         SbExpr inputExpr,
                                         const sbe::MakeObjInputPlan& inputPlan,
                                         const PlanStageSlots* slots);
}  // namespace mongo::stage_builder
