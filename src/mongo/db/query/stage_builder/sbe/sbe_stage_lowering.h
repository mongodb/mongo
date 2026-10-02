// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * Stage Builder Blueprint Lowering Pass
 *
 * lowerSbeBlueprint() walks a blueprint subtree bottom-up and produces a concrete sbe::PlanStage
 * tree. This is the analogue of SbExpr::lower() one level up the AST.
 */

#include "mongo/db/query/stage_builder/sbe/builder_state.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

namespace mongo::stage_builder {

/**
 * Lowers the blueprint subtree rooted at 'root' to a concrete sbe::PlanStage tree. Each node of
 * the subtree is moved out of 'blueprint' and replaced by SbBlueprintLowered. Every node of
 * 'blueprint' that is not already lowered must be reachable from 'root' and is reached exactly
 * once.
 *
 * 'varTypes' is passed to SbExpr::lower() for the expressions within the subtree, enabling
 * type-based optimizations (e.g. traverseF elimination).
 */
SbStage lowerSbeBlueprint(SbBlueprint& blueprint,
                          SbBlueprintNodeIdx root,
                          StageBuilderState& state,
                          const VariableTypes* varTypes = nullptr);

}  // namespace mongo::stage_builder
