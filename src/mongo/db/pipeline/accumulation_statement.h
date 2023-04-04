/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

/**
 * Registers an AccumulatorState to have the name 'key'. When an accumulator with name '$key' is
 * found during parsing, 'factory' will be called to construct the AccumulatorState.
 *
 * As an example, if your accumulator looks like {"$foo": <args>}, with a factory method 'create',
 * you would add this line:
 * REGISTER_ACCUMULATOR(foo, AccumulatorFoo::create);
 */
#define REGISTER_ACCUMULATOR(key, factory)                            \
    REGISTER_ACCUMULATOR_CONDITIONALLY(key,                           \
                                       factory,                       \
                                       AllowedWithApiStrict::kAlways, \
                                       AllowedWithClientType::kAny,   \
                                       boost::none,                   \
                                       true)

/**
 * Like REGISTER_ACCUMULATOR, except the accumulator will only be registered when featureFlag is
 * enabled. We store featureFlag in the parseMap, so that it can be checked at runtime
 * to correctly enable/disable the accumulator.
 */
#define REGISTER_ACCUMULATOR_WITH_FEATURE_FLAG(key, factory, featureFlag) \
    REGISTER_ACCUMULATOR_CONDITIONALLY(key,                               \
                                       factory,                           \
                                       AllowedWithApiStrict::kAlways,     \
                                       AllowedWithClientType::kAny,       \
                                       featureFlag,                       \
                                       featureFlag.isEnabledAndIgnoreFCVUnsafeAtStartup())

/**
 * You can specify a condition, evaluated during startup,
 * that decides whether to register the parser.
 *
 * For example, you could check a feature flag, and register the parser only when it's enabled.
 *
 * Note that the condition is evaluated only once, during a MONGO_INITIALIZER. Don't specify
 * a condition that can change at runtime, such as FCV. (Feature flags are ok, because they
 * cannot be toggled at runtime.)
 *
 * This is the most general REGISTER_ACCUMULATOR* macro, which all others should delegate to.
 */
#define REGISTER_ACCUMULATOR_CONDITIONALLY(                                                   \
    key, factory, allowedWithApiStrict, allowedClientType, featureFlag, ...)                  \
    MONGO_INITIALIZER_GENERAL(addToAccumulatorFactoryMap_##key,                               \
                              ("BeginAccumulatorRegistration"),                               \
                              ("EndAccumulatorRegistration"))                                 \
    (InitializerContext*) {                                                                   \
        if (!(__VA_ARGS__)) {                                                                 \
            return;                                                                           \
        }                                                                                     \
        AccumulationStatement::registerAccumulator(                                           \
            "$" #key, (factory), (allowedWithApiStrict), (allowedClientType), (featureFlag)); \
    }

/**
 * AccumulatorExpression represents the right-hand side of an AccumulationStatement. Note this is
 * different from Expression; they are different nonterminals in the grammar.
 *
 * For example, in
 *     {$group: {
 *         _id: 1,
 *         count: {$sum: {$size: "$tags"}}
 *     }}
 *
 * we would say:
 *     The AccumulationStatement is      count: {$sum: {$size: "$tags"}}
 *     The AccumulationExpression is     {$sum: {$size: "$tags"}}
 *     The AccumulatorState::Factory is  $sum
 *     The argument Expression is        {$size: "$tags"}
 *     There is no initializer Expression.
 *
 * "$sum" corresponds to an AccumulatorState::Factory rather than AccumulatorState because
 * AccumulatorState is an execution concept, not an AST concept: each instance of AccumulatorState
 * contains intermediate values being accumulated.
 *
 * Like most accumulators, $sum does not require or accept an initializer Expression. At time of
 * writing, only user-defined accumulators and the 'N' family of accumulators accept an initializer.
 *
 * For example, in:
 *     {$group: {
 *         _id: {cc: "$country_code"},
 *         top_stories: {$accumulator: {
 *             init: function(cc) { ... },
 *             initArgs: ["$cc"],
 *             accumulate: function(state, title, upvotes) { ... },
 *             accumulateArgs: ["$title", "$upvotes"],
 *             merge: function(state1, state2) { ... },
 *             lang: "js",
 *         }}
 *     }}
 *
 * we would say:
 *     The AccumulationStatement is      top_stories: {$accumulator: ... }
 *     The AccumulationExpression is     {$accumulator: ... }
 *     The argument Expression is        ["$cc"]
 *     The initializer Expression is     ["$title", "$upvotes"]
 *     The AccumulatorState::Factory holds all the other arguments to $accumulator.
 *
 */
struct AccumulationExpression {
    AccumulationExpression(boost::intrusive_ptr<Expression> initializer,
                           boost::intrusive_ptr<Expression> argument,
                           AccumulatorState::Factory factory,
                           StringData name)
        : initializer(initializer), argument(argument), factory(factory), name(name) {
        invariant(this->initializer);
        invariant(this->argument);
    }

    // The expression to use to obtain the input to the accumulator.
    boost::intrusive_ptr<Expression> initializer;

    // An expression evaluated once per input document, and passed to AccumulatorState::process.
    boost::intrusive_ptr<Expression> argument;

    // A no argument function object that can be called to create an AccumulatorState.
    const AccumulatorState::Factory factory;

    // The name of the accumulator expression. It is the caller's responsibility to make sure the
    // memory this points to does not get freed. This can best be accomplished by passing in a
    // pointer to a string constant. This StringData is always required to point to a valid
    // null-terminated string.
    const StringData name;
};

/**
 * A default parser for any accumulator that only takes a single expression as an argument. Returns
 * the expression to be evaluated by the accumulator and an AccumulatorState::Factory.
 */
template <class AccName>
AccumulationExpression genericParseSingleExpressionAccumulator(ExpressionContext* const expCtx,
                                                               BSONElement elem,
                                                               VariablesParseState vps) {
    auto initializer = ExpressionConstant::create(expCtx, Value(BSONNULL));
    auto argument = Expression::parseOperand(expCtx, elem, vps);
    return {initializer, argument, [expCtx]() { return AccName::create(expCtx); }, AccName::kName};
}

/**
 * A parser for any SBE unsupported accumulator that only takes a single expression as an argument.
 * Returns the expression to be evaluated by the accumulator and an AccumulatorState::Factory.
 */
template <class AccName>
AccumulationExpression genericParseSBEUnsupportedSingleExpressionAccumulator(
    ExpressionContext* const expCtx, BSONElement elem, VariablesParseState vps) {
    expCtx->sbeGroupCompatibility = SbeCompatibility::notCompatible;
    return genericParseSingleExpressionAccumulator<AccName>(expCtx, elem, vps);
}

/**
 * A parser that desugars { $count: {} } to { $sum: 1 }.
 */
inline AccumulationExpression parseCountAccumulator(ExpressionContext* const expCtx,
                                                    BSONElement elem,
                                                    VariablesParseState vps) {
    uassert(ErrorCodes::TypeMismatch,
            "$count takes no arguments, i.e. $count:{}",
            elem.type() == BSONType::Object && elem.Obj().isEmpty());
    auto initializer = ExpressionConstant::create(expCtx, Value(BSONNULL));
    auto argument = ExpressionConstant::create(expCtx, Value(1));
    return {initializer,
            argument,
            [expCtx]() { return AccumulatorSum::create(expCtx); },
            AccumulatorSum::kName};
}

/**
 * A class representing a user-specified accumulation, including the field name to put the
 * accumulated result in, which accumulator to use, and the expression used to obtain the input to
 * the AccumulatorState.
 */
class AccumulationStatement {
public:
    using Parser = std::function<AccumulationExpression(
        ExpressionContext* const, BSONElement, VariablesParseState)>;

    /**
     * Associates a Parser with information regarding which contexts it can be used in, including
     * API Version and feature flag.
     */
    using ParserRegistration = std::
        tuple<Parser, AllowedWithApiStrict, AllowedWithClientType, boost::optional<FeatureFlag>>;

    AccumulationStatement(std::string fieldName, AccumulationExpression expr)
        : fieldName(std::move(fieldName)), expr(std::move(expr)) {}

    /**
     * Parses a BSONElement that is an accumulated field, and returns an AccumulationStatement for
     * that accumulated field.
     *
     * Throws an AssertionException if parsing fails, if the configured API version is not
     * compatible with this AccumulationStatement, or if the Parser is registered under an FCV
     * greater than the specified maximum allowed FCV.
     */
    static AccumulationStatement parseAccumulationStatement(ExpressionContext* expCtx,
                                                            const BSONElement& elem,
                                                            const VariablesParseState& vps);

    /**
     * Registers an AccumulatorState with a parsing function, so that when an accumulator with the
     * given name is encountered during parsing, we will know to call 'factory' to construct that
     * AccumulatorState.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_ACCUMULATOR macro defined in this
     * file.
     */
    static void registerAccumulator(std::string name,
                                    Parser parser,
                                    AllowedWithApiStrict allowedWithApiStrict,
                                    AllowedWithClientType allowedWithClientType,
                                    boost::optional<FeatureFlag> featureFlag);

    /**
     * Retrieves the Parser for the accumulator specified by the given name, and raises an error if
     * there is no such Parser registered.
     */
    static ParserRegistration& getParser(StringData name);

    // The field name is used to store the results of the accumulation in a result document.
    std::string fieldName;

    AccumulationExpression expr;

    // Constructs an AccumulatorState to do actual accumulation.
    boost::intrusive_ptr<AccumulatorState> makeAccumulator() const;
};


}  // namespace mongo
