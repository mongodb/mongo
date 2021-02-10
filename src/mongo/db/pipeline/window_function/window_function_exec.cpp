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

#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"

namespace mongo {

namespace {

std::unique_ptr<WindowFunctionExec> translateDocumentWindow(
    PartitionIterator* iter,
    boost::intrusive_ptr<window_function::Expression> expr,
    const WindowBounds::DocumentBased& bounds) {
    uassert(5397904,
            "Only 'unbounded' lower bound is currently supported",
            stdx::holds_alternative<WindowBounds::Unbounded>(bounds.lower));
    uassert(5397903,
            "Only 'current' upper bound is currently supported",
            stdx::holds_alternative<WindowBounds::Current>(bounds.upper));

    // A left unbounded window will always be non-removable regardless of the upper
    // bound.
    return std::make_unique<WindowFunctionExecNonRemovable<AccumulatorState>>(
        iter, std::move(expr->input()), expr->buildAccumulatorOnly(), bounds.upper);
}

}  // namespace

std::unique_ptr<WindowFunctionExec> WindowFunctionExec::create(
    PartitionIterator* iter, const WindowFunctionStatement& functionStmt) {
    uassert(5397905,
            "Window functions cannot set to dotted paths",
            functionStmt.fieldName.find('.') == std::string::npos);

    // Use a sentinel variable to avoid a compilation error when some cases of std::visit don't
    // return a value.
    std::unique_ptr<WindowFunctionExec> exec;
    stdx::visit(
        visit_helper::Overloaded{
            [&](const WindowBounds::DocumentBased& docBase) {
                exec = translateDocumentWindow(iter, functionStmt.expr, docBase);
            },
            [&](const WindowBounds::RangeBased& rangeBase) {
                uasserted(5397901, "Ranged based windows not currently supported");
            },
            [&](const WindowBounds::TimeBased& timeBase) {
                uasserted(5397902, "Time based windows are not currently supported");
            }},
        functionStmt.expr->bounds().bounds);
    return exec;
}

}  // namespace mongo
