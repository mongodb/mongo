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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"

#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/pipeline/window_function/window_function_exec_derivative.h"
#include "mongo/db/pipeline/window_function/window_function_exec_first_last.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_first_last_n.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_n_traits.h"
#include "mongo/db/pipeline/window_function/window_function_top_bottom_n.h"

using boost::intrusive_ptr;
using boost::optional;

namespace mongo::window_function {
using namespace std::string_literals;
using namespace window_function_n_traits;
REGISTER_STABLE_WINDOW_FUNCTION(derivative, ExpressionDerivative::parse);
REGISTER_STABLE_WINDOW_FUNCTION(first, ExpressionFirst::parse);
REGISTER_STABLE_WINDOW_FUNCTION(last, ExpressionLast::parse);
REGISTER_WINDOW_FUNCTION_CONDITIONALLY(linearFill,
                                       (ExpressionLinearFill::parse),
                                       feature_flags::gFeatureFlagFill.getVersion(),
                                       AllowedWithApiStrict::kNeverInVersion1,
                                       feature_flags::gFeatureFlagFill.isEnabledAndIgnoreFCV());
REGISTER_STABLE_WINDOW_FUNCTION(minN, (ExpressionN<WindowFunctionMinN, AccumulatorMinN>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(maxN, (ExpressionN<WindowFunctionMaxN, AccumulatorMaxN>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(firstN,
                                (ExpressionN<WindowFunctionFirstN, AccumulatorFirstN>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(lastN, (ExpressionN<WindowFunctionLastN, AccumulatorLastN>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(
    topN,
    (ExpressionN<WindowFunctionTopN, AccumulatorTopBottomN<TopBottomSense::kTop, false>>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(
    bottomN,
    (ExpressionN<WindowFunctionBottomN,
                 AccumulatorTopBottomN<TopBottomSense::kBottom, false>>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(
    top,
    (ExpressionN<WindowFunctionTop, AccumulatorTopBottomN<TopBottomSense::kTop, true>>::parse));
REGISTER_STABLE_WINDOW_FUNCTION(
    bottom,
    (ExpressionN<WindowFunctionBottom,
                 AccumulatorTopBottomN<TopBottomSense::kBottom, true>>::parse));

StringMap<Expression::ExpressionParserRegistration> Expression::parserMap;

intrusive_ptr<Expression> Expression::parse(BSONObj obj,
                                            const optional<SortPattern>& sortBy,
                                            ExpressionContext* expCtx) {
    for (const auto& field : obj) {
        // Check if window function is $-prefixed.
        auto fieldName = field.fieldNameStringData();

        if (fieldName.startsWith("$"_sd)) {
            auto exprName = field.fieldNameStringData();
            if (auto parserFCV = parserMap.find(exprName); parserFCV != parserMap.end()) {
                // Found one valid window function. If there are multiple window functions they will
                // be caught as invalid arguments to the Expression parser later.
                const auto& parserRegistration = parserFCV->second;
                const auto& parser = parserRegistration.parser;
                const auto& fcv = parserRegistration.fcv;
                uassert(ErrorCodes::QueryFeatureNotAllowed,
                        str::stream()
                            << exprName
                            << " is not allowed in the current feature compatibility version. See "
                            << feature_compatibility_version_documentation::kCompatibilityLink
                            << " for more information.",
                        !expCtx->maxFeatureCompatibilityVersion || !fcv ||
                            (*fcv <= *expCtx->maxFeatureCompatibilityVersion));

                auto allowedWithApi = parserRegistration.allowedWithApi;

                const auto opCtx = expCtx->opCtx;

                if (!opCtx) {
                    // It's expected that we always have an op context attached to the expression
                    // context for window functions.
                    MONGO_UNREACHABLE_TASSERT(6089901);
                }

                assertLanguageFeatureIsAllowed(
                    opCtx, exprName, allowedWithApi, AllowedWithClientType::kAny);

                expCtx->incrementWindowAccumulatorExprCounter(exprName);
                return parser(obj, sortBy, expCtx);
            }

            // The window function provided in the window function expression is invalid.

            // For example, in this window function expression:
            //     {$setWindowFields:
            //         {output:
            //             {total:
            //                 {$summ: "$x", windoww: {documents: ['unbounded', 'current']}
            //                 }
            //             }
            //         }
            //     }
            //
            // the window function, $summ, is invalid as it is mispelled.
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Unrecognized window function, " << fieldName);
        }
    }
    // The command did not contain any $-prefixed window functions.
    uasserted(ErrorCodes::FailedToParse,
              "Expected a $-prefixed window function"s +
                  (obj.firstElementFieldNameStringData().empty()
                       ? ""s
                       : ", "s + obj.firstElementFieldNameStringData()));
}

void Expression::registerParser(
    std::string functionName,
    Parser parser,
    boost::optional<multiversion::FeatureCompatibilityVersion> requiredMinVersion,
    AllowedWithApiStrict allowedWithApi) {
    invariant(parserMap.find(functionName) == parserMap.end());
    ExpressionParserRegistration r{parser, requiredMinVersion, allowedWithApi};
    operatorCountersWindowAccumulatorExpressions.addCounter(functionName);
    parserMap.emplace(std::move(functionName), std::move(r));
}


boost::intrusive_ptr<Expression> ExpressionExpMovingAvg::parse(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {
    // 'obj' is something like '{$expMovingAvg: {input: <arg>, <N/alpha>: <int/float>}}'
    boost::optional<StringData> accumulatorName;
    boost::intrusive_ptr<::mongo::Expression> input;
    uassert(ErrorCodes::FailedToParse,
            "$expMovingAvg must have exactly one argument that is an object",
            obj.nFields() == 1 && obj.hasField(kAccName) &&
                obj[kAccName].type() == BSONType::Object);
    auto subObj = obj[kAccName].embeddedObject();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$expMovingAvg sub object must have exactly two fields: An '"
                          << kInputArg << "' field, and either an '" << kNArg << "' field or an '"
                          << kAlphaArg << "' field",
            subObj.nFields() == 2 && subObj.hasField(kInputArg));
    uassert(ErrorCodes::FailedToParse, "$expMovingAvg requires an explicit 'sortBy'", sortBy);
    input =
        ::mongo::Expression::parseOperand(expCtx, subObj[kInputArg], expCtx->variablesParseState);
    // ExpMovingAvg is always unbounded to current.
    WindowBounds bounds = WindowBounds{
        WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Current{}}};
    if (subObj.hasField(kNArg)) {
        auto nVal = subObj[kNArg];
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kNArg << "' field must be an integer, but found type "
                              << nVal.type(),
                nVal.isNumber());
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kNArg << "' field must be an integer, but found  " << nVal
                              << ". To use a non-integer, use the '" << kAlphaArg
                              << "' argument instead",
                nVal.safeNumberDouble() == floor(nVal.safeNumberDouble()));
        auto nNum = nVal.safeNumberLong();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kNArg << "' must be greater than zero. Got " << nNum,
                nNum > 0);
        return make_intrusive<ExpressionExpMovingAvg>(
            expCtx, std::string(kAccName), std::move(input), std::move(bounds), nNum);
    } else if (subObj.hasField(kAlphaArg)) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kAlphaArg << "' must be a number",
                subObj[kAlphaArg].isNumber());
        auto alpha = subObj[kAlphaArg].numberDecimal();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kAlphaArg << "' must be between 0 and 1 (exclusive), found "
                              << subObj[kAlphaArg],
                alpha.isGreater(Decimal128(0)) && alpha.isLess(Decimal128(1.0)));
        return make_intrusive<ExpressionExpMovingAvg>(
            expCtx, std::string(kAccName), std::move(input), std::move(bounds), std::move(alpha));
    } else {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Got unrecognized field in $expMovingAvg"
                                << "$expMovingAvg sub object must have exactly two fields: An '"
                                << kInputArg << "' field, and either an '" << kNArg
                                << "' field or an '" << kAlphaArg << "' field");
    }
}

boost::intrusive_ptr<Expression> ExpressionFirstLast::parse(
    BSONObj obj,
    const boost::optional<SortPattern>& sortBy,
    ExpressionContext* expCtx,
    Sense sense) {
    // Example document:
    // {
    //   accumulatorName: <expr>,
    //   window: {...} // optional
    // }

    const std::string& accumulatorName = senseToAccumulatorName(sense);
    boost::optional<WindowBounds> bounds;
    boost::intrusive_ptr<::mongo::Expression> input;
    for (const auto& arg : obj) {
        auto argName = arg.fieldNameStringData();
        if (argName == kWindowArg) {
            uassert(ErrorCodes::FailedToParse,
                    "'window' field must be an object",
                    obj[kWindowArg].type() == BSONType::Object);
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "saw multiple 'window' fields in '" << accumulatorName
                                  << "' expression",
                    bounds == boost::none);
            bounds = WindowBounds::parse(arg.embeddedObject(), sortBy, expCtx);
        } else if (argName == StringData(accumulatorName)) {
            input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);

        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << accumulatorName << " got unexpected argument: " << argName);
        }
    }
    tassert(ErrorCodes::FailedToParse,
            str::stream() << accumulatorName << " parser called with no " << accumulatorName
                          << " key",
            input);

    // The default window bounds are [unbounded, unbounded].
    if (!bounds) {
        bounds = WindowBounds{
            WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Unbounded{}}};
    }

    switch (sense) {
        case Sense::kFirst:
            return make_intrusive<ExpressionFirst>(expCtx, std::move(input), std::move(*bounds));
        case Sense::kLast:
            return make_intrusive<ExpressionLast>(expCtx, std::move(input), std::move(*bounds));
        default:
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << accumulatorName << " is not $first or $last");
            return nullptr;
    }
}

template <typename WindowFunctionN, typename AccumulatorNType>
Value ExpressionN<WindowFunctionN, AccumulatorNType>::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    auto acc = buildAccumulatorOnly();
    MutableDocument result(acc->serialize(nExpr, _input, static_cast<bool>(explain)));

    MutableDocument windowField;
    _bounds.serialize(windowField);
    result[kWindowArg] = windowField.freezeToValue();
    return result.freezeToValue();
}

template <typename WindowFunctionN, typename AccumulatorNType>
boost::intrusive_ptr<AccumulatorState>
ExpressionN<WindowFunctionN, AccumulatorNType>::buildAccumulatorOnly() const {
    static_assert(isWindowFunctionN<WindowFunctionN>::value,
                  "tried to use ExpressionN with an unsupported window function");
    boost::intrusive_ptr<AccumulatorState> acc;
    if constexpr (!needsSortBy<WindowFunctionN>::value) {
        tassert(5788606,
                str::stream() << AccumulatorNType::getName()
                              << " should not have recieved a 'sortBy' but did!",
                !sortPattern);

        acc = AccumulatorNType::create(_expCtx);
    } else {
        tassert(5788601,
                str::stream() << AccumulatorNType::getName()
                              << " should have recieved a 'sortBy' but did not!",
                sortPattern);
        acc = AccumulatorNType::create(_expCtx, *sortPattern);
    }

    // Initialize 'n' for our accumulator. At this point we don't have any user defined variables
    // so you physically can't reference the partition key in 'n'. It will evaluate to MISSING and
    // fail validation done in startNewGroup().
    auto nVal = nExpr->evaluate({}, &_expCtx->variables);
    acc->startNewGroup(nVal);
    return acc;
}

template <typename WindowFunctionN, typename AccumulatorNType>
std::unique_ptr<WindowFunctionState>
ExpressionN<WindowFunctionN, AccumulatorNType>::buildRemovable() const {
    if constexpr (needsSortBy<WindowFunctionN>::value) {
        tassert(5788602,
                str::stream() << AccumulatorNType::getName()
                              << " should have recieved a 'sortBy' but did not!",
                sortPattern);
        return WindowFunctionN::create(
            _expCtx,
            *sortPattern,
            AccumulatorN::validateN(nExpr->evaluate({}, &_expCtx->variables)));
    } else {
        return WindowFunctionN::create(
            _expCtx, AccumulatorN::validateN(nExpr->evaluate({}, &_expCtx->variables)));
    }
}

template <typename WindowFunctionN, typename AccumulatorNType>
boost::intrusive_ptr<Expression> ExpressionN<WindowFunctionN, AccumulatorNType>::parse(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {
    auto name = AccumulatorNType::getName();

    // This is for the sortBy to this specific window function if we are parsing
    // top/bottom/topN/bottomN, not the sortBy parameter to $setWindowFields.
    boost::optional<SortPattern> innerSortPattern;
    boost::intrusive_ptr<::mongo::Expression> nExpr;
    boost::intrusive_ptr<::mongo::Expression> outputExpr;
    boost::optional<WindowBounds> bounds;
    for (auto&& elem : obj) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == name) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "saw multiple specifications for '" << name << "' expression",
                    !(nExpr || outputExpr));

            auto accExpr = WindowFunctionN::parse(expCtx, elem, expCtx->variablesParseState);
            nExpr = std::move(accExpr.initializer);
            outputExpr = std::move(accExpr.argument);
            // For top/bottom/topN/bottomN we also need a sortPattern. It was already validated when
            // we called parse, so here we just grab it again for constructing future instances.
            if constexpr (needsSortBy<WindowFunctionN>::value) {
                auto innerSortByBson = elem[AccumulatorN::kFieldNameSortBy];
                tassert(5788604,
                        str::stream()
                            << "expected 'sortBy' to already be an object in the arguments to "
                            << AccumulatorNType::getName(),
                        innerSortByBson.type() == BSONType::Object);
                innerSortPattern.emplace(innerSortByBson.embeddedObject(), expCtx);
            }
        } else if (fieldName == kWindowArg) {
            uassert(ErrorCodes::FailedToParse,
                    "'window' field must be an object",
                    obj[kWindowArg].type() == BSONType::Object);
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "saw multiple 'window' fields in '" << name << "' expression",
                    bounds == boost::none);
            bounds = WindowBounds::parse(elem.embeddedObject(), sortBy, expCtx);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << name << " got unexpected argument: " << fieldName);
        }
    }

    // The default window bounds are [unbounded, unbounded].
    if (!bounds) {
        bounds = WindowBounds::defaultBounds();
    }
    tassert(5788403,
            str::stream() << "missing accumulator specification for " << name,
            nExpr && outputExpr);
    return make_intrusive<ExpressionN<WindowFunctionN, AccumulatorNType>>(
        expCtx,
        std::move(outputExpr),
        std::string(name),
        *bounds,
        std::move(nExpr),
        std::move(innerSortPattern));
}

MONGO_INITIALIZER_GROUP(BeginWindowFunctionRegistration,
                        ("default"),
                        ("EndWindowFunctionRegistration"))
MONGO_INITIALIZER_GROUP(EndWindowFunctionRegistration, ("BeginWindowFunctionRegistration"), ())
}  // namespace mongo::window_function
