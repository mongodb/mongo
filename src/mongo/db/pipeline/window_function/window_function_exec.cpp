// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_exec.h"

#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec_derivative.h"
#include "mongo/db/pipeline/window_function/window_function_exec_first_last.h"
#include "mongo/db/pipeline/window_function/window_function_exec_linear_fill.h"
#include "mongo/db/pipeline/window_function/window_function_exec_min_max_scaler_non_removable.h"
#include "mongo/db/pipeline/window_function/window_function_exec_min_max_scaler_non_removable_range.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_range.h"
#include "mongo/db/pipeline/window_function/window_function_exec_removable_document.h"
#include "mongo/db/pipeline/window_function/window_function_exec_removable_range.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_shift.h"
#include "mongo/db/pipeline/window_function/window_function_statement.h"

#include <variant>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

/**
 * Translates the input Expression to suit certain window function's need. For example, $integral
 * window function requires the input value to be a 2-sized Value vector containing the evaluating
 * value of 'sortBy' and 'input' field. So we create an 'ExpressionArray' as the input Expression
 * for Executors.
 *
 * Returns the 'input' in 'expr' if no extra translation is needed.
 */
boost::intrusive_ptr<Expression> translateInputExpression(
    boost::intrusive_ptr<window_function::Expression> expr,
    const boost::optional<SortPattern>& sortBy) {
    if (!expr)
        return nullptr;
    if (auto integral = dynamic_cast<window_function::ExpressionIntegral*>(expr.get())) {
        auto expCtx = integral->expCtx();
        tassert(5558802,
                "$integral requires a 1-field sortBy",
                sortBy && sortBy->size() == 1 && !sortBy->begin()->expression);
        auto sortByExpr = ExpressionFieldPath::createPathFromString(
            expCtx, sortBy->begin()->fieldPath->fullPath(), expCtx->variablesParseState);
        return ExpressionArray::create(
            expCtx, std::vector<boost::intrusive_ptr<Expression>>{sortByExpr, integral->input()});
    }

    return expr->input();
}

// Assertions for when 'WindowBounds' are 'RangeBased'.
void assertRangeBasedBoundsChecks(const boost::optional<SortPattern>& sortBy) {
    tassert(5429401,
            "Range-based window needs a non-compound sortBy",
            sortBy != boost::none && sortBy->size() == 1);
    SortPattern::SortPatternPart part = *sortBy->begin();
    tassert(5429410,
            "Range-based window doesn't work on expression-sortBy",
            part.fieldPath != boost::none && !part.expression);
}

std::unique_ptr<WindowFunctionExec> translateDocumentWindow(
    PartitionIterator* iter,
    boost::intrusive_ptr<window_function::Expression> expr,
    const boost::optional<SortPattern>& sortBy,
    const WindowBounds::DocumentBased& bounds,
    SimpleMemoryUsageTracker* memTracker) {
    auto inputExpr = translateInputExpression(expr, sortBy);

    return visit(
        OverloadedVisitor{
            [&](const WindowBounds::Unbounded&) -> std::unique_ptr<WindowFunctionExec> {
                // A left unbounded window will always be non-removable regardless of the upper
                // bound.
                return std::make_unique<WindowFunctionExecNonRemovable>(
                    iter, inputExpr, expr->buildAccumulatorOnly(), bounds.upper, memTracker);
            },
            [&](const auto&) -> std::unique_ptr<WindowFunctionExec> {
                return std::make_unique<WindowFunctionExecRemovableDocument>(
                    iter, inputExpr, expr->buildRemovable(), bounds, memTracker);
            }},
        bounds.lower);
}

std::unique_ptr<mongo::WindowFunctionExec> translateDerivative(
    window_function::ExpressionDerivative* expr,
    PartitionIterator* iter,
    const boost::optional<SortPattern>& sortBy,
    SimpleMemoryUsageTracker* memTracker) {
    tassert(5490703,
            "$derivative requires a 1-field sortBy",
            sortBy && sortBy->size() == 1 && !sortBy->begin()->expression);
    auto sortExpr =
        ExpressionFieldPath::createPathFromString(expr->expCtx(),
                                                  sortBy->begin()->fieldPath->fullPath(),
                                                  expr->expCtx()->variablesParseState);

    return std::make_unique<WindowFunctionExecDerivative>(
        iter, expr->input(), sortExpr, expr->bounds(), expr->unit(), memTracker);
}

// $minMaxScaler uses a custom translation from a ExpressionMinMaxScaler to WindowFunctionExec
// because, like other window functions, it differentiates its execution implementation between
// removable and non-removable windows for execution efficiency; however, unlike other window
// functions, it does not use the generic implementation of non-removable WindowFunctionExecs
// because it is unable to implement a generic accumulator, due to needing the value of
// the "current" document being processed.
std::unique_ptr<mongo::WindowFunctionExec> translateMinMaxScaler(
    ExpressionContext* expCtx,
    window_function::ExpressionMinMaxScaler* minMaxScalerExpr,
    PartitionIterator* iter,
    const boost::optional<SortPattern>& sortBy,
    SimpleMemoryUsageTracker* memTracker) {
    WindowBounds bounds = minMaxScalerExpr->bounds();
    return visit(
        OverloadedVisitor{
            [&](const WindowBounds::DocumentBased& docBounds) {
                return visit(
                    OverloadedVisitor{
                        [&](const WindowBounds::Unbounded&) -> std::unique_ptr<WindowFunctionExec> {
                            // A left unbounded window will always be non-removable regardless of
                            // the upper bound.
                            return std::make_unique<WindowFunctionExecMinMaxScalerNonRemovable>(
                                iter,
                                minMaxScalerExpr->input(),
                                docBounds.upper,
                                memTracker,
                                minMaxScalerExpr->getDomainMinAndMax());
                        },
                        [&](const auto&) -> std::unique_ptr<WindowFunctionExec> {
                            return std::make_unique<WindowFunctionExecRemovableDocument>(
                                iter,
                                minMaxScalerExpr->input(),
                                minMaxScalerExpr->buildRemovable(),
                                docBounds,
                                memTracker);
                        }},
                    docBounds.lower);
            },
            [&](const WindowBounds::RangeBased& rangeBounds)
                -> std::unique_ptr<WindowFunctionExec> {
                // These checks should be enforced already during parsing.
                assertRangeBasedBoundsChecks(sortBy);
                auto sortByExpr = ExpressionFieldPath::createPathFromString(
                    expCtx, sortBy->begin()->fieldPath->fullPath(), expCtx->variablesParseState);
                if (holds_alternative<WindowBounds::Unbounded>(rangeBounds.lower)) {
                    // A left unbounded window will always be non-removable regardless of
                    // the upper bound.
                    return std::make_unique<WindowFunctionExecMinMaxScalerNonRemovableRange>(
                        iter,
                        minMaxScalerExpr->input(),
                        std::move(sortByExpr),
                        bounds,
                        memTracker,
                        minMaxScalerExpr->getDomainMinAndMax());
                } else {
                    return std::make_unique<WindowFunctionExecRemovableRange>(
                        iter,
                        minMaxScalerExpr->input(),
                        std::move(sortByExpr),
                        minMaxScalerExpr->buildRemovable(),
                        bounds,
                        memTracker);
                }
            },
        },
        bounds.bounds);
}

}  // namespace

std::unique_ptr<WindowFunctionExec> WindowFunctionExec::create(
    ExpressionContext* expCtx,
    PartitionIterator* iter,
    const WindowFunctionStatement& functionStmt,
    const boost::optional<SortPattern>& sortBy,
    MemoryUsageTracker* memTracker) {

    SimpleMemoryUsageTracker& functionMemTracker = (*memTracker)[functionStmt.fieldName];
    if (auto expr = dynamic_cast<window_function::ExpressionDerivative*>(functionStmt.expr.get())) {
        return translateDerivative(expr, iter, sortBy, &functionMemTracker);
    } else if (auto expr = dynamic_cast<window_function::ExpressionMinMaxScaler*>(
                   functionStmt.expr.get())) {
        return translateMinMaxScaler(expCtx, expr, iter, sortBy, &functionMemTracker);
    } else if (auto expr =
                   dynamic_cast<window_function::ExpressionFirst*>(functionStmt.expr.get())) {
        return std::make_unique<WindowFunctionExecFirst>(
            iter, expr->input(), expr->bounds(), boost::none, &functionMemTracker);
    } else if (auto expr =
                   dynamic_cast<window_function::ExpressionLast*>(functionStmt.expr.get())) {
        return std::make_unique<WindowFunctionExecLast>(
            iter, expr->input(), expr->bounds(), &functionMemTracker);
    } else if (auto expr =
                   dynamic_cast<window_function::ExpressionShift*>(functionStmt.expr.get())) {
        return std::make_unique<WindowFunctionExecFirst>(
            iter, expr->input(), expr->bounds(), expr->defaultVal(), &functionMemTracker);
    } else if (auto expr =
                   dynamic_cast<window_function::ExpressionLinearFill*>(functionStmt.expr.get())) {
        tassert(12728000,
                "$linearFill requires a 1-field non-expression sortBy",
                sortBy && sortBy->size() == 1 && !sortBy->begin()->expression);
        auto sortByExpr = ExpressionFieldPath::createPathFromString(
            expCtx, sortBy->begin()->fieldPath->fullPath(), expCtx->variablesParseState);
        return std::make_unique<WindowFunctionExecLinearFill>(
            iter, expr->input(), std::move(sortByExpr), expr->bounds(), &functionMemTracker);
    }

    WindowBounds bounds = functionStmt.expr->bounds();

    return visit(
        OverloadedVisitor{
            [&](const WindowBounds::DocumentBased& docBounds) {
                return translateDocumentWindow(
                    iter, functionStmt.expr, sortBy, docBounds, &functionMemTracker);
            },
            [&](const WindowBounds::RangeBased& rangeBounds)
                -> std::unique_ptr<WindowFunctionExec> {
                // These checks should be enforced already during parsing.
                assertRangeBasedBoundsChecks(sortBy);
                auto sortByExpr = ExpressionFieldPath::createPathFromString(
                    expCtx, sortBy->begin()->fieldPath->fullPath(), expCtx->variablesParseState);
                auto inputExpr = translateInputExpression(functionStmt.expr, sortBy);
                if (holds_alternative<WindowBounds::Unbounded>(rangeBounds.lower)) {
                    return std::make_unique<WindowFunctionExecNonRemovableRange>(
                        iter,
                        inputExpr,
                        std::move(sortByExpr),
                        functionStmt.expr->buildAccumulatorOnly(),
                        bounds,
                        &functionMemTracker);
                } else {
                    return std::make_unique<WindowFunctionExecRemovableRange>(
                        iter,
                        inputExpr,
                        std::move(sortByExpr),
                        functionStmt.expr->buildRemovable(),
                        bounds,
                        &functionMemTracker);
                }
            },
        },
        bounds.bounds);
}

}  // namespace mongo
