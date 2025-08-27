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

#include "mongo/db/pipeline/window_function/window_function_expression.h"

#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/window_function/window_function_expression_gen.h"
#include "mongo/db/pipeline/window_function/window_function_first_last_n.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_n_traits.h"
#include "mongo/db/pipeline/window_function/window_function_percentile.h"
#include "mongo/db/pipeline/window_function/window_function_top_bottom_n.h"
#include "mongo/db/stats/counters.h"

#include <cmath>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;
using boost::optional;

namespace mongo::window_function {
using namespace std::string_literals;
using namespace window_function_n_traits;
REGISTER_STABLE_WINDOW_FUNCTION(derivative, ExpressionDerivative::parse);
REGISTER_STABLE_WINDOW_FUNCTION(first, ExpressionFirst::parse);
REGISTER_STABLE_WINDOW_FUNCTION(last, ExpressionLast::parse);
REGISTER_STABLE_WINDOW_FUNCTION(linearFill, ExpressionLinearFill::parse);
REGISTER_WINDOW_FUNCTION_WITH_FEATURE_FLAG(minMaxScaler,
                                           ExpressionMinMaxScaler::parse,
                                           &feature_flags::gFeatureFlagSearchHybridScoringFull,
                                           AllowedWithApiStrict::kNeverInVersion1);
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

REGISTER_STABLE_WINDOW_FUNCTION(
    percentile, (window_function::ExpressionQuantile<AccumulatorPercentile>::parse));

REGISTER_STABLE_WINDOW_FUNCTION(median,
                                (window_function::ExpressionQuantile<AccumulatorMedian>::parse));
StringMap<Expression::ExpressionParserRegistration> Expression::parserMap;

intrusive_ptr<Expression> Expression::parse(BSONObj obj,
                                            const optional<SortPattern>& sortBy,
                                            ExpressionContext* expCtx) {
    for (const auto& field : obj) {
        // Check if window function is $-prefixed.
        auto fieldName = field.fieldNameStringData();

        if (fieldName.starts_with("$"_sd)) {
            auto exprName = field.fieldNameStringData();
            if (auto parserFCV = parserMap.find(exprName); parserFCV != parserMap.end()) {
                // Found one valid window function. If there are multiple window functions they will
                // be caught as invalid arguments to the Expression parser later.
                const auto& parserRegistration = parserFCV->second;
                const auto& parser = parserRegistration.parser;
                const auto& featureFlag = parserRegistration.featureFlag;

                if (featureFlag) {
                    expCtx->ignoreFeatureInParserOrRejectAndThrow(exprName, *featureFlag);
                }

                auto allowedWithApi = parserRegistration.allowedWithApi;

                const auto opCtx = expCtx->getOperationContext();

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

void Expression::registerParser(std::string functionName,
                                Parser parser,
                                FeatureFlag* featureFlag,
                                AllowedWithApiStrict allowedWithApi) {
    auto op = parserMap.find(functionName);
    uassert(10021101,
            str::stream() << "Duplicate parsers (" << functionName << ") registered.",
            op == parserMap.end());
    ExpressionParserRegistration r{parser, featureFlag, allowedWithApi};
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
                obj[kAccName].type() == BSONType::object);
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
                    str::stream() << "saw multiple 'window' fields in '" << accumulatorName
                                  << "' expression",
                    bounds == boost::none);
            bounds = WindowBounds::parse(arg, sortBy, expCtx);
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

namespace {
// Parse and validate the window bounds for $minMaxScaler.
WindowBounds getMinMaxScalerWindowBoundsFromSpec(const MinMaxScalerSpec& spec,
                                                 const boost::optional<SortPattern>& sortBy,
                                                 ExpressionContext* expCtx) {
    if (!spec.getWindow().has_value()) {
        // No bounds were specified by this spec.
        // Use the default ones (unbounded).
        return WindowBounds::defaultBounds();
    }

    auto bounds =
        WindowBounds::parse(BSON("" << spec.getWindow().value()).firstElement(), sortBy, expCtx);

    // If bounds have been specified, we must ensure that the configured window will always
    // include the current document. This is because $minMaxScaler computes the relative
    // percentage that each document is between the min and max of the window, thus the
    // current document must be in the current window to ensure its bounded between the min
    // and the max values. Practically, we check that the lower bound is not an
    // index greater than the current document (0), and that the maximum is not an index
    // less than the current document (0). The computation is equivalent for both document
    // and range based bounds, because range based bounds always require that the numerical
    // bounds tolerances are relative to the values that the documents are sorted by.
    //
    // Note that descending sort bounds work for $minMaxScaler when the bounds are Document based
    // (provided that the bounds continue to meet the criteria that the lower cannot be greater than
    // 0 and the upper cannot be less than 0). This is because in reversed sort order, the window is
    // still sorted so adding or removing documents to the window continues to guarantee that the
    // current document is still bounded between the min and max of the window.
    //
    // Get a bound value as a number. The first value of the return is the bound value,
    // the second value is whether or not the bound is numerically expressable.
    // Non-numerical bounds ("current" / "unbounded") do not need to be checked as they
    // will always include the current document in the window.
    // Pass false to get the lower bound, and true to get the upper bound.
    auto getBoundAsNumeric = [&](bool lower) -> std::pair<double, bool> {
        return visit(
            OverloadedVisitor{
                [&](const WindowBounds::DocumentBased& docBounds) -> std::pair<double, bool> {
                    return visit(OverloadedVisitor{
                                     [&](const int bound) -> std::pair<double, bool> {
                                         return {bound, true};
                                     },
                                     [&](const auto& bound) -> std::pair<double, bool> {
                                         return {0, false};
                                     },
                                 },
                                 lower ? docBounds.lower : docBounds.upper);
                },
                [&](const WindowBounds::RangeBased& rangeBounds) -> std::pair<double, bool> {
                    return visit(OverloadedVisitor{
                                     [&](const Value bound) -> std::pair<double, bool> {
                                         return {bound.coerceToDouble(), true};
                                     },
                                     [&](const auto& bound) -> std::pair<double, bool> {
                                         return {0, false};
                                     },
                                 },
                                 lower ? rangeBounds.lower : rangeBounds.upper);
                },
            },
            bounds.bounds);
    };

    auto lowerBound = getBoundAsNumeric(true);
    if (lowerBound.second) {
        uassert(ErrorCodes::FailedToParse,
                "Lower specified bound cannot be greater than 0 (the current doc), as "
                "$minMaxScaler must ensure that the current document being processed is always "
                "within the configured window. Lower specified bound = " +
                    std::to_string(lowerBound.first),
                lowerBound.first <= 0);
    }
    auto upperBound = getBoundAsNumeric(false);
    if (upperBound.second) {
        uassert(ErrorCodes::FailedToParse,
                "Upper specified bound cannot be less than 0 (the current doc), as "
                "$minMaxScaler must ensure that the current document being processed is always "
                "within the configured window. Upper specified bound = " +
                    std::to_string(upperBound.first),
                upperBound.first >= 0);
    }

    return bounds;
}

// This struct represents the deserialized arguments to the $minMaxScaler window function.
struct MinMaxScalerArguments {
    // The input expression to calculate for $minMaxScaler.
    boost::intrusive_ptr<::mongo::Expression> input;

    // The first Value is the min, the second value is the max.
    std::pair<Value, Value> minAndMax;
};

// Parse and validate the arguments of $minMaxScaler.
MinMaxScalerArguments getMinMaxScalerArgumentsFromSpec(const MinMaxScalerSpec& spec,
                                                       ExpressionContext* expCtx) {
    auto minMaxScaler = spec.getMinMaxScaler();

    boost::intrusive_ptr<::mongo::Expression> input = ::mongo::Expression::parseOperand(
        expCtx, minMaxScaler.getInput().getElement(), expCtx->variablesParseState);

    // This helper function takes in a constant numerical expression and evaluates it into a Value.
    auto parseNumericalValueConstant = [&expCtx](StringData argName,
                                                 BSONElement expressionElem) -> Value {
        auto expr =
            ::mongo::Expression::parseOperand(expCtx, expressionElem, expCtx->variablesParseState)
                ->optimize();
        ExpressionConstant* exprConst = dynamic_cast<ExpressionConstant*>(expr.get());
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << argName << "' argument to $minMaxScaler must be a constant",
                exprConst);
        Value v = exprConst->getValue();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << argName
                              << "' argument to $minMaxScaler must be a numeric type",
                v.numeric());
        return v;
    };

    uassert(ErrorCodes::FailedToParse,
            "Only one of 'min' and 'max' were specified as an argument to $minMaxScaler."
            " Neither or both must be specified",
            // XNOR will be false iff one of the values are true.
            !(minMaxScaler.getMin().has_value() ^ minMaxScaler.getMax().has_value()));

    std::pair<Value, Value> sMinAndsMax{0, 1};
    // Note that only one of getMin().has_value() or getMax().has_value() is necessary here as a
    // conditional, because the two must have the same value (as checked above). However, for
    // clarity in code reading, check both here.
    if (minMaxScaler.getMin().has_value() && minMaxScaler.getMax().has_value()) {
        sMinAndsMax.first = parseNumericalValueConstant(ExpressionMinMaxScaler::kMinArg,
                                                        minMaxScaler.getMin()->getElement());
        sMinAndsMax.second = parseNumericalValueConstant(ExpressionMinMaxScaler::kMaxArg,
                                                         minMaxScaler.getMax()->getElement());
        // Max must be strictly greater than min.
        uassert(ErrorCodes::FailedToParse,
                "the 'max' must be strictly greater than 'min', as arguments to $minMaxScaler",
                Value::compare(sMinAndsMax.first, sMinAndsMax.second, nullptr) < 0);
    }

    return MinMaxScalerArguments{input, sMinAndsMax};
}
}  // namespace

boost::intrusive_ptr<Expression> ExpressionMinMaxScaler::parse(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {
    // expected 'obj' format:
    // {
    //   $minMaxScaler: {
    //      input: <expr>
    //      min: <constant numerical expr> // optional, default 0
    //      max: <constant numerical expr> // optional, default 1
    //   },
    //   window: {...} // optional, default ['unbounded', 'unbounded']
    // }

    auto minMaxScalerSpec = MinMaxScalerSpec::parse(obj, IDLParserContext("root"));
    auto bounds = getMinMaxScalerWindowBoundsFromSpec(minMaxScalerSpec, sortBy, expCtx);
    auto minMaxArgs = getMinMaxScalerArgumentsFromSpec(minMaxScalerSpec, expCtx);

    expCtx->setSbeWindowCompatibility(SbeCompatibility::notCompatible);
    return make_intrusive<ExpressionMinMaxScaler>(
        expCtx, minMaxArgs.input, std::move(bounds), std::move(minMaxArgs.minAndMax));
}

template <typename WindowFunctionN, typename AccumulatorNType>
Value ExpressionN<WindowFunctionN, AccumulatorNType>::serialize(
    const SerializationOptions& opts) const {
    // Create but don't initialize the accumulator for serialization. This is because initialization
    // evaluates and validates the 'n' expression, which is unnecessary for this case and can cause
    // errors for query stats.
    auto acc = createAccumulatorWithoutInitializing();

    MutableDocument result(acc->serialize(nExpr, _input, opts));

    MutableDocument windowField;
    _bounds.serialize(windowField, opts);
    result[kWindowArg] = windowField.freezeToValue();
    return result.freezeToValue();
}

template <typename WindowFunctionN, typename AccumulatorNType>
boost::intrusive_ptr<AccumulatorState>
ExpressionN<WindowFunctionN, AccumulatorNType>::createAccumulatorWithoutInitializing() const {
    static_assert(isWindowFunctionN<WindowFunctionN>::value,
                  "tried to use ExpressionN with an unsupported window function");
    if constexpr (!needsSortBy<WindowFunctionN>::value) {
        tassert(5788606,
                str::stream() << AccumulatorNType::getName()
                              << " should not have received a 'sortBy' but did!",
                !sortPattern);

        return make_intrusive<AccumulatorNType>(_expCtx);
    } else {
        tassert(5788601,
                str::stream() << AccumulatorNType::getName()
                              << " should have received a 'sortBy' but did not!",
                sortPattern);
        return make_intrusive<AccumulatorNType>(_expCtx, *sortPattern);
    }
}

template <typename WindowFunctionN, typename AccumulatorNType>
boost::intrusive_ptr<AccumulatorState>
ExpressionN<WindowFunctionN, AccumulatorNType>::buildAccumulatorOnly() const {
    boost::intrusive_ptr<AccumulatorState> acc = createAccumulatorWithoutInitializing();

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
                              << " should have received a 'sortBy' but did not!",
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
                        innerSortByBson.type() == BSONType::object);
                innerSortPattern.emplace(innerSortByBson.embeddedObject(), expCtx);
            }
        } else if (fieldName == kWindowArg) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "saw multiple 'window' fields in '" << name << "' expression",
                    bounds == boost::none);
            bounds = WindowBounds::parse(elem, sortBy, expCtx);
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

template <typename AccumulatorTType>
boost::intrusive_ptr<Expression> ExpressionQuantile<AccumulatorTType>::parse(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {

    std::vector<double> ps;
    PercentileMethodEnum method = PercentileMethodEnum::kApproximate;
    boost::intrusive_ptr<::mongo::Expression> outputExpr;
    boost::intrusive_ptr<::mongo::Expression> initializeExpr;  // need for serializer.
    boost::optional<WindowBounds> bounds = WindowBounds::defaultBounds();
    auto name = AccumulatorTType::kName;

    for (auto&& elem : obj) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == name) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "saw multiple specifications for '" << name << "expression ",
                    !(initializeExpr || outputExpr));
            auto accExpr = AccumulatorTType::parseArgs(expCtx, elem, expCtx->variablesParseState);
            outputExpr = std::move(accExpr.argument);
            initializeExpr = std::move(accExpr.initializer);

            // Retrieve the values of 'ps' and 'method' from the accumulator's IDL parser.
            std::tie(ps, method) = AccumulatorTType::parsePercentileAndMethod(
                expCtx, elem, expCtx->variablesParseState);

        } else if (fieldName == kWindowArg) {
            bounds = WindowBounds::parse(elem, sortBy, expCtx);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << name << " got unexpected argument: " << fieldName);
        }
    }

    uassert(7455900,
            str::stream() << "Missing or incomplete accumulator specification for " << name,
            initializeExpr && outputExpr && !ps.empty());

    return make_intrusive<ExpressionQuantile>(
        expCtx, std::string(name), std::move(outputExpr), initializeExpr, *bounds, ps, method);
}

template <typename AccumulatorTType>
Value ExpressionQuantile<AccumulatorTType>::serialize(const SerializationOptions& opts) const {
    MutableDocument result;

    MutableDocument md;
    AccumulatorTType::serializeHelper(_input, opts, _ps, _method, md);
    result[AccumulatorTType::kName] = md.freezeToValue();

    MutableDocument windowField;
    _bounds.serialize(windowField, opts);
    result[kWindowArg] = windowField.freezeToValue();
    return result.freezeToValue();
}

template <typename AccumulatorTType>
std::unique_ptr<WindowFunctionState> ExpressionQuantile<AccumulatorTType>::buildRemovable() const {
    if (AccumulatorTType::kName == AccumulatorMedian::kName) {
        return WindowFunctionMedian::create(_expCtx, _method);
    } else {
        return WindowFunctionPercentile::create(_expCtx, _method, _ps);
    }
}

template <typename AccumulatorTType>
boost::intrusive_ptr<AccumulatorState> ExpressionQuantile<AccumulatorTType>::buildAccumulatorOnly()
    const {
    return make_intrusive<AccumulatorTType>(_expCtx, _ps, _method);
}


MONGO_INITIALIZER_GROUP(BeginWindowFunctionRegistration,
                        ("default"),
                        ("EndWindowFunctionRegistration"))
MONGO_INITIALIZER_GROUP(EndWindowFunctionRegistration, ("BeginWindowFunctionRegistration"), ())
}  // namespace mongo::window_function
