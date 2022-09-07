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
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/projection_ast.h"
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

std::unique_ptr<sbe::EExpression> generateNullOrMissing(sbe::FrameId frameId,
                                                        sbe::value::SlotId slotId);

std::unique_ptr<sbe::EExpression> generateNullOrMissing(std::unique_ptr<sbe::EExpression> arg);

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
 * Generates an EExpression that checks if the input expression is a numeric Infinity.
 */
std::unique_ptr<sbe::EExpression> generateInfinityCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a non-positive number (i.e. <= 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a positive number (i.e. > 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generatePositiveCheck(const sbe::EVariable& var);

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

std::unique_ptr<sbe::EExpression> generateNonTimestampCheck(const sbe::EVariable& var);

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
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e);

/**
 * Creates an EFunction expression with the given name and arguments.
 */
template <typename... Args>
inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name, Args&&... args) {
    return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
}

template <typename T>
inline auto makeConstant(sbe::value::TypeTags tag, T value) {
    return sbe::makeE<sbe::EConstant>(tag, sbe::value::bitcastFrom<T>(value));
}

inline auto makeConstant(StringData str) {
    auto [tag, value] = sbe::value::makeNewString(str);
    return sbe::makeE<sbe::EConstant>(tag, value);
}

std::unique_ptr<sbe::EExpression> makeVariable(sbe::value::SlotId slotId);

std::unique_ptr<sbe::EExpression> makeVariable(sbe::FrameId frameId, sbe::value::SlotId slotId);

inline auto makeFail(int code, StringData errorMessage) {
    return sbe::makeE<sbe::EFail>(ErrorCodes::Error{code}, errorMessage);
}

/**
 * Check if expression returns Nothing and return null if so. Otherwise, return the expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e);

/**
 * Check if expression returns Nothing and return bsonUndefined if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyUndefined(std::unique_ptr<sbe::EExpression> e);

/**
 * Check if expression returns an array and return Nothing if so. Otherwise, return the expression.
 */
std::unique_ptr<sbe::EExpression> makeNothingArrayCheck(
    std::unique_ptr<sbe::EExpression> isArrayInput, std::unique_ptr<sbe::EExpression> otherwise);

/**
 * Makes "newObj" function from variadic parameter pack of 'FieldPair' which is a pair of a field
 * name and field expression.
 */
template <typename... Ts>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(Ts... fields);

using FieldPair = std::pair<StringData, std::unique_ptr<sbe::EExpression>>;
template <size_t N>
using FieldExprs = std::array<std::unique_ptr<sbe::EExpression>, N>;

// The following two template functions convert 'FieldPair' to two 'EExpression's and add them to
// 'EExpression' array which will be converted back to variadic parameter pack for 'makeFunction()'.
template <size_t N, size_t... Is>
FieldExprs<N + 2> array_append(FieldExprs<N> fieldExprs,
                               const std::index_sequence<Is...>&,
                               std::unique_ptr<sbe::EExpression> nameExpr,
                               std::unique_ptr<sbe::EExpression> valExpr) {
    return FieldExprs<N + 2>{std::move(fieldExprs[Is])..., std::move(nameExpr), std::move(valExpr)};
}
template <size_t N>
FieldExprs<N + 2> array_append(FieldExprs<N> fieldExprs, FieldPair field) {
    return array_append(std::move(fieldExprs),
                        std::make_index_sequence<N>{},
                        makeConstant(field.first),
                        std::move(field.second));
}

// The following two template functions convert the 'EExpression' array back to variadic parameter
// pack and calls the 'makeFunction("newObj")'.
template <size_t N, size_t... Is>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs,
                                                     const std::index_sequence<Is...>&) {
    return makeFunction("newObj", std::move(fieldExprs[Is])...);
}
template <size_t N>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs) {
    return makeNewObjFunction(std::move(fieldExprs), std::make_index_sequence<N>{});
}

// Deals with the last 'FieldPair' and adds it to the 'EExpression' array.
template <size_t N>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs, FieldPair field) {
    return makeNewObjFunction(array_append(std::move(fieldExprs), std::move(field)));
}

// Deals with the intermediary 'FieldPair's and adds them to the 'EExpression' array.
template <size_t N, typename... Ts>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs,
                                                     FieldPair field,
                                                     Ts... fields) {
    return makeNewObjFunction(array_append(std::move(fieldExprs), std::move(field)),
                              std::forward<Ts>(fields)...);
}

// Deals with the first 'FieldPair' and adds it to the 'EExpression' array.
template <typename... Ts>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldPair field, Ts... fields) {
    return makeNewObjFunction(FieldExprs<2>{makeConstant(field.first), std::move(field.second)},
                              std::forward<Ts>(fields)...);
}

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
    return {sbe::makeS<sbe::FilterStage<IsConst, IsEof>>(
                stage.extractStage(planNodeId), std::move(filter), planNodeId),
            stage.extractOutSlots()};
}

EvalStage makeProject(EvalStage stage,
                      sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects,
                      PlanNodeId planNodeId);

template <typename... Ts>
EvalStage makeProject(EvalStage stage, PlanNodeId planNodeId, Ts&&... pack) {
    return makeProject(std::move(stage), makeEM(std::forward<Ts>(pack)...), planNodeId);
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
 * Creates a branch stage with the specified condition ifExpr.
 */
EvalStage makeBranch(EvalStage thenStage,
                     EvalStage elseStage,
                     std::unique_ptr<sbe::EExpression> ifExpr,
                     sbe::value::SlotVector thenVals,
                     sbe::value::SlotVector elseVals,
                     sbe::value::SlotVector outputVals,
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

EvalStage makeLimitSkip(EvalStage input,
                        PlanNodeId planNodeId,
                        boost::optional<long long> limit,
                        boost::optional<long long> skip = boost::none);

EvalStage makeUnion(std::vector<EvalStage> inputStages,
                    std::vector<sbe::value::SlotVector> inputVals,
                    sbe::value::SlotVector outputVals,
                    PlanNodeId planNodeId);

EvalStage makeHashAgg(EvalStage stage,
                      sbe::value::SlotVector gbs,
                      sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs,
                      boost::optional<sbe::value::SlotId> collatorSlot,
                      bool allowDiskUse,
                      PlanNodeId planNodeId);

EvalStage makeMkBsonObj(EvalStage stage,
                        sbe::value::SlotId objSlot,
                        boost::optional<sbe::value::SlotId> rootSlot,
                        boost::optional<sbe::MakeObjFieldBehavior> fieldBehavior,
                        std::vector<std::string> fields,
                        std::vector<std::string> projectFields,
                        sbe::value::SlotVector projectVars,
                        bool forceNewObject,
                        bool returnOldObject,
                        PlanNodeId planNodeId);

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
 * Helper functions for converting from BSONObj/BSONArray to SBE Object/Array. Caller owns the SBE
 * Object/Array returned. These helper functions do not assume ownership of the BSONObj/BSONArray.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONObj& bo);
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba);

/**
 * Helper function for converting mongo::Value to SBE Value. Caller owns the SBE Value returned.
 * This helper function does not assume ownership of the mongo::Value.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const Value& val);

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


std::tuple<sbe::value::SlotId, sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                     sbe::value::SlotId seekKeySlot,
                     sbe::value::SlotId snapshotIdSlot,
                     sbe::value::SlotId indexIdSlot,
                     sbe::value::SlotId indexKeySlot,
                     sbe::value::SlotId indexKeyPatternSlot,
                     const CollectionPtr& collToFetch,
                     StringMap<const IndexAccessMethod*> iamMap,
                     PlanNodeId planNodeId,
                     sbe::value::SlotVector slotsToForward,
                     sbe::value::SlotIdGenerator& slotIdGenerator);

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

/**
 * Imagine that we have some parent QuerySolutionNode X and child QSN Y which both participate in a
 * covered plan. Stage X requests some slots to be constructed out of the index keys using
 * 'parentIndexKeyReqs'. Stage Y requests it's own slots, and adds those to the set requested by X,
 * resulting in 'childIndexKeyReqs'. Note the invariant that 'childIndexKeyReqs' is a superset of
 * 'parentIndexKeyReqs'. Let's notate the number of slots requested by 'childIndexKeyReqs' as |Y|
 * and the set of slots requested by 'parentIndexKeyReqs' as |X|.
 *
 * The underlying SBE plan is constructed, and returns a vector of |Y| slots. However, the parent
 * stage expects a vector of just |X| slots. The purpose of this function is to calculate and return
 * the appropriate subset of the slot vector so that the parent stage X receives its expected |X|
 * slots.
 *
 * As a concrete example, let's say the QSN tree is X => Y => IXSCAN and the index key pattern is
 * {a: 1, b: 1, c: 1, d: 1}. X requests "a" and "d" using the bit vector 1001. Y additionally
 * requires "c" so it requests three slots with the bit vector 1011. As a result, Y receives a
 * 3-element slot vector, <s1, s2, s3>. Here, s1 will contain the value of "a", s2 contains "c", and
 * s3 contain s"d".
 *
 * Parent QSN X expects just a two element slot vector where the first slot is for "a" and the
 * second is for "d". This function would therefore return the slot vector <s1, s3>.
 */
sbe::value::SlotVector makeIndexKeyOutputSlotsMatchingParentReqs(
    const BSONObj& indexKeyPattern,
    sbe::IndexKeysInclusionSet parentIndexKeyReqs,
    sbe::IndexKeysInclusionSet childIndexKeyReqs,
    sbe::value::SlotVector childOutputSlots);

/**
 * Given an index key pattern, and a subset of the fields of the index key pattern that are depended
 * on to compute the query, returns the corresponding 'IndexKeysInclusionSet' bit vector and field
 * name vector.
 *
 * For example, suppose that we have an index key pattern {d: 1, c: 1, b: 1, a: 1}, and the caller
 * depends on the set of 'requiredFields' {"b", "d"}. In this case, the pair of return values would
 * be:
 *  - 'IndexKeysInclusionSet' bit vector of 1010
 *  - Field name vector of <"d", "b">
 */
template <typename T>
std::pair<sbe::IndexKeysInclusionSet, std::vector<std::string>> makeIndexKeyInclusionSet(
    const BSONObj& indexKeyPattern, const T& requiredFields) {
    sbe::IndexKeysInclusionSet indexKeyBitset;
    std::vector<std::string> keyFieldNames;
    size_t i = 0;
    for (auto&& elt : indexKeyPattern) {
        if (requiredFields.count(elt.fieldName())) {
            indexKeyBitset.set(i);
            keyFieldNames.push_back(elt.fieldName());
        }

        ++i;
    }

    return {std::move(indexKeyBitset), std::move(keyFieldNames)};
}

struct PlanStageData;

/**
 * Common parameters to SBE stage builder functions extracted into separate class to simplify
 * argument passing. Also contains a mapping of global variable ids to slot ids.
 */
struct StageBuilderState {
    StageBuilderState(OperationContext* opCtx,
                      PlanStageData* data,
                      const Variables& variables,
                      sbe::value::SlotIdGenerator* slotIdGenerator,
                      sbe::value::FrameIdGenerator* frameIdGenerator,
                      sbe::value::SpoolIdGenerator* spoolIdGenerator,
                      bool needsMerge,
                      bool allowDiskUse)
        : slotIdGenerator{slotIdGenerator},
          frameIdGenerator{frameIdGenerator},
          spoolIdGenerator{spoolIdGenerator},
          opCtx{opCtx},
          data{data},
          variables{variables},
          needsMerge{needsMerge},
          allowDiskUse{allowDiskUse} {}

    StageBuilderState(const StageBuilderState& other) = delete;

    sbe::value::SlotId getGlobalVariableSlot(Variables::Id variableId);

    sbe::value::SlotId slotId() {
        return slotIdGenerator->generate();
    }

    sbe::FrameId frameId() {
        return frameIdGenerator->generate();
    }

    sbe::SpoolId spoolId() {
        return spoolIdGenerator->generate();
    }

    /**
     * Register a Slot in the 'RuntimeEnvironment'. The newly registered Slot should be associated
     * with 'paramId' and tracked in the 'InputParamToSlotMap' for auto-parameterization use. The
     * slot is set to 'Nothing' on registration and will be populated with the real value when
     * preparing the SBE plan for execution.
     */
    sbe::value::SlotId registerInputParamSlot(MatchExpression::InputParamId paramId);

    sbe::value::SlotIdGenerator* const slotIdGenerator;
    sbe::value::FrameIdGenerator* const frameIdGenerator;
    sbe::value::SpoolIdGenerator* const spoolIdGenerator;

    OperationContext* const opCtx;
    PlanStageData* const data;

    const Variables& variables;
    // When the mongos splits $group stage and sends it to shards, it adds 'needsMerge'/'fromMongs'
    // flags to true so that shards can sends special partial aggregation results to the mongos.
    bool needsMerge;

    // A flag to indicate the user allows disk use for spilling.
    bool allowDiskUse;

    // This map is used to plumb through pre-generated field expressions ('EvalExpr')
    // corresponding to field paths to 'generateExpression' to avoid repeated expression generation.
    // Key is expected to represent field paths in form CURRENT.<field_name>[.<field_name>]*.
    stdx::unordered_map<std::string /*field path*/, EvalExpr> preGeneratedExprs;
};

/**
 * Tree representing index key pattern or a subset of it.
 *
 * For example, the key pattern {a.b: 1, x: 1, a.c: 1} would look like:
 *
 *         <root>
 *         /   |
 *        a    x
 *       / \
 *      b   c
 *
 * This tree is used for building SBE subtrees to re-hydrate index keys and for covered projections.
 */
struct IndexKeyPatternTreeNode {
    IndexKeyPatternTreeNode* emplace(StringData fieldComponent) {
        auto newNode = std::make_unique<IndexKeyPatternTreeNode>();
        const auto newNodeRaw = newNode.get();
        children.emplace(fieldComponent, std::move(newNode));
        childrenOrder.push_back(fieldComponent.toString());

        return newNodeRaw;
    }

    /**
     * Returns leaf node matching field path. If the field path provided resolves to a non-leaf
     * node, null will be returned.
     *
     * For example, if tree was built for key pattern {a: 1, a.b: 1}, this method will return
     * nullptr for field path "a". On the other hand, this method will return corresponding node for
     * field path "a.b".
     */
    IndexKeyPatternTreeNode* findLeafNode(const FieldRef& fieldRef, size_t currentIndex = 0) {
        if (currentIndex == fieldRef.numParts()) {
            if (children.empty()) {
                return this;
            }
            return nullptr;
        }

        auto currentPart = fieldRef.getPart(currentIndex);
        if (auto it = children.find(currentPart); it != children.end()) {
            return it->second->findLeafNode(fieldRef, currentIndex + 1);
        } else {
            return nullptr;
        }
    }

    StringMap<std::unique_ptr<IndexKeyPatternTreeNode>> children;
    std::vector<std::string> childrenOrder;

    // Which slot the index key for this component is stored in. May be boost::none for non-leaf
    // nodes.
    boost::optional<sbe::value::SlotId> indexKeySlot;
};

std::unique_ptr<IndexKeyPatternTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                             const sbe::value::SlotVector& slots);
std::unique_ptr<sbe::EExpression> buildNewObjExpr(const IndexKeyPatternTreeNode* kpTree);

std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot);

}  // namespace mongo::stage_builder
