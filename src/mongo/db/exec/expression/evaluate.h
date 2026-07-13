// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/pipeline/expression_js_emit.h"
#include "mongo/db/pipeline/expression_test_feature_flags.h"
#include "mongo/db/pipeline/expression_trigonometric.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace exec::expression {

/**
 * Resolves the memory-usage tracker an expression evaluator should use. Returns the stage-owned
 * tracker when one was wired to 'ctx' (so usage is attributed to the stage); otherwise returns the
 * query-scoped fallback tracker owned by the expression's ExpressionContext (see
 * getExpressionFallbackTracker()). Contexts excluded from operation-wide memory tracking always
 * use the fallback tracker.
 */
inline SimpleMemoryUsageTracker& getMemoryTracker(const Expression& expr,
                                                  const EvaluationContext& ctx) {
    auto* expCtx = expr.getExpressionContext();
    if (ctx.tracker && !expCtx->getExcludeOperationMemoryTracking()) {
        return *ctx.tracker;
    }
    return expCtx->getExpressionFallbackTracker();
}

/**
 * Calls function 'function' with zero parameters and returns the result. If AssertionException is
 * raised during the call of 'function', adds all the context 'errorContext' to the exception.
 */
template <typename F, class... Args>
auto addContextToAssertionException(F&& function, Args... errorContext) {
    try {
        return function();
    } catch (AssertionException& exception) {
        str::stream ss;
        ((ss << errorContext), ...);
        exception.addContext(ss);
        throw;
    }
}

/**
 * Converts 'value' to TimeUnit for an expression named 'expressionName'. It assumes that the
 * parameter is named "unit". Throws an AssertionException if 'value' contains an invalid value.
 */
TimeUnit parseTimeUnit(const Value& value, std::string_view expressionName);

/**
 * Converts 'value' to DayOfWeek for an expression named 'expressionName' with parameter named as
 * 'parameterName'. Throws an AssertionException if 'value' contains an invalid value.
 */
DayOfWeek parseDayOfWeek(const Value& value,
                         std::string_view expressionName,
                         std::string_view parameterName);

/**
 * Evaluates the expression in 'timeZone', and loads the corresponding TimeZone object from the
 * 'tzdb' database. Throws an AssertionException if 'timeZone' doesn't contain a string or if it is
 * not the name of a valid timezone. Returns boost::none if 'timeZone' is Null.
 */
boost::optional<TimeZone> makeTimeZone(const TimeZoneDatabase* tzdb,
                                       const Document& root,
                                       const Expression* timeZone,
                                       Variables* variables,
                                       const EvaluationContext& ctx);

/**
 * Converts $dateTrunc expression parameter "binSize" 'value' to 64-bit integer.
 */
unsigned long long convertDateTruncBinSizeValue(const Value& value);

/**
 * Converts a 'val' value holding an array into a unordered set.
 */
ValueFlatUnorderedSet arrayToUnorderedSet(const Value& val, const ValueComparator& valueComparator);

/**
 * Converts a 'val' value holding an array into a unordered map, associating each unique value with
 * the list of array positions where the value can be found in the original array.
 */
ValueUnorderedMap<std::vector<int>> arrayToIndexMap(const Value& val,
                                                    const ValueComparator& valueComparator);


ExpressionRegex::PrecompiledRegex precompileRegex(const Value& regex,
                                                  const Value& options,
                                                  const std::string& opName);

Value evaluate(const ExpressionDateFromParts& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateFromString& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateToParts& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateToString& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateDiff& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateAdd& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateSubtract& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDateTrunc& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTsSecond& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTsIncrement& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDayOfMonth& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDayOfWeek& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDayOfYear& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHour& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionMillisecond& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionMinute& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionMonth& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSecond& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionWeek& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIsoWeekYear& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIsoDayOfWeek& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIsoWeek& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionYear& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionCurrentDate& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionArray& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionArrayElemAt& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionFirst& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionLast& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionObjectToArray& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionArrayToObject& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionConcatArrays& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIndexOfArray& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIsArray& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionReverseArray& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSortArray& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTopN& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTop& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBottomN& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBottom& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSetDifference& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSetEquals& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSetIntersection& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSetIsSubset& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSetUnion& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSlice& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSize& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionZip& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSimilarityDotProduct& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSimilarityCosine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSimilarityEuclidean& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionMap& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionReduce& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionFilter& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionConcat& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionReplaceOne& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionReplaceAll& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionStrcasecmp& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSubstrBytes& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSubstrCP& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionStrLenBytes& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBinarySize& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionStrLenCP& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionToLower& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionToUpper& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTrim& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSplit& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIndexOfBytes& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionIndexOfCP& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

/**
 * Adds two values as if by {$add: [{$const: lhs}, {$const: rhs}]}.
 *
 * If either argument is nullish, returns BSONNULL.
 *
 * Otherwise, returns ErrorCodes::TypeMismatch.
 */
StatusWith<Value> evaluateAdd(Value lhs, Value rhs);

/**
 * Subtracts two values as if by {$subtract: [{$const: lhs}, {$const: rhs}]}.
 *
 * If either argument is nullish, returns BSONNULL.
 *
 * Otherwise, the arguments can be either:
 *     (numeric, numeric)
 *     (Date, Date)       Returns the time difference in milliseconds.
 *     (Date, numeric)    Returns the date shifted earlier by that many milliseconds.
 *
 * Otherwise, returns ErrorCodes::TypeMismatch.
 */
StatusWith<Value> evaluateSubtract(Value lhs, Value rhs);

/**
 * Multiplies two values together as if by evaluate() on
 *     {$multiply: [{$const: lhs}, {$const: rhs}]}.
 *
 * Note that evaluate(ExpressionMultiply&) does not use evaluateMultiply() directly, because when
 * $multiply takes more than two arguments, it uses a wider intermediate state than Value.
 *
 * Returns BSONNULL if either argument is nullish.
 *
 * Returns ErrorCodes::TypeMismatch if any argument is non-nullish, non-numeric.
 */
StatusWith<Value> evaluateMultiply(Value lhs, Value rhs);

/**
 * Divides two values as if by {$divide: [{$const: numerator}, {$const: denominator]}.
 *
 * Returns BSONNULL if either argument is nullish.
 *
 * Returns ErrorCodes::TypeMismatch if either argument is non-nullish and non-numeric.
 * Returns ErrorCodes::BadValue if the denominator is zero.
 */
StatusWith<Value> evaluateDivide(Value lhs, Value rhs);

/**
 * Compute the remainder of the division of two values as if by {$mod: [{$const: numerator},
 * {$const: denominator]}.
 *
 * Returns BSONNULL if either argument is nullish.
 *
 * Returns an error the denominator is zero, or if either argument is non-nullish and non-numeric.
 */
StatusWith<Value> evaluateMod(Value lhs, Value rhs);

Value evaluate(const ExpressionAdd& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

inline Value evaluate(const ExpressionConstant& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    return expr.getValue();
}

inline Value evaluate(const ExpressionDivide& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    auto& children = expr.getChildren();
    return uassertStatusOK(evaluateDivide(children[0]->evaluate(root, variables, ctx),
                                          children[1]->evaluate(root, variables, ctx)));
}

inline Value evaluate(const ExpressionMod& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    auto& children = expr.getChildren();
    return uassertStatusOK(evaluateMod(children[0]->evaluate(root, variables, ctx),
                                       children[1]->evaluate(root, variables, ctx)));
}

Value evaluate(const ExpressionMultiply& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionLog& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionRandom& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionRange& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

inline Value evaluate(const ExpressionSubtract& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    auto& children = expr.getChildren();
    return uassertStatusOK(evaluateSubtract(children[0]->evaluate(root, variables, ctx),
                                            children[1]->evaluate(root, variables, ctx)));
}

Value evaluate(const ExpressionRound& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTrunc& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

inline Value evaluate(const ExpressionIsNumber& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    return Value(expr.getChildren()[0]->evaluate(root, variables, ctx).numeric());
}

Value evaluate(const ExpressionConvert& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionAbs& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionCeil& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionExp& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionPow& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionFloor& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionLn& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionLog10& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSqrt& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBitNot& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDegreesToRadians& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionRadiansToDegrees& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionArcTangent2& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionCosine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTangent& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionArcCosine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionArcSine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHyperbolicArcTangent& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHyperbolicArcCosine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionArcTangent& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHyperbolicArcSine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHyperbolicCosine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHyperbolicSine& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionHyperbolicTangent& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionFunction& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalJsEmit& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

inline Value evaluate(const ExpressionAnd& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    for (auto&& child : expr.getChildren()) {
        if (!child->evaluate(root, variables, ctx).coerceToBool()) {
            return Value(false);
        }
    }

    return Value(true);
}

Value evaluate(const ExpressionAllElementsTrue& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionAnyElementTrue& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

inline Value evaluate(const ExpressionCompare& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    const auto& children = expr.getChildren();
    int cmp = expr.getExpressionContext()->getValueComparator().compare(
        children[0]->evaluate(root, variables, ctx), children[1]->evaluate(root, variables, ctx));

    // Make cmp one of 1, 0, or -1.
    if (cmp == 0) {
        // leave as 0
    } else if (cmp < 0) {
        cmp = -1;
    } else if (cmp > 0) {
        cmp = 1;
    }

    if (expr.getOp() == ExpressionCompare::CmpOp::CMP) {
        return Value(cmp);
    }

    static const bool cmpLookup[6][3] = {
        /*          -1      0      1   */
        /* EQ  */ {false, true, false},
        /* NE  */ {true, false, true},
        /* GT  */ {false, false, true},
        /* GTE */ {false, true, true},
        /* LT  */ {true, false, false},
        /* LTE */ {true, true, false},

        // We don't require the lookup table for CMP.
    };

    bool returnValue = cmpLookup[expr.getOp()][cmp + 1];
    return Value(returnValue);
}

inline Value evaluate(const ExpressionCond& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    const auto& children = expr.getChildren();
    int idx = children[0]->evaluate(root, variables, ctx).coerceToBool() ? 1 : 2;
    return children[idx]->evaluate(root, variables, ctx);
}

inline Value evaluate(const ExpressionIfNull& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    const auto& children = expr.getChildren();
    const size_t n = children.size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(children[i]->evaluate(root, variables, ctx));
        if (!pValue.nullish() || i == n - 1) {
            return pValue;
        }
    }
    return Value();
}

Value evaluate(const ExpressionIn& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

inline Value evaluate(const ExpressionNot& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    return Value(!expr.getChildren()[0]->evaluate(root, variables, ctx).coerceToBool());
}

inline Value evaluate(const ExpressionOr& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    for (auto&& child : expr.getChildren()) {
        if (child->evaluate(root, variables, ctx).coerceToBool()) {
            return Value(true);
        }
    }

    return Value(false);
}

inline Value evaluate(const ExpressionSwitch& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    for (int i = 0; i < expr.numBranches(); ++i) {
        auto [caseExpr, thenExpr] = expr.getBranch(i);
        Value caseResult = caseExpr->evaluate(root, variables, ctx);

        if (caseResult.coerceToBool()) {
            return thenExpr->evaluate(root, variables, ctx);
        }
    }

    uassert(40066,
            "$switch could not find a matching branch for an input, and no default was specified.",
            expr.defaultExpr());

    return expr.defaultExpr()->evaluate(root, variables, ctx);
}

Value evaluate(const ExpressionBitAnd& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBitOr& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBitXor& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionRegexFind& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionRegexFindAll& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionRegexMatch& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionObject& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionBsonSize& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

/*
 * Helper function to evaluate ExpressionFieldPath, used recursively.
 *
 * The helper function doesn't just use a loop because of
 * the possibility that we need to skip over an array.  If the path
 * is "a.b.c", and a is an array, then we fan out from there, and
 * traverse "b.c" for each element of a:[...].  This requires that
 * a be an array of objects in order to navigate more deeply.
 *
 * @param fieldPath
 * @param index current path field index to extract
 * @param input current document traversed to (not the top-level one)
 * @returns the field found; could be an array
 */
Value evaluatePath(const FieldPath& fieldPath, size_t index, const Document& input);

/*
 * Helper for evaluatePath to handle Array case
 */
Value evaluatePathArray(const FieldPath& fieldPath, size_t index, const Value& input);

inline Value evaluate(const ExpressionFieldPath& expr,
                      const Document& root,
                      Variables* variables,
                      const EvaluationContext& ctx) {
    auto& fieldPath = expr.getFieldPath();
    auto variable = expr.getVariableId();

    if (fieldPath.getPathLength() == 1) {  // get the whole variable
        return variables->getValue(variable, root);
    }

    if (variable == Variables::kRootId) {
        // ROOT is always a document so use optimized code path
        return evaluatePath(fieldPath, 1, root);
    }

    Value var = variables->getValue(variable, root);
    switch (var.getType()) {
        case BSONType::object:
            return evaluatePath(fieldPath, 1, var.getDocument());
        case BSONType::array:
            return evaluatePathArray(fieldPath, 1, var);
        default:
            return Value();
    }
}

Value evaluate(const ExpressionGetField& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSetField& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalFindAllValuesAtPath& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionMeta& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionType& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionSubtype& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTestApiVersion& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionLet& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalRawSortKey& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionTestFeatureFlags& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionToHashedIndexKey& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalKeyStringValue& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionInternalFindPositional& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalFindSlice& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalFindElemMatch& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionInternalFLEEqual& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalFLEBetween& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionEncStrStartsWith& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionEncStrEndsWith& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionEncStrContains& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionEncStrNormalizedEq& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionSerializeEJSON& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionDeserializeEJSON& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

Value evaluate(const ExpressionHash& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

}  // namespace exec::expression
}  // namespace mongo
