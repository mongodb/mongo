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

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> makeUnaryOp(sbe::EPrimUnary::Op unaryOp,
                                              std::unique_ptr<sbe::EExpression> operand);

/**
 * Wrap expression into logical negation.
 */
std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e);

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs,
                                               std::unique_ptr<sbe::EExpression> collator = {});

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs,
                                               sbe::RuntimeEnvironment* env);

std::unique_ptr<sbe::EExpression> makeIsMember(std::unique_ptr<sbe::EExpression> input,
                                               std::unique_ptr<sbe::EExpression> arr,
                                               std::unique_ptr<sbe::EExpression> collator = {});

std::unique_ptr<sbe::EExpression> makeIsMember(std::unique_ptr<sbe::EExpression> input,
                                               std::unique_ptr<sbe::EExpression> arr,
                                               sbe::RuntimeEnvironment* env);

/**
 * Generates an EExpression that checks if the input expression is null or missing.
 */
std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var);

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::FrameId frameId,
                                                        const sbe::value::SlotId slotId);

/**
 * Generates an EExpression that checks if the input expression is a non-numeric type _assuming
 * that_ it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is the value NumberLong(-2^64).
 */
std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a non-positive number (i.e. <= 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a negative (i.e., < 0) number
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is _not_ an object, _assuming that_
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is not a string, _assuming that
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks whether the input expression is null, missing, or
 * unable to be converted to the type NumberInt32.
 */
std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var);

/**
 * A pair representing a 1) true/false condition and 2) the value that should be returned if that
 * condition evaluates to true.
 */
using CaseValuePair =
    std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>;

/**
 * Convert a list of CaseValuePairs into a chain of EIf expressions, with the final else case
 * evaluating to the 'defaultValue' EExpression.
 */
template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(Ts... cases);

template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(CaseValuePair headCase, Ts... rest) {
    return sbe::makeE<sbe::EIf>(std::move(headCase.first),
                                std::move(headCase.second),
                                buildMultiBranchConditional(std::move(rest)...));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase);

/**
 * Converts a std::vector of CaseValuePairs into a chain of EIf expressions in the same manner as
 * the 'buildMultiBranchConditional()' function.
 */
std::unique_ptr<sbe::EExpression> buildMultiBranchConditionalFromCaseValuePairs(
    std::vector<CaseValuePair> caseValuePairs, std::unique_ptr<sbe::EExpression> defaultValue);

/**
 * Insert a limit stage on top of the 'input' stage.
 */
std::unique_ptr<sbe::PlanStage> makeLimitTree(std::unique_ptr<sbe::PlanStage> inputStage,
                                              PlanNodeId planNodeId,
                                              long long limit = 1);

/**
 * Create tree consisting of coscan stage followed by limit stage.
 */
std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit = 1);

/**
 * Same as 'makeLimitCoScanTree()', but returns 'EvalStage' with empty 'outSlots' vector.
 */
EvalStage makeLimitCoScanStage(PlanNodeId planNodeId, long long limit = 1);

/**
 * If 'stage.stage' is 'nullptr', return limit-1/coscan tree. Otherwise, return stage.
 */
EvalStage stageOrLimitCoScan(EvalStage stage, PlanNodeId planNodeId, long long limit = 1);

/**
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e);

/**
 * Creates an EFunction expression with the given name and arguments.
 */
template <typename... Args>
inline std::unique_ptr<sbe::EExpression> makeFunction(std::string_view name, Args&&... args) {
    return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
}

template <typename T>
inline auto makeConstant(sbe::value::TypeTags tag, T value) {
    return sbe::makeE<sbe::EConstant>(tag, sbe::value::bitcastFrom<T>(value));
}

inline auto makeConstant(std::string_view str) {
    auto [tag, value] = sbe::value::makeNewString(str);
    return sbe::makeE<sbe::EConstant>(tag, value);
}

/**
 * Check if expression returns Nothing and return null if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e);

/**
 * Check if expression returns an array and return Nothing if so. Otherwise, return the expression.
 */
std::unique_ptr<sbe::EExpression> makeNothingArrayCheck(
    std::unique_ptr<sbe::EExpression> isArrayInput, std::unique_ptr<sbe::EExpression> otherwise);

/**
 * Creates an expression to extract a shard key part from inputExpr. The generated expression is a
 * let binding that binds a getField expression to extract the shard key part value from the
 * inputExpr. The entire let binding evaluates to a constant expression carrying the Nothing value
 * if the binding is an array. Otherwise, it evaluates to a fillEmpty null expression. Here is an
 * example expression generated from this function for a shard key pattern {'a.b': 1}:
 *
 * let [l1.0 = getField (s1, "a")]
 *   if (isArray (l1.0), NOTHING,
 *     let [l2.0 = getField (l1.0, "b")]
 *       if (isArray (l2.0), NOTHING, fillEmpty (l2.0, null)))
 */
std::unique_ptr<sbe::EExpression> generateShardKeyBinding(
    const FieldRef& keyPatternField,
    sbe::value::FrameIdGenerator& frameIdGenerator,
    std::unique_ptr<sbe::EExpression> inputExpr,
    int level);

/**
 * If given 'EvalExpr' already contains a slot, simply returns it. Otherwise, allocates a new slot
 * and creates project stage to assign expression to this new slot. After that, new slot and project
 * stage are returned.
 */
std::pair<sbe::value::SlotId, EvalStage> projectEvalExpr(
    EvalExpr expr,
    EvalStage stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator);

template <bool IsConst, bool IsEof = false>
EvalStage makeFilter(EvalStage stage,
                     std::unique_ptr<sbe::EExpression> filter,
                     PlanNodeId planNodeId) {
    stage = stageOrLimitCoScan(std::move(stage), planNodeId);

    return {sbe::makeS<sbe::FilterStage<IsConst, IsEof>>(
                std::move(stage.stage), std::move(filter), planNodeId),
            std::move(stage.outSlots)};
}

template <typename... Ts>
EvalStage makeProject(EvalStage stage, PlanNodeId planNodeId, Ts&&... pack) {
    stage = stageOrLimitCoScan(std::move(stage), planNodeId);

    auto outSlots = std::move(stage.outSlots);
    auto projects = makeEM(std::forward<Ts>(pack)...);

    for (auto& [slot, expr] : projects) {
        outSlots.push_back(slot);
    }

    return {sbe::makeS<sbe::ProjectStage>(std::move(stage.stage), std::move(projects), planNodeId),
            std::move(outSlots)};
}

/**
 * Creates loop join stage. All 'outSlots' from the 'left' argument along with slots from the
 * 'lexicalEnvironment' argument are passed as correlated.
 * If stage in 'left' or 'right' argument is 'nullptr', it is treated as if it was limit-1/coscan.
 * In this case, loop join stage is not created. 'right' stage is returned if 'left' is 'nullptr'.
 * 'left' stage is returned if 'right' is 'nullptr'.
 */
EvalStage makeLoopJoin(EvalStage left,
                       EvalStage right,
                       PlanNodeId planNodeId,
                       const sbe::value::SlotVector& lexicalEnvironment = {});

/**
 * Creates an unwind stage and an output slot for it using the first slot in the outSlots vector of
 * the inputEvalStage as the input slot to the new stage. The preserveNullAndEmptyArrays is passed
 * to the UnwindStage constructor to specify the treatment of null or missing inputs.
 */
EvalStage makeUnwind(EvalStage inputEvalStage,
                     sbe::value::SlotIdGenerator* slotIdGenerator,
                     PlanNodeId planNodeId,
                     bool preserveNullAndEmptyArrays = true);

/**
 * Creates a branch stage with the specified condition ifExpr and creates output slots for the
 * branch stage. This forwards the outputs of the thenStage to the output slots of the branchStage
 * if the condition evaluates to true, and forwards the elseStage outputs if the condition is false.
 */
EvalStage makeBranch(std::unique_ptr<sbe::EExpression> ifExpr,
                     EvalStage thenStage,
                     EvalStage elseStage,
                     sbe::value::SlotIdGenerator* slotIdGenerator,
                     PlanNodeId planNodeId);

/**
 * Creates traverse stage. All 'outSlots' from 'outer' argument (except for 'inField') along with
 * slots from the 'lexicalEnvironment' argument are passed as correlated.
 */
EvalStage makeTraverse(EvalStage outer,
                       EvalStage inner,
                       sbe::value::SlotId inField,
                       sbe::value::SlotId outField,
                       sbe::value::SlotId outFieldInner,
                       std::unique_ptr<sbe::EExpression> foldExpr,
                       std::unique_ptr<sbe::EExpression> finalExpr,
                       PlanNodeId planNodeId,
                       boost::optional<size_t> nestedArraysDepth,
                       const sbe::value::SlotVector& lexicalEnvironment = {});

using BranchFn = std::function<std::pair<sbe::value::SlotId, EvalStage>(
    EvalExpr expr,
    EvalStage stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator)>;

/**
 * Creates a union stage with specified branches. Each branch is passed to 'branchFn' first. If
 * 'branchFn' is not set, expression from branch is simply projected to a slot.
 */
EvalExprStagePair generateUnion(std::vector<EvalExprStagePair> branches,
                                BranchFn branchFn,
                                PlanNodeId planNodeId,
                                sbe::value::SlotIdGenerator* slotIdGenerator);
/**
 * Creates limit-1/union stage with specified branches. Each branch is passed to 'branchFn' first.
 * If 'branchFn' is not set, expression from branch is simply projected to a slot.
 */
EvalExprStagePair generateSingleResultUnion(std::vector<EvalExprStagePair> branches,
                                            BranchFn branchFn,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotIdGenerator* slotIdGenerator);

/** This helper takes an SBE SlotIdGenerator and an SBE Array and returns an output slot and a
 * unwind/project/limit/coscan subtree that streams out the elements of the array one at a time via
 * the output slot over a series of calls to getNext(), mimicking the output of a collection scan or
 * an index scan. Note that this method assumes ownership of the SBE Array being passed in.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal);

/**
 * Make a mock scan with multiple output slots from an BSON array. This method does NOT assume
 * ownership of the BSONArray passed in.
 */
std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal);

/**
 * Converts a BSONArray to an SBE Array. Caller owns the SBE Array returned. This method does not
 * assume ownership of the BSONArray.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba);

/**
 * Returns a BSON type mask of all data types coercible to date.
 */
uint32_t dateTypeMask();

/**
 * Constructs local binding with inner expression built by 'innerExprFunc' and variables assigned
 * to expressions from 'bindings'.
 * Example usage:
 *
 * makeLocalBind(
 *   _context->frameIdGenerator,
 *   [](sbe::EVariable inputArray, sbe::EVariable index) {
 *     return <expression using inputArray and index>;
 *   },
 *   <expression to assign to inputArray>,
 *   <expression to assign to index>
 * );
 */
template <typename... Bindings,
          typename InnerExprFunc,
          typename = std::enable_if_t<
              std::conjunction_v<std::is_same<std::unique_ptr<sbe::EExpression>, Bindings>...>>>
std::unique_ptr<sbe::EExpression> makeLocalBind(sbe::value::FrameIdGenerator* frameIdGenerator,
                                                InnerExprFunc innerExprFunc,
                                                Bindings... bindings) {
    auto frameId = frameIdGenerator->generate();
    auto binds = sbe::makeEs();
    binds.reserve(sizeof...(Bindings));
    sbe::value::SlotId lastIndex = 0;
    auto convertToVariable = [&](std::unique_ptr<sbe::EExpression> expr) {
        binds.emplace_back(std::move(expr));
        auto currentIndex = lastIndex;
        lastIndex++;
        return sbe::EVariable{frameId, currentIndex};
    };
    auto innerExpr = innerExprFunc(convertToVariable(std::move(bindings))...);
    return sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(innerExpr));
}

/**
 * Trees generated by 'generateFilter' maintain state during execution. There are two types of state
 * that can be maintained:
 *  - Boolean. The state is just boolean value, indicating if the document matches the predicate.
 *  - Index. The state stores a tuple of (boolean, optional int32).
 *
 * Depending on the query type, one of state types can be selected to use in the tree.
 * 'FilterStateHelper' class and it's descendants aim to provide unified interface to operate with
 * this two types of states.
 */
class FilterStateHelper {
public:
    using Expression = std::unique_ptr<sbe::EExpression>;

    virtual ~FilterStateHelper() = default;

    /**
     * Returns true if state contains a value along with boolean and false otherwise.
     */
    virtual bool stateContainsValue() const = 0;

    /**
     * Creates a constant holding state with given boolean 'value'. Index part of the constructed
     * state is empty.
     */
    virtual Expression makeState(bool value) const = 0;

    /**
     * Creates an expression that constructs state from 'expr'. 'expr' must evaluate to a boolean
     * value. Index part of the constructed state is empty.
     */
    virtual Expression makeState(Expression expr) const = 0;

    /**
     * Creates an expression that constructs an initial state from 'expr'. 'expr' must evaluate to a
     * boolean value.
     * Initial state is used as an output value for the inner branch passed to
     * 'makeTraverseCombinator'.
     */
    virtual Expression makeInitialState(Expression expr) const = 0;

    /**
     * Creates an expression that extracts boolean value from the state evaluated from 'expr'.
     */
    virtual Expression getBool(Expression expr) const = 0;

    Expression getBool(sbe::value::SlotId slotId) const {
        return getBool(sbe::makeE<sbe::EVariable>(slotId));
    }

    /**
     * Implements Elvis operator. If state from 'left' expression represents true boolean value,
     * returns 'left'. Otherwise, returns 'right'.
     */
    virtual Expression mergeStates(Expression left,
                                   Expression right,
                                   sbe::value::FrameIdGenerator* frameIdGenerator) const = 0;

    /**
     * Extracts index value from the state and projects it into a separate slot. If state does not
     * contain index value, slot contains Nothing.
     * If state does not support storing index value, this function does nothing.
     */
    virtual std::pair<boost::optional<sbe::value::SlotId>, EvalStage> projectValueCombinator(
        sbe::value::SlotId stateSlot,
        EvalStage stage,
        PlanNodeId planNodeId,
        sbe::value::SlotIdGenerator* slotIdGenerator,
        sbe::value::FrameIdGenerator* frameIdGenerator) const = 0;

    /**
     * Uses an expression from 'EvalExprStagePair' to construct state. Expresion must evaluate to
     * boolean value.
     */
    virtual EvalExprStagePair makePredicateCombinator(EvalExprStagePair pair) const = 0;

    /**
     * Creates traverse stage with fold and final expressions tuned to maintain consistent state.
     * If state does support index value, records the index of a first array element for which inner
     * branch returns true value.
     */
    virtual EvalStage makeTraverseCombinator(
        EvalStage outer,
        EvalStage inner,
        sbe::value::SlotId inputSlot,
        sbe::value::SlotId outputSlot,
        sbe::value::SlotId innerOutputSlot,
        PlanNodeId planNodeId,
        sbe::value::FrameIdGenerator* frameIdGenerator) const = 0;
};

/**
 * This class implements 'FilterStateHelper' interface for a state which can be represented as a
 * tuple of (boolean, optional int32). Such tuple is encoded as a single int64 value.
 *
 * While we could represent such tuple as an SBE array, this approach would cost us additional
 * allocations and the need to call expensive builtins such as 'getElement'. Integer operations are
 * much simpler, faster and do not require any allocations.
 *
 * The following encoding is implemented:
 *  - [False, Nothing] -> -1
 *  - [True, Nothing]  -> 0
 *  - [False, value]   -> - value - 2
 *  - [True, value]    -> value + 1
 *
 * Such encoding allows us to easily extract boolean value (just compare resulting int64 with 0) and
 * requires only a few arithmetical operations to extract the index value. Furthemore, we can
 * increment/decrement index value simply by incrementing/decrementing the decoded value.
 */
class IndexStateHelper : public FilterStateHelper {
public:
    static constexpr sbe::value::TypeTags ValueType = sbe::value::TypeTags::NumberInt64;

    bool stateContainsValue() const override {
        return true;
    }

    Expression makeState(bool value) const override {
        return makeConstant(ValueType, value ? 0 : -1);
    }

    Expression makeState(Expression expr) const override {
        return sbe::makeE<sbe::EIf>(std::move(expr), makeState(true), makeState(false));
    }

    Expression makeInitialState(Expression expr) const override {
        return sbe::makeE<sbe::EIf>(
            std::move(expr), makeConstant(ValueType, 1), makeConstant(ValueType, -2));
    }

    Expression getBool(Expression expr) const override {
        return sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::greaterEq, std::move(expr), makeConstant(ValueType, 0));
    }

    Expression mergeStates(Expression left,
                           Expression right,
                           sbe::value::FrameIdGenerator* frameIdGenerator) const override {
        return makeLocalBind(frameIdGenerator,
                             [&](sbe::EVariable left) {
                                 return sbe::makeE<sbe::EIf>(
                                     getBool(left.clone()), left.clone(), std::move(right));
                             },
                             std::move(left));
    }

    std::pair<boost::optional<sbe::value::SlotId>, EvalStage> projectValueCombinator(
        sbe::value::SlotId stateSlot,
        EvalStage stage,
        PlanNodeId planNodeId,
        sbe::value::SlotIdGenerator* slotIdGenerator,
        sbe::value::FrameIdGenerator* frameIdGenerator) const override {
        sbe::EVariable stateVar{stateSlot};
        auto indexSlot = slotIdGenerator->generate();

        auto indexInt64 = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::sub, stateVar.clone(), makeConstant(ValueType, 1));

        auto indexInt32 = makeLocalBind(
            frameIdGenerator,
            [&](sbe::EVariable convertedIndex) {
                return sbe::makeE<sbe::EIf>(
                    makeFunction("exists", convertedIndex.clone()),
                    convertedIndex.clone(),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{5291403},
                                           "Cannot convert array index into int32 number"));
            },
            sbe::makeE<sbe::ENumericConvert>(std::move(indexInt64),
                                             sbe::value::TypeTags::NumberInt32));

        auto resultStage = makeProject(
            std::move(stage),
            planNodeId,
            indexSlot,
            sbe::makeE<sbe::EIf>(sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::greater,
                                                              stateVar.clone(),
                                                              makeConstant(ValueType, 0)),
                                 std::move(indexInt32),
                                 makeConstant(sbe::value::TypeTags::Nothing, 0)));
        return {indexSlot, std::move(resultStage)};
    }

    EvalExprStagePair makePredicateCombinator(EvalExprStagePair pair) const override {
        auto [expr, stage] = std::move(pair);
        return {makeState(expr.extractExpr()), std::move(stage)};
    }

    EvalStage makeTraverseCombinator(EvalStage outer,
                                     EvalStage inner,
                                     sbe::value::SlotId inputSlot,
                                     sbe::value::SlotId outputSlot,
                                     sbe::value::SlotId innerOutputSlot,
                                     PlanNodeId planNodeId,
                                     sbe::value::FrameIdGenerator* frameIdGenerator) const override;
};

/**
 * This class implements 'FilterStateHelper' interface for a plain boolean state, without index
 * part.
 */
class BooleanStateHelper : public FilterStateHelper {
public:
    bool stateContainsValue() const override {
        return false;
    }

    Expression makeState(bool value) const override {
        return makeConstant(sbe::value::TypeTags::Boolean, value);
    }

    Expression makeState(Expression expr) const override {
        return expr;
    }

    Expression makeInitialState(Expression expr) const override {
        return expr;
    }

    Expression getBool(Expression expr) const override {
        return expr;
    }

    Expression mergeStates(Expression left,
                           Expression right,
                           sbe::value::FrameIdGenerator* frameIdGenerator) const override {
        return sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicOr, std::move(left), std::move(right));
    }

    std::pair<boost::optional<sbe::value::SlotId>, EvalStage> projectValueCombinator(
        sbe::value::SlotId stateSlot,
        EvalStage stage,
        PlanNodeId planNodeId,
        sbe::value::SlotIdGenerator* slotIdGenerator,
        sbe::value::FrameIdGenerator* frameIdGenerator) const override {
        return {stateSlot, std::move(stage)};
    }

    EvalExprStagePair makePredicateCombinator(EvalExprStagePair pair) const override {
        return pair;
    }

    EvalStage makeTraverseCombinator(
        EvalStage outer,
        EvalStage inner,
        sbe::value::SlotId inputSlot,
        sbe::value::SlotId outputSlot,
        sbe::value::SlotId innerOutputSlot,
        PlanNodeId planNodeId,
        sbe::value::FrameIdGenerator* frameIdGenerator) const override {
        return makeTraverse(
            std::move(outer),
            std::move(inner),
            inputSlot,
            outputSlot,
            innerOutputSlot,
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                         sbe::makeE<sbe::EVariable>(outputSlot),
                                         sbe::makeE<sbe::EVariable>(innerOutputSlot)),
            sbe::makeE<sbe::EVariable>(outputSlot),
            planNodeId,
            1);
    }
};

/**
 * Helper function to create respective 'FilterStateHelper' implementation. If 'trackIndex' is true,
 * returns 'IndexStateHelper'. Otherwise, returns 'BooleanStateHelper'.
 */
std::unique_ptr<FilterStateHelper> makeFilterStateHelper(bool trackIndex);

/**
 * Creates tree with short-circuiting for OR and AND. Each element in 'braches' argument represents
 * logical expression and sub-tree generated for it.
 */
EvalExprStagePair generateShortCircuitingLogicalOp(sbe::EPrimBinary::Op logicOp,
                                                   std::vector<EvalExprStagePair> branches,
                                                   PlanNodeId planNodeId,
                                                   sbe::value::SlotIdGenerator* slotIdGenerator,
                                                   const FilterStateHelper& stateHelper);

}  // namespace mongo::stage_builder
