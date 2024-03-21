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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/window_function/window_function_statement.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
namespace Accum {
class Op;
}

/**
 * Build a list of window function init functions.
 */
SbExpr::Vector buildWindowInit(StageBuilderState& state, const WindowFunctionStatement& stmt);

SbExpr::Vector buildWindowInit(StageBuilderState& state,
                               const WindowFunctionStatement& stmt,
                               StringDataMap<SbExpr> args);

/**
 * Build a list of window function add functions.
 */
SbExpr::Vector buildWindowAdd(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbExpr arg);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr::Vector buildWindowAdd(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              StringDataMap<SbExpr> args);

/**
 * Build a list of window function remove functions.
 */
SbExpr::Vector buildWindowRemove(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 SbExpr arg);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr::Vector buildWindowRemove(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 StringDataMap<SbExpr> args);

/**
 * Build a window function finalize functions from the list of intermediate values.
 */
SbExpr buildWindowFinalize(StageBuilderState& state,
                           const WindowFunctionStatement& stmt,
                           SbSlotVector values);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr buildWindowFinalize(StageBuilderState& state,
                           const WindowFunctionStatement& stmt,
                           SbSlotVector values,
                           StringDataMap<SbExpr> args);

/**
 * Given an Accum::Op 'acc' and a single input expression ('input'), these functions
 * generate the accumulate expressions for 'acc'.
 */
SbExpr::Vector buildAccumulatorForWindowFunc(const Accum::Op& acc,
                                             SbExpr input,
                                             StageBuilderState& state);

/**
 * Given an Accum::Op 'acc' and a set of input expressions ('inputs'), these functions
 * generate the accumulate expressions for 'acc'.
 */
SbExpr::Vector buildAccumulatorForWindowFunc(const Accum::Op& acc,
                                             StringDataMap<SbExpr> inputs,
                                             StageBuilderState& state);

SbExpr::Vector buildInitializeForWindowFunc(const Accum::Op& acc, StageBuilderState&);

SbExpr::Vector buildInitializeForWindowFunc(const Accum::Op& acc,
                                            StringDataMap<SbExpr> argExprs,
                                            StageBuilderState&);

SbExpr buildFinalizeForWindowFunc(const Accum::Op& acc,
                                  StageBuilderState& state,
                                  const SbSlotVector& aggSlots);

SbExpr buildFinalizeForWindowFunc(const Accum::Op& acc,
                                  StringDataMap<SbExpr> argExprs,
                                  StageBuilderState& state,
                                  const SbSlotVector& aggSlots);
}  // namespace mongo::stage_builder
