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

#include "mongo/base/initializer.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo {
class WindowFunctionExec;
class PartitionIterator;
}  // namespace mongo

#define REGISTER_STABLE_WINDOW_FUNCTION(name, parser) \
    REGISTER_WINDOW_FUNCTION_CONDITIONALLY(           \
        name, parser, boost::none, AllowedWithApiStrict::kAlways, true)

#define REGISTER_WINDOW_FUNCTION(name, parser, allowedWithApi) \
    REGISTER_WINDOW_FUNCTION_CONDITIONALLY(name, parser, boost::none, allowedWithApi, true)

/**
 * We store featureFlag in the parserMap, so that it can be checked at runtime to correctly
 * enable/disable the parser.
 */
#define REGISTER_WINDOW_FUNCTION_WITH_FEATURE_FLAG(name, parser, featureFlag, allowedWithApi) \
    REGISTER_WINDOW_FUNCTION_CONDITIONALLY(name, parser, featureFlag, allowedWithApi, true)

#define REGISTER_WINDOW_FUNCTION_CONDITIONALLY(name, parser, featureFlag, allowedWithApi, ...) \
    MONGO_INITIALIZER_GENERAL(addToWindowFunctionMap_##name,                                   \
                              ("BeginWindowFunctionRegistration"),                             \
                              ("EndWindowFunctionRegistration"))                               \
    (InitializerContext*) {                                                                    \
        if (!__VA_ARGS__ ||                                                                    \
            (boost::optional<FeatureFlag>(featureFlag) != boost::none &&                       \
             !boost::optional<FeatureFlag>(featureFlag)                                        \
                  ->isEnabledAndIgnoreFCVUnsafeAtStartup())) {                                 \
            return;                                                                            \
        }                                                                                      \
        ::mongo::window_function::Expression::registerParser(                                  \
            "$" #name, parser, featureFlag, allowedWithApi);                                   \
    }

#define REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(name, accumClass, wfClass)           \
    MONGO_INITIALIZER_GENERAL(addToWindowFunctionMap_##name,                           \
                              ("BeginWindowFunctionRegistration"),                     \
                              ("EndWindowFunctionRegistration"))                       \
    (InitializerContext*) {                                                            \
        ::mongo::window_function::Expression::registerParser(                          \
            "$" #name,                                                                 \
            ::mongo::window_function::ExpressionRemovable<accumClass, wfClass>::parse, \
            boost::none,                                                               \
            AllowedWithApiStrict::kAlways);                                            \
    }


namespace mongo::window_function {

/**
 * A window-function expression describes how to compute a single output value in a
 * $setWindowFields stage. For example, in
 *
 *     {$setWindowFields: {
 *         output: {
 *             totalCost: {$sum: "$price"},
 *             numItems: {$count: {}},
 *         }
 *     }}
 *
 * the two window-function expressions are {$sum: "$price"} and {$count: {}}.
 *
 * Because this class is part of a syntax tree, it does not hold any execution state:
 * instead it lets you create new instances of a window-function state.
 */
class Expression : public RefCountable {
public:
    static constexpr StringData kWindowArg = "window"_sd;
    /**
     * Parses a single window-function expression. One of the BSONObj's keys is the function
     * name, and the other (optional) key is 'window': for example, the whole BSONObj might be
     * {$sum: "$x"} or {$sum: "$x", window: {documents: [2,3]}}.
     *
     * 'sortBy' is from the sortBy argument of $setWindowFields. Some window functions require
     * a sort spec, or require a one-field sort spec; they use this argument to enforce those
     * requirements.
     *
     * If the window function accepts bounds, parse() parses them, from the window field. For window
     * functions like $rank, which don't accept bounds, parse() is responsible for throwing a parse
     * error, just like other unexpected arguments.
     */
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx);

    /**
     * A Parser has the same signature as parse(). The BSONObj is the whole expression, as
     * described above, because some parsers need to switch on the function name.
     */
    using Parser = std::function<decltype(parse)>;

    struct ExpressionParserRegistration {
        Parser parser;
        boost::optional<FeatureFlag> featureFlag;
        AllowedWithApiStrict allowedWithApi;
    };

    static void registerParser(std::string functionName,
                               Parser parser,
                               boost::optional<FeatureFlag> featureFlag,
                               AllowedWithApiStrict allowedWithApi);

    /**
     * Is this a function that the parser knows about?
     */
    inline static bool isFunction(const mongo::StringData& name) {
        return parserMap.find(name) != parserMap.end();
    }

    /**
     * Optimizes the input expression using its own optimize() method.
     */
    void optimize() {
        if (_input) {
            _input = _input->optimize();
        }
    }

    Expression(ExpressionContext* expCtx,
               std::string accumulatorName,
               boost::intrusive_ptr<::mongo::Expression> input,
               WindowBounds bounds)
        : _expCtx(expCtx),
          _accumulatorName(accumulatorName),
          _input(std::move(input)),
          _bounds(std::move(bounds)) {}

    std::string getOpName() const {
        return _accumulatorName;
    }

    WindowBounds bounds() const {
        return _bounds;
    }

    boost::intrusive_ptr<::mongo::Expression> input() const {
        return _input;
    }

    auto expCtx() const {
        return _expCtx;
    }

    virtual boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const = 0;

    virtual std::unique_ptr<WindowFunctionState> buildRemovable() const = 0;

    virtual void addDependencies(DepsTracker* deps) const {
        if (_input) {
            expression::addDependencies(_input.get(), deps);
        }
    }

    virtual void addVariableRefs(std::set<Variables::Id>* refs) const {
        if (_input) {
            expression::addVariableRefs(_input.get(), refs);
        }
    }

    virtual Value serialize(SerializationOptions opts) const {
        MutableDocument args;

        args[_accumulatorName] = _input->serialize(opts);
        MutableDocument windowField;
        _bounds.serialize(windowField, opts);
        args[kWindowArg] = windowField.freezeToValue();
        return args.freezeToValue();
    }

protected:
    ExpressionContext* _expCtx;
    std::string _accumulatorName;
    boost::intrusive_ptr<::mongo::Expression> _input;

    /**
     * Some window functions do not accept bounds in their syntax ($rank).
     * In these cases, this field is ignored.
     */
    WindowBounds _bounds;
    static StringMap<ExpressionParserRegistration> parserMap;
};

template <typename NonRemovableType>
class ExpressionFromAccumulator : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // 'obj' is something like '{$func: <args>, window: {...}}'
        boost::optional<StringData> accumulatorName;
        WindowBounds bounds = WindowBounds::defaultBounds();
        boost::intrusive_ptr<::mongo::Expression> input;
        for (const auto& arg : obj) {
            auto argName = arg.fieldNameStringData();
            if (argName == kWindowArg) {
                bounds = WindowBounds::parse(arg, sortBy, expCtx);
            } else if (isFunction(argName)) {
                uassert(ErrorCodes::FailedToParse,
                        "Cannot specify two functions in window function spec",
                        !accumulatorName);
                accumulatorName = argName;
                input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Window function found an unknown argument: " << argName);
            }
        }

        uassert(ErrorCodes::FailedToParse,
                "Must specify a window function in output field",
                accumulatorName);
        return make_intrusive<ExpressionFromAccumulator<NonRemovableType>>(
            expCtx, accumulatorName->toString(), std::move(input), std::move(bounds));
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return NonRemovableType::create(_expCtx);
    }
    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        uasserted(5461500,
                  str::stream() << "Window function " << _accumulatorName
                                << " is not supported with a removable window");
    }

    ExpressionFromAccumulator(ExpressionContext* expCtx,
                              std::string accumulatorName,
                              boost::intrusive_ptr<::mongo::Expression> input,
                              WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}
};

/**
 * Similar to ExpressionFromAccumulator except only an expression argument is allowed.
 */
template <typename FunctionType>
class ExpressionFromLeftUnboundedWindowFunction : public Expression {
public:
    ExpressionFromLeftUnboundedWindowFunction(ExpressionContext* expCtx,
                                              std::string accumulatorName,
                                              boost::intrusive_ptr<::mongo::Expression> input,
                                              WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // 'obj' is something like '{$func: <expressionArg>}'
        boost::optional<StringData> accumulatorName;
        // These expressions have variable lower bounds, but the functions themselves will handle
        // the specifics of what documents to count. All documents preceding current must be
        // seen by the function.
        WindowBounds bounds = WindowBounds{
            WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Current{}}};
        boost::intrusive_ptr<::mongo::Expression> input;
        bool windowFieldMissing = true;
        for (const auto& arg : obj) {
            auto argName = arg.fieldNameStringData();
            if (argName == kWindowArg) {
                windowFieldMissing = false;
            } else if (isFunction(argName)) {
                uassert(ErrorCodes::FailedToParse,
                        "Cannot specify two functions in window function spec",
                        !accumulatorName);
                accumulatorName = argName;
                input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Window function found an unknown argument: " << argName);
            }
        }

        uassert(ErrorCodes::FailedToParse,
                "Must specify a window function in output field",
                accumulatorName);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'window' field is not allowed in " << accumulatorName,
                windowFieldMissing);
        return make_intrusive<ExpressionFromLeftUnboundedWindowFunction<FunctionType>>(
            expCtx, accumulatorName->toString(), std::move(input), std::move(bounds));
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return FunctionType::create(_expCtx);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        tasserted(6050101,
                  str::stream() << "Window function " << _accumulatorName
                                << " is not supported as a removable window function");
    }

    Value serialize(SerializationOptions opts) const final {
        MutableDocument args;
        args.addField(_accumulatorName, Value(_input->serialize(opts)));
        return args.freezeToValue();
    }
};

template <typename NonRemovableType, typename RemovableType>
class ExpressionRemovable : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // 'obj' is something like '{$func: <args>, window: {...}}'
        boost::optional<StringData> accumulatorName;
        WindowBounds bounds = WindowBounds::defaultBounds();
        boost::intrusive_ptr<::mongo::Expression> input;
        for (const auto& arg : obj) {
            auto argName = arg.fieldNameStringData();
            if (argName == kWindowArg) {
                bounds = WindowBounds::parse(arg, sortBy, expCtx);
            } else if (isFunction(argName)) {
                uassert(ErrorCodes::FailedToParse,
                        "Cannot specify two functions in window function spec",
                        !accumulatorName);
                accumulatorName = argName;
                input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Window function found an unknown argument: " << argName);
            }
        }

        uassert(ErrorCodes::FailedToParse,
                "Must specify a window function in output field",
                accumulatorName);
        return make_intrusive<ExpressionRemovable<NonRemovableType, RemovableType>>(
            expCtx, accumulatorName->toString(), std::move(input), std::move(bounds));
    }

    ExpressionRemovable(ExpressionContext* expCtx,
                        std::string accumulatorName,
                        boost::intrusive_ptr<::mongo::Expression> input,
                        WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return NonRemovableType::create(_expCtx);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        return RemovableType::create(_expCtx);
    }
};

template <typename RankType>
class ExpressionFromRankAccumulator : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // 'obj' is something like '{$func: <args>}'
        uassert(5371601, "Rank style window functions take no other arguments", obj.nFields() == 1);
        boost::optional<StringData> accumulatorName;
        // Rank based accumulators are always unbounded to current.
        WindowBounds bounds = WindowBounds{
            WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Current{}}};
        auto arg = obj.firstElement();
        auto argName = arg.fieldNameStringData();
        if (isFunction(argName)) {
            uassert(5371603,
                    str::stream() << accumulatorName << " must be specified with '{}' as the value",
                    arg.type() == BSONType::Object && arg.embeddedObject().nFields() == 0);
            accumulatorName = argName;
        } else {
            tasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Window function found an unknown argument: " << argName);
        }

        // Rank based accumulators use the sort by expression as the input.
        uassert(5371602,
                str::stream() << accumulatorName
                              << " must be specified with a top level sortBy expression with "
                                 "exactly one element",
                sortBy && sortBy->isSingleElementKey());
        auto sortPatternPart = sortBy.get()[0];
        if (sortPatternPart.fieldPath) {
            auto sortExpression = ExpressionFieldPath::createPathFromString(
                expCtx, sortPatternPart.fieldPath->fullPath(), expCtx->variablesParseState);
            return make_intrusive<ExpressionFromRankAccumulator<RankType>>(
                expCtx, accumulatorName->toString(), std::move(sortExpression), std::move(bounds));
        } else {
            return make_intrusive<ExpressionFromRankAccumulator<RankType>>(
                expCtx, accumulatorName->toString(), sortPatternPart.expression, std::move(bounds));
        }
    }

    ExpressionFromRankAccumulator(ExpressionContext* expCtx,
                                  std::string accumulatorName,
                                  boost::intrusive_ptr<::mongo::Expression> input,
                                  WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return RankType::create(_expCtx);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        tasserted(5371600,
                  str::stream() << "Window function " << _accumulatorName
                                << " is not supported with a removable window");
    }

    Value serialize(SerializationOptions opts) const final {
        MutableDocument args;
        args.addField(_accumulatorName, Value(Document()));
        return args.freezeToValue();
    }
};

class ExpressionExpMovingAvg : public Expression {
public:
    static constexpr StringData kAccName = "$expMovingAvg"_sd;
    static constexpr StringData kInputArg = "input"_sd;
    static constexpr StringData kNArg = "N"_sd;
    static constexpr StringData kAlphaArg = "alpha"_sd;
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx);

    ExpressionExpMovingAvg(ExpressionContext* expCtx,
                           std::string accumulatorName,
                           boost::intrusive_ptr<::mongo::Expression> input,
                           WindowBounds bounds,
                           long long nValue)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)),
          _N(nValue) {}

    ExpressionExpMovingAvg(ExpressionContext* expCtx,
                           std::string accumulatorName,
                           boost::intrusive_ptr<::mongo::Expression> input,
                           WindowBounds bounds,
                           Decimal128 alpha)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)),
          _alpha(alpha) {}

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        if (_N) {
            return AccumulatorExpMovingAvg::create(
                _expCtx, Decimal128(2).divide(Decimal128(_N.get()).add(Decimal128(1))));
        } else if (_alpha) {
            return AccumulatorExpMovingAvg::create(_expCtx, _alpha.get());
        }
        tasserted(5433602, "ExpMovingAvg neither N nor alpha was set");
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        tasserted(5433603,
                  str::stream() << "Window function " << _accumulatorName
                                << " is not supported with a removable window");
    }

    Value serialize(SerializationOptions opts) const final {
        MutableDocument subObj;
        tassert(5433604, "ExpMovingAvg neither N nor alpha was set", _N || _alpha);
        if (_N) {
            subObj[kNArg] = opts.serializeLiteralValue(_N.get());
        } else {
            subObj[kAlphaArg] = opts.serializeLiteralValue(_alpha.get());
        }
        subObj[kInputArg] = _input->serialize(opts);
        MutableDocument outerObj;
        outerObj[kAccName] = subObj.freezeToValue();
        return outerObj.freezeToValue();
    }

protected:
    boost::optional<long long> _N;
    boost::optional<Decimal128> _alpha;
};

class ExpressionWithUnit : public Expression {
public:
    static constexpr StringData kArgInput = "input"_sd;
    static constexpr StringData kArgUnit = "unit"_sd;

    ExpressionWithUnit(ExpressionContext* expCtx,
                       std::string accumulatorName,
                       boost::intrusive_ptr<::mongo::Expression> input,
                       WindowBounds bounds,
                       boost::optional<TimeUnit> unit)
        : Expression(expCtx, accumulatorName, std::move(input), std::move(bounds)), _unit(unit) {}

    boost::optional<TimeUnit> unit() const {
        return _unit;
    }

    Value serialize(SerializationOptions opts) const final {
        MutableDocument result;
        result[_accumulatorName][kArgInput] = _input->serialize(opts);
        if (_unit) {
            result[_accumulatorName][kArgUnit] = Value(serializeTimeUnit(*_unit));
        }

        MutableDocument windowField;
        _bounds.serialize(windowField, opts);
        result[kWindowArg] = windowField.freezeToValue();
        return result.freezeToValue();
    }

protected:
    static boost::optional<TimeUnit> parseUnit(const BSONElement& arg) {
        boost::optional<TimeUnit> unit;
        {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << kArgUnit << "' must be a string, but got " << arg.type(),
                    arg.type() == String);
            unit = parseTimeUnit(arg.valueStringData());
            switch (*unit) {
                // These larger time units vary so much, it doesn't make sense to define a
                // fixed conversion from milliseconds. (See 'timeUnitTypicalMilliseconds'.)
                case TimeUnit::year:
                case TimeUnit::quarter:
                case TimeUnit::month:
                    uasserted(5490710, "unit must be 'week' or smaller");
                // Only these time units are allowed.
                case TimeUnit::week:
                case TimeUnit::day:
                case TimeUnit::hour:
                case TimeUnit::minute:
                case TimeUnit::second:
                case TimeUnit::millisecond:
                    break;
            }
        }
        return unit;
    }

    static void validateSortBy(const boost::optional<SortPattern>& sortBy,
                               const std::string& accumulatorName) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << accumulatorName << " requires a sortBy",
                sortBy);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << accumulatorName << " requires a non-compound sortBy",
                sortBy->size() == 1);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << accumulatorName << " requires a non-expression sortBy",
                !sortBy->begin()->expression);
    }

    boost::optional<long long> convertTimeUnitToMillis(boost::optional<TimeUnit> unit) const {
        if (!unit)
            return boost::none;

        auto status = timeUnitTypicalMilliseconds(*unit);
        tassert(status);

        return status.getValue();
    }

    boost::optional<TimeUnit> _unit;
};

class ExpressionDerivative : public ExpressionWithUnit {
public:
    ExpressionDerivative(ExpressionContext* expCtx,
                         boost::intrusive_ptr<::mongo::Expression> input,
                         WindowBounds bounds,
                         boost::optional<TimeUnit> unit)
        : ExpressionWithUnit(expCtx, "$derivative", std::move(input), std::move(bounds), unit) {}

    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // {
        //   $derivative: {
        //     input: <expr>,
        //     unit: <string>, // optional
        //   }
        //   window: {...} // optional
        // }
        validateSortBy(sortBy, "$derivative");

        boost::optional<WindowBounds> bounds;
        BSONElement derivativeArgs;
        for (const auto& arg : obj) {
            auto argName = arg.fieldNameStringData();
            if (argName == kWindowArg) {
                bounds = WindowBounds::parse(arg, sortBy, expCtx);
            } else if (argName == "$derivative"_sd) {
                derivativeArgs = arg;
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "$derivative got unexpected argument: " << argName);
            }
        }
        tassert(5490700,
                "$derivative parser called on object with no $derivative key",
                derivativeArgs.ok());
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "$derivative expects an object, but got a "
                              << derivativeArgs.type() << ": " << derivativeArgs,
                derivativeArgs.type() == BSONType::Object);

        boost::intrusive_ptr<::mongo::Expression> input;
        boost::optional<TimeUnit> unit;
        for (const auto& arg : derivativeArgs.Obj()) {
            auto argName = arg.fieldNameStringData();
            if (argName == kArgInput) {
                input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            } else if (argName == kArgUnit) {
                unit = parseUnit(arg);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "$derivative got unexpected argument: " << argName);
            }
        }
        uassert(ErrorCodes::FailedToParse, "$derivative requires an 'input' expression", input);

        // The default window bounds are [unbounded, unbounded], which may be a surprising default
        // for $derivative.
        uassert(ErrorCodes::FailedToParse,
                "$derivative requires explicit window bounds",
                bounds != boost::none);

        return make_intrusive<ExpressionDerivative>(
            expCtx, std::move(input), std::move(*bounds), unit);
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        MONGO_UNREACHABLE_TASSERT(5490701);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        MONGO_UNREACHABLE_TASSERT(5490702);
    }
};

class ExpressionIntegral : public ExpressionWithUnit {
public:
    ExpressionIntegral(ExpressionContext* expCtx,
                       boost::intrusive_ptr<::mongo::Expression> input,
                       WindowBounds bounds,
                       boost::optional<TimeUnit> unit)
        : ExpressionWithUnit(expCtx, "$integral", std::move(input), std::move(bounds), unit) {}

    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        // {
        //   $integral: {
        //     input: <expr>,
        //     unit: <string>, // optional
        //   }
        //   window: {...} // optional
        // }
        //
        validateSortBy(sortBy, "$integral");

        boost::optional<WindowBounds> bounds = boost::none;
        BSONElement integralArgs;
        for (const auto& arg : obj) {
            auto argName = arg.fieldNameStringData();
            if (argName == kWindowArg) {
                uassert(ErrorCodes::FailedToParse,
                        "There can be only one 'window' field for $integral",
                        bounds == boost::none);
                bounds = WindowBounds::parse(arg, sortBy, expCtx);
            } else if (argName == "$integral"_sd) {
                integralArgs = arg;
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "$integral got unexpected argument: " << argName);
            }
        }
        tassert(
            5558801, "$integral parser called on object with no $integral key", integralArgs.ok());
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "$integral expects an object, but got a " << integralArgs.type()
                              << ": " << integralArgs,
                integralArgs.type() == BSONType::Object);

        boost::intrusive_ptr<::mongo::Expression> input;
        boost::optional<TimeUnit> unit = boost::none;
        for (const auto& arg : integralArgs.Obj()) {
            auto argName = arg.fieldNameStringData();
            if (argName == kArgInput) {
                input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            } else if (argName == kArgUnit) {
                uassert(ErrorCodes::FailedToParse,
                        "There can be only one 'unit' field for $integral",
                        unit == boost::none);
                unit = parseUnit(arg);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "$integral got unexpected argument: " << argName);
            }
        }
        uassert(ErrorCodes::FailedToParse, "$integral requires an 'input' expression", input);

        return make_intrusive<ExpressionIntegral>(
            expCtx, std::move(input), bounds ? *bounds : WindowBounds(), unit);
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        return AccumulatorIntegral::create(_expCtx, convertTimeUnitToMillis(_unit));
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        return WindowFunctionIntegral::create(_expCtx, convertTimeUnitToMillis(_unit));
    }
};

class ExpressionLinearFill : public Expression {
public:
    ExpressionLinearFill(ExpressionContext* expCtx,
                         std::string accumulatorName,
                         boost::intrusive_ptr<::mongo::Expression> input,
                         WindowBounds bounds)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)) {}
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        boost::optional<StringData> accumulatorName;
        WindowBounds bounds = WindowBounds::defaultBounds();
        boost::intrusive_ptr<::mongo::Expression> input;

        std::vector<std::pair<std::string, boost::intrusive_ptr<mongo::Expression>>> linearFillVec;
        bool windowFieldMissing = true;
        for (const auto& arg : obj) {
            auto argName = arg.fieldNameStringData();
            if (argName == kWindowArg) {
                windowFieldMissing = false;
            } else if (isFunction(argName)) {
                uassert(ErrorCodes::FailedToParse,
                        "Cannot specify two functions in window function spec",
                        !accumulatorName);
                accumulatorName = argName;
                input = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Window function found an unknown argument: " << argName);
            }
        }

        uassert(ErrorCodes::FailedToParse,
                "Must specify a window function in output field",
                accumulatorName);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'window' field is not allowed in " << accumulatorName,
                windowFieldMissing);

        uassert(605001,
                str::stream() << accumulatorName
                              << " must be specified with a top level sortBy expression with "
                                 "exactly one element",
                sortBy && sortBy->isSingleElementKey());

        return make_intrusive<ExpressionLinearFill>(
            expCtx, accumulatorName->toString(), std::move(input), std::move(bounds));
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        MONGO_UNREACHABLE_TASSERT(5490704);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        MONGO_UNREACHABLE_TASSERT(5490705);
    }

    Value serialize(SerializationOptions opts) const final {
        MutableDocument args;
        args.addField(_accumulatorName, Value(_input->serialize(opts)));
        return args.freezeToValue();
    }
};

class ExpressionFirstLast : public Expression {
public:
    using Sense = AccumulatorFirstLastN::Sense;

    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx,
                                                  Sense sense);
    static std::string senseToAccumulatorName(Sense sense) {
        switch (sense) {
            case Sense::kFirst:
                return "$first";
            case Sense::kLast:
                return "$last";
            default:
                return "unrecognized sense";
        }
    }
};

class ExpressionFirst : public Expression {
public:
    ExpressionFirst(ExpressionContext* expCtx,
                    boost::intrusive_ptr<::mongo::Expression> input,
                    WindowBounds bounds)
        : Expression(expCtx, "$first", std::move(input), std::move(bounds)) {}

    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        return ExpressionFirstLast::parse(obj, sortBy, expCtx, ExpressionFirstLast::Sense::kFirst);
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        MONGO_UNREACHABLE_TASSERT(5490706);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        MONGO_UNREACHABLE_TASSERT(5490707);
    }
};

class ExpressionLast : public Expression {
public:
    ExpressionLast(ExpressionContext* expCtx,
                   boost::intrusive_ptr<::mongo::Expression> input,
                   WindowBounds bounds)
        : Expression(expCtx, "$last", std::move(input), std::move(bounds)) {}

    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx) {
        return ExpressionFirstLast::parse(obj, sortBy, expCtx, ExpressionFirstLast::Sense::kLast);
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        MONGO_UNREACHABLE_TASSERT(5490708);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        MONGO_UNREACHABLE_TASSERT(5490709);
    }
};

/**
 * Describes a window function expression that accepts 'n' as a parameter. Templated by
 * 'WindowFunctionN', which corresponds to the 'WindowFunctionState' removable type associated
 * with this expression. It is also templated by AccumulatorType which is the AccumulatorState
 * associated with this expression.
 */
template <typename WindowFunctionN, typename AcumulatorNType>
class ExpressionN : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx);

    ExpressionN(ExpressionContext* expCtx,
                boost::intrusive_ptr<::mongo::Expression> input,
                std::string name,
                WindowBounds bounds,
                boost::intrusive_ptr<::mongo::Expression> nExpr,
                boost::optional<SortPattern> sortPattern)
        : Expression(expCtx, std::move(name), std::move(input), std::move(bounds)),
          nExpr(std::move(nExpr)),
          sortPattern(std::move(sortPattern)) {}

    Value serialize(SerializationOptions opts) const final;

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final;

    std::unique_ptr<WindowFunctionState> buildRemovable() const final;

    void addDependencies(DepsTracker* deps) const final {
        if (_input) {
            expression::addDependencies(_input.get(), deps);
        }

        if (nExpr) {
            expression::addDependencies(nExpr.get(), deps);
        }
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        if (_input) {
            expression::addVariableRefs(_input.get(), refs);
        }

        if (nExpr) {
            expression::addVariableRefs(nExpr.get(), refs);
        }
    }

    // TODO SERVER-59327 make these members private
    boost::intrusive_ptr<::mongo::Expression> nExpr;
    boost::optional<SortPattern> sortPattern;
};

template <typename AccumulatorTType>
class ExpressionQuantile : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx);

    ExpressionQuantile(ExpressionContext* expCtx,
                       std::string accumulatorName,
                       boost::intrusive_ptr<::mongo::Expression> input,
                       boost::intrusive_ptr<::mongo::Expression> initializeExpr,
                       WindowBounds bounds,
                       std::vector<double> ps,
                       int32_t method)
        : Expression(expCtx, std::move(accumulatorName), std::move(input), std::move(bounds)),
          _ps(std::move(ps)),
          _method(method),
          _intializeExpr(std::move(initializeExpr)) {}

    Value serialize(SerializationOptions opts) const final;

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final;

    std::unique_ptr<WindowFunctionState> buildRemovable() const final;

private:
    std::vector<double> _ps;
    int32_t _method;
    boost::intrusive_ptr<::mongo::Expression> _intializeExpr;
};

}  // namespace mongo::window_function
