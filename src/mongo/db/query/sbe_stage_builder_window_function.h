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

/**
 * Build a list of window function init functions.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInit(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Similar to above but takes multiple arguments.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInit(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Build a list of window function add functions.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAdd(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Similar to above but takes multiple arguments.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAdd(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Build a list of window function remove functions.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemove(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Similar to above but takes multiple arguments.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemove(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args);

/**
 * Build a window function finalize functions from the list of intermediate values.
 */
std::unique_ptr<sbe::EExpression> buildWindowFinalize(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector values,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Similar to above but takes multiple arguments.
 */
std::unique_ptr<sbe::EExpression> buildWindowFinalize(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector values,
    StringDataMap<std::unique_ptr<sbe::EExpression>> arg,
    boost::optional<sbe::value::SlotId> collatorSlots);
}  // namespace mongo::stage_builder
