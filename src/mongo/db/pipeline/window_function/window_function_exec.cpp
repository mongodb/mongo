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
#include "mongo/db/pipeline/window_function/window_function_exec_derivative.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_range.h"
#include "mongo/db/pipeline/window_function/window_function_exec_removable_document.h"
#include "mongo/db/pipeline/window_function/window_function_exec_removable_range.h"

namespace mongo {

namespace {

std::unique_ptr<WindowFunctionExec> translateDocumentWindow(
    PartitionIterator* iter,
    boost::intrusive_ptr<window_function::Expression> expr,
    const WindowBounds::DocumentBased& bounds) {
    return stdx::visit(
        visit_helper::Overloaded{
            [&](const WindowBounds::Unbounded&) -> std::unique_ptr<WindowFunctionExec> {
                // A left unbounded window will always be non-removable regardless of the upper
                // bound.
                return std::make_unique<WindowFunctionExecNonRemovable<AccumulatorState>>(
                    iter, expr->input(), expr->buildAccumulatorOnly(), bounds.upper);
            },
            [&](const auto&) -> std::unique_ptr<WindowFunctionExec> {
                return std::make_unique<WindowFunctionExecRemovableDocument>(
                    iter, expr->input(), expr->buildRemovable(), bounds);
            }},
        bounds.lower);
}

std::unique_ptr<WindowFunctionExec> translateDerivative(
    PartitionIterator* iter,
    const window_function::ExpressionDerivative& deriv,
    const boost::optional<SortPattern>& sortBy) {
    auto expCtx = deriv.expCtx();

    tassert(5490703,
            "$derivative requires a 1-field ascending sortBy",
            sortBy && sortBy->size() == 1 && !sortBy->begin()->expression &&
                sortBy->begin()->isAscending);
    auto sortExpr = ExpressionFieldPath::createPathFromString(
        expCtx, sortBy->begin()->fieldPath->fullPath(), expCtx->variablesParseState);

    return std::make_unique<WindowFunctionExecDerivative>(
        iter, deriv.input(), sortExpr, deriv.bounds(), deriv.outputUnit());
}

}  // namespace

std::unique_ptr<WindowFunctionExec> WindowFunctionExec::create(
    ExpressionContext* expCtx,
    PartitionIterator* iter,
    const WindowFunctionStatement& functionStmt,
    const boost::optional<SortPattern>& sortBy) {

    if (auto deriv =
            dynamic_cast<window_function::ExpressionDerivative*>(functionStmt.expr.get())) {
        return translateDerivative(iter, *deriv, sortBy);
    }

    WindowBounds bounds = functionStmt.expr->bounds();

    return stdx::visit(
        visit_helper::Overloaded{
            [&](const WindowBounds::DocumentBased& docBounds) {
                return translateDocumentWindow(iter, functionStmt.expr, docBounds);
            },
            [&](const WindowBounds::RangeBased& rangeBounds)
                -> std::unique_ptr<WindowFunctionExec> {
                // These checks should be enforced already during parsing.
                tassert(5429401,
                        "Range-based window needs a non-compound sortBy",
                        sortBy != boost::none && sortBy->size() == 1);
                SortPattern::SortPatternPart part = *sortBy->begin();
                tassert(5429410,
                        "Range-based window doesn't work on expression-sortBy",
                        part.fieldPath != boost::none && !part.expression);
                auto sortByExpr = ExpressionFieldPath::createPathFromString(
                    expCtx, part.fieldPath->fullPath(), expCtx->variablesParseState);

                if (stdx::holds_alternative<WindowBounds::Unbounded>(rangeBounds.lower)) {
                    return std::make_unique<WindowFunctionExecNonRemovableRange>(
                        iter,
                        functionStmt.expr->input(),
                        std::move(sortByExpr),
                        functionStmt.expr->buildAccumulatorOnly(),
                        bounds);
                } else {
                    return std::make_unique<WindowFunctionExecRemovableRange>(
                        iter,
                        functionStmt.expr->input(),
                        std::move(sortByExpr),
                        functionStmt.expr->buildRemovable(),
                        bounds);
                }
            },
            [&](const WindowBounds::TimeBased& timeBounds) -> std::unique_ptr<WindowFunctionExec> {
                uasserted(5397902, "Time based windows are not currently supported");
            }},
        bounds.bounds);
}

}  // namespace mongo
