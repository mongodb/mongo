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
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::projection_ast {
class Projection;
}

namespace mongo::stage_builder {

class PlanStageSlots;

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
std::unique_ptr<sbe::EExpression> generateNullOrMissing(EvalExpr arg, StageBuilderState& state);

/**
 * Generates an EExpression that checks if the input expression is a non-numeric type _assuming
 * that_ it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var);
std::unique_ptr<sbe::EExpression> generateNonNumericCheck(EvalExpr expr, StageBuilderState& state);

/**
 * Generates an EExpression that checks if the input expression is the value NumberLong(-2^64).
 */
std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var);
std::unique_ptr<sbe::EExpression> generateNaNCheck(EvalExpr expr, StageBuilderState& state);

/**
 * Generates an EExpression that checks if the input expression is a numeric Infinity.
 */
std::unique_ptr<sbe::EExpression> generateInfinityCheck(const sbe::EVariable& var);
std::unique_ptr<sbe::EExpression> generateInfinityCheck(EvalExpr expr, StageBuilderState& state);

/**
 * Generates an EExpression that checks if the input expression is a non-positive number (i.e. <= 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a positive number (i.e. > 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generatePositiveCheck(const sbe::EExpression& expr);

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
std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EExpression& expr);

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

std::unique_ptr<sbe::EExpression> makeMoveVariable(sbe::FrameId frameId, sbe::value::SlotId slotId);

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
 * inputExpr. If the binding is an array, the array is returned. This is done so caller can check
 * for array and generate an empty shard key. Here is an example expression generated from this
 * function for a shard key pattern {'a.b.c': 1}:
 *
 * let [l1.0 = getField (s1, "a") ?: null]
 *   if (isArray (l1.0), l1.0,
 *     let [l2.0 = getField (l1.0, "b") ?: null]
 *       if (isArray (l2.0), l2.0, getField (l2.0, "c") ?: null))
 */
std::unique_ptr<sbe::EExpression> generateShardKeyBinding(
    const sbe::MatchPath& keyPatternField,
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
    sbe::value::SlotIdGenerator* slotIdGenerator,
    StageBuilderState& state);

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
                      sbe::HashAggStage::AggExprVector aggs,
                      boost::optional<sbe::value::SlotId> collatorSlot,
                      bool allowDiskUse,
                      sbe::SlotExprPairVector mergingExprs,
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

/**
 * Creates a chain of EIf expressions that will inspect each arg in order and return the first
 * arg that is not null or missing.
 */
std::unique_ptr<sbe::EExpression> makeIfNullExpr(
    std::vector<std::unique_ptr<sbe::EExpression>> values,
    sbe::value::FrameIdGenerator* frameIdGenerator);

/** This helper takes an SBE SlotIdGenerator and an SBE Array and returns an output slot and a
 * unwind/project/limit/coscan subtree that streams out the elements of the array one at a time via
 * the output slot over a series of calls to getNext(), mimicking the output of a collection scan or
 * an index scan. Note that this method assumes ownership of the SBE Array being passed in.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy = nullptr);

/**
 * Make a mock scan with multiple output slots from an BSON array. This method does NOT assume
 * ownership of the BSONArray passed in.
 */
std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy = nullptr);

/**
 * Helper functions for converting from BSONObj/BSONArray to SBE Object/Array. Caller owns the SBE
 * Object/Array returned. These helper functions do not assume ownership of the BSONObj/BSONArray.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONObj& bo);
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

std::unique_ptr<sbe::PlanStage> makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                                     sbe::value::SlotId resultSlot,
                                                     sbe::value::SlotId recordIdSlot,
                                                     std::vector<std::string> fields,
                                                     sbe::value::SlotVector fieldSlots,
                                                     sbe::value::SlotId seekKeySlot,
                                                     sbe::value::SlotId snapshotIdSlot,
                                                     sbe::value::SlotId indexIdentSlot,
                                                     sbe::value::SlotId indexKeySlot,
                                                     sbe::value::SlotId indexKeyPatternSlot,
                                                     const CollectionPtr& collToFetch,
                                                     PlanNodeId planNodeId,
                                                     sbe::value::SlotVector slotsToForward);

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

    // Holds the mapping between the custom ABT variable names and the slot id they are referencing.
    optimizer::SlotVarMap slotVarMap;

    StringMap<sbe::value::SlotId> stringConstantToSlotMap;
    SimpleBSONObjMap<sbe::value::SlotId> keyPatternToSlotMap;
};

/**
 * A tree of nodes arranged based on field path. PathTreeNode can be used to represent index key
 * patterns, projections, etc. A PathTreeNode can also optionally hold a value of type T.
 *
 * For example, the key pattern {a.b: 1, x: 1, a.c: 1} in tree form would look like:
 *
 *         <root>
 *         /   |
 *        a    x
 *       / \
 *      b   c
 */
template <typename T>
struct PathTreeNode {
    PathTreeNode() = default;
    explicit PathTreeNode(std::string name) : name(std::move(name)) {}

    // Aside from the root node, it is very common for a node to have no children or only 1 child.
    using ChildrenVector = absl::InlinedVector<std::unique_ptr<PathTreeNode<T>>, 1>;

    // It is the caller's responsibility to verify that there is not an existing field with the
    // same name as 'fieldComponent'.
    PathTreeNode<T>* emplace_back(std::string fieldComponent) {
        auto newNode = std::make_unique<PathTreeNode<T>>(std::move(fieldComponent));
        const auto newNodeRaw = newNode.get();
        children.emplace_back(std::move(newNode));

        if (childrenMap) {
            childrenMap->emplace(newNodeRaw->name, newNodeRaw);
        } else if (children.size() >= 3) {
            // If 'childrenMap' is null and there are 3 or more children, build 'childrenMap' now.
            buildChildrenMap();
        }

        return newNodeRaw;
    }

    bool isLeaf() const {
        return children.empty();
    }

    PathTreeNode<T>* findChild(StringData fieldComponent) {
        if (childrenMap) {
            auto it = childrenMap->find(fieldComponent);
            return it != childrenMap->end() ? it->second : nullptr;
        }
        for (auto&& child : children) {
            if (child->name == fieldComponent) {
                return child.get();
            }
        }
        return nullptr;
    }

    PathTreeNode<T>* findNode(const sbe::MatchPath& fieldRef, size_t currentIndex = 0) {
        if (currentIndex == fieldRef.numParts()) {
            return this;
        }

        auto currentPart = fieldRef.getPart(currentIndex);
        if (auto child = findChild(currentPart)) {
            return child->findNode(fieldRef, currentIndex + 1);
        } else {
            return nullptr;
        }
    }

    /**
     * Returns leaf node matching field path. If the field path provided resolves to a non-leaf
     * node, null will be returned. For example, if tree was built for key pattern {a.b: 1}, this
     * method will return nullptr for field path "a".
     */
    PathTreeNode<T>* findLeafNode(const sbe::MatchPath& fieldRef, size_t currentIndex = 0) {
        auto* node = findNode(fieldRef, currentIndex);
        return (node && node->isLeaf() ? node : nullptr);
    }

    void clearChildren() {
        children.clear();
        childrenMap.reset();
    }

    void buildChildrenMap() {
        if (!childrenMap) {
            childrenMap = std::make_unique<StringDataMap<PathTreeNode<T>*>>();
            for (auto&& child : children) {
                childrenMap->insert_or_assign(child->name, child.get());
            }
        }
    }

    std::string name;

    ChildrenVector children;

    // We only build a hash map when there are 3 or more children. The vast majority of nodes
    // will have 2 children or less, so we dynamically allocate 'childrenMap' to save space.
    std::unique_ptr<StringDataMap<PathTreeNode<T>*>> childrenMap;

    T value = {};
};

using SlotTreeNode = PathTreeNode<boost::optional<sbe::value::SlotId>>;

std::unique_ptr<SlotTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                  const sbe::value::SlotVector& slots);

std::unique_ptr<sbe::EExpression> buildNewObjExpr(const SlotTreeNode* slotTree);

std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot);

std::unique_ptr<SlotTreeNode> buildSlotTreeForProjection(const projection_ast::Projection& proj);

template <typename T>
inline const char* getRawStringData(const T& str) {
    if constexpr (std::is_same_v<T, StringData>) {
        return str.rawData();
    } else {
        return str.data();
    }
}

template <typename T, typename IterT, typename StringT>
inline auto buildPathTreeImpl(const std::vector<StringT>& paths,
                              boost::optional<IterT> valsBegin,
                              boost::optional<IterT> valsEnd,
                              bool removeConflictingPaths) {
    auto tree = std::make_unique<PathTreeNode<T>>();
    auto valsIt = std::move(valsBegin);

    for (auto&& pathStr : paths) {
        auto path = sbe::MatchPath{pathStr};

        size_t numParts = path.numParts();
        size_t i = 0;

        auto* node = tree.get();
        StringData part;
        for (; i < numParts; ++i) {
            part = path.getPart(i);
            auto child = node->findChild(part);
            if (!child) {
                break;
            }
            node = child;
        }

        // When 'removeConflictingPaths' is true, if we're processing a sub-path of another path
        // that's already been processed, then we should ignore the sub-path.
        const bool ignorePath = (removeConflictingPaths && node->isLeaf() && node != tree.get());
        if (!ignorePath) {
            if (i < numParts) {
                node = node->emplace_back(std::string(part));
                for (++i; i < numParts; ++i) {
                    node = node->emplace_back(std::string(path.getPart(i)));
                }
            } else if (removeConflictingPaths && !node->isLeaf()) {
                // If 'removeConflictingPaths' is true, delete any children that 'node' has.
                node->clearChildren();
            }
            if (valsIt) {
                tassert(7182003,
                        "buildPathTreeImpl() did not expect iterator 'valsIt' to reach the end",
                        !valsEnd || *valsIt != *valsEnd);

                node->value = **valsIt;
            }
        }

        if (valsIt) {
            ++(*valsIt);
        }
    }

    return tree;
}

/**
 * Builds a path tree from a set of paths and returns the root node of the tree.
 *
 * If 'removeConflictingPaths' is false, this function will build a tree that contains all paths
 * specified in 'paths' (regardless of whether there are any paths that conflict).
 *
 * If 'removeConflictingPaths' is true, when there are two conflicting paths (ex. "a" and "a.b")
 * the conflict is resolved by removing the longer path ("a.b") and keeping the shorter path ("a").
 */
template <typename T, typename StringT>
auto buildPathTree(const std::vector<StringT>& paths, bool removeConflictingPaths) {
    return buildPathTreeImpl<T, std::move_iterator<typename std::vector<T>::iterator>, StringT>(
        paths, boost::none, boost::none, removeConflictingPaths);
}

/**
 * Builds a path tree from a set of paths, assigns a sequence of values to the sequence of nodes
 * corresponding to each path, and returns the root node of the tree.
 *
 * The 'values' sequence/vector and the 'paths' vector are expected to have the same number of
 * elements. The nth value in the the 'values' sequence will be assigned to the node corresponding
 * to the nth path in 'paths'.
 *
 * If 'removeConflictingPaths' is false, this function will build a tree that contains all paths
 * specified in 'paths' (regardless of whether there are any paths that conflict).
 *
 * If 'removeConflictingPaths' is true, when there are two conflicting paths (ex. "a" and "a.b")
 * the conflict is resolved by removing the longer path ("a.b") and keeping the shorter path ("a").
 * Note that when a path from 'paths' is removed due to a conflict, the corresponding value in
 * 'values' will be ignored.
 */
template <typename T, typename IterT, typename StringT>
auto buildPathTree(const std::vector<StringT>& paths,
                   IterT valuesBegin,
                   IterT valuesEnd,
                   bool removeConflictingPaths) {
    return buildPathTreeImpl<T, IterT, StringT>(
        paths, std::move(valuesBegin), std::move(valuesEnd), removeConflictingPaths);
}

template <typename T, typename U>
auto buildPathTree(const std::vector<std::string>& paths,
                   const std::vector<U>& values,
                   bool removeConflictingPaths) {
    tassert(7182004,
            "buildPathTreeImpl() expects 'paths' and 'values' to be the same size",
            paths.size() == values.size());

    return buildPathTree<T>(paths, values.begin(), values.end(), removeConflictingPaths);
}

template <typename T, typename U>
auto buildPathTree(const std::vector<std::string>& paths,
                   std::vector<U>&& values,
                   bool removeConflictingPaths) {
    tassert(7182005,
            "buildPathTreeImpl() expects 'paths' and 'values' to be the same size",
            paths.size() == values.size());

    return buildPathTree<T>(paths,
                            std::make_move_iterator(values.begin()),
                            std::make_move_iterator(values.end()),
                            removeConflictingPaths);
}

/**
 * If a boolean can be constructed from type T, this function will construct a boolean from 'value'
 * and then return the negation. If a boolean cannot be constructed from type T, then this function
 * returns false.
 */
template <typename T>
bool convertsToFalse(const T& value) {
    if constexpr (std::is_constructible_v<bool, T>) {
        return !bool(value);
    } else {
        return false;
    }
}

template <typename T>
struct InvokeAndReturnBoolHelper {
    template <typename FuncT, typename... Args>
    static bool invoke(FuncT&& fn, bool defaultReturnValue, Args&&... args) {
        (std::forward<FuncT>(fn))(std::forward<Args>(args)...);
        return defaultReturnValue;
    }
};
template <>
struct InvokeAndReturnBoolHelper<bool> {
    template <typename FuncT, typename... Args>
    static bool invoke(FuncT&& fn, bool, Args&&... args) {
        return (std::forward<FuncT>(fn))(std::forward<Args>(args)...);
    }
};

/**
 * This function will invoke an invocable object ('fn') with the specified arguments ('args'). If
 * 'fn' returns bool, this function will return fn's return value. If 'fn' returns void or some type
 * other than bool, this function will return 'defaultReturnValue'.
 *
 * As a special case, this function alows fn's type (FuncT) to be nullptr_t. In such cases, this
 * function will do nothing and it will return 'defaultReturnValue'.
 *
 * If 'fn' is not invocable with the specified arguments and fn's type is not nullptr_t, this
 * function will raise a static assertion.
 *
 * Note that when a bool can be constructed from 'fn' (for example, if FuncT is a function pointer
 * type), this method will always invoke 'fn' regardless of whether "!bool(fn)" is true or false.
 * It is the caller's responsibility to do any necessary checks (ex. null checks) before calling
 * this function.
 */
template <typename FuncT, typename... Args>
inline bool invokeAndReturnBool(FuncT&& fn, bool defaultReturnValue, Args&&... args) {
    if constexpr (std::is_invocable_v<FuncT, Args...>) {
        return InvokeAndReturnBoolHelper<typename std::invoke_result<FuncT, Args...>::type>::invoke(
            std::forward<FuncT>(fn), defaultReturnValue, std::forward<Args>(args)...);
    } else {
        static_assert(std::is_null_pointer_v<std::remove_reference_t<FuncT>>);
        return defaultReturnValue;
    }
}

/**
 * This is a helper function used by visitPathTreeNodes() to invoke preVisit and postVisit callback
 * functions. This helper function will check if 'fn' supports invocation with the following args:
 *   (1) Node* node, const std::string& path, const DfsState& dfsState
 *   (2) Node* node, const DfsState& dfsState
 *   (3) Node* node, const std::string& path
 *   (4) Node* node
 *
 * After checking what 'fn' supports, this helper function will then use invokeAndReturnBool() to
 * invoke 'fn' accordingly and it will return invokeAndReturnBool()'s return value. If 'fn' supports
 * multiple signatures, whichever signature that appears first in the list above will be used.
 */
template <typename NodeT, typename FuncT>
inline bool invokeVisitPathTreeNodesCallback(
    FuncT&& fn,
    NodeT* node,
    const std::string& path,
    const std::vector<std::pair<NodeT*, size_t>>& dfsState) {
    using DfsState = std::vector<std::pair<NodeT*, size_t>>;

    if constexpr (std::is_invocable_v<FuncT, NodeT*, const std::string&, const DfsState&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, path, dfsState);
    } else if constexpr (std::is_invocable_v<FuncT, NodeT*, const DfsState&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, dfsState);
    } else if constexpr (std::is_invocable_v<FuncT, NodeT*, const std::string&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, path);
    } else {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node);
    }
}

/**
 * This function performs a DFS traversal on a path tree (as given by 'treeRoot') and it invokes
 * the specified preVisit and postVisit callbacks at the appropriate times.
 *
 * The caller may pass nullptr for 'preVisit' if they do not wish to perform any pre-visit actions,
 * and likewise the caller may pass nullptr for 'postVisit' if they do not wish to perform any
 * post-visit actions.
 *
 * Assuming 'preVisit' is not null, the 'preVisit' callback must support one of the following
 * signatures:
 *   (1) Node* node, const std::string& path, const DfsState& dfsState
 *   (2) Node* node, const DfsState& dfsState
 *   (3) Node* node, const std::string& path
 *   (4) Node* node
 *
 * Likewise, assuming 'postVisit' is not null, the 'postVisit' callback must support one of the
 * signatures listed above. For details, see invokeVisitPathTreeNodesCallback().
 *
 * The 'preVisit' callback can return any type. If preVisit's return type is not bool or if
 * 'preVisit' returns boolean true, then preVisit's return value is ignored. If preVisit's return
 * type is bool _and_ preVisit returns boolean false, then the node that was just pre-visited will
 * be "skipped" and its descendents will not be visited (i.e. instead of the DFS descending, it will
 * backtrack), and likewise the 'postVisit' callback will be "skipped" as well and won't be invoked
 * for the node.
 *
 * The 'postVisit' callback can return any type. postVisit's return value (if any) is ignored.
 *
 * If 'invokeCallbacksForRootNode' is false (which is the default), the preVisit and postVisit
 * callbacks won't be invoked for the root node of the tree. If 'invokeCallbacksForRootNode' is
 * true, the preVisit and postVisit callbacks will be invoked for the root node of the tree at
 * the appropriate times.
 *
 * The 'rootPath' parameter allows the caller to specify the absolute path of 'treeRoot', which
 * will be used as the base/prefix to determine the paths of all the other nodes in the tree. If
 * no 'rootPath' argument is provided, then 'rootPath' defaults to boost::none.
 */
template <typename T, typename PreVisitFn, typename PostVisitFn>
void visitPathTreeNodes(PathTreeNode<T>* treeRoot,
                        const PreVisitFn& preVisit,
                        const PostVisitFn& postVisit,
                        bool invokeCallbacksForRootNode = false,
                        boost::optional<std::string> rootPath = boost::none) {
    using Node = PathTreeNode<T>;
    using DfsState = std::vector<std::pair<Node*, size_t>>;
    constexpr bool isPathNeeded =
        std::is_invocable_v<PreVisitFn, Node*, const std::string&, const DfsState&> ||
        std::is_invocable_v<PreVisitFn, Node*, const std::string&> ||
        std::is_invocable_v<PostVisitFn, Node*, const std::string&, const DfsState&> ||
        std::is_invocable_v<PostVisitFn, Node*, const std::string&>;

    if (!treeRoot || treeRoot->children.empty()) {
        return;
    }

    const bool hasPreVisit = !convertsToFalse(preVisit);
    const bool hasPostVisit = !convertsToFalse(postVisit);

    // Perform a depth-first traversal using 'dfs' to keep track of where we are.
    DfsState dfs;
    dfs.emplace_back(treeRoot, std::numeric_limits<size_t>::max());
    boost::optional<std::string> path = std::move(rootPath);
    const std::string emptyPath;

    auto getPath = [&]() -> const std::string& {
        return path ? *path : emptyPath;
    };
    auto dfsPop = [&] {
        dfs.pop_back();
        if (isPathNeeded && path) {
            if (auto pos = path->find_last_of('.'); pos != std::string::npos) {
                path->resize(pos);
            } else {
                path = boost::none;
            }
        }
    };

    if (hasPreVisit && invokeCallbacksForRootNode) {
        // Invoke the pre-visit callback on the root node if appropriate.
        if (!invokeVisitPathTreeNodesCallback(preVisit, treeRoot, getPath(), dfs)) {
            dfsPop();
        }
    }

    while (!dfs.empty()) {
        ++dfs.back().second;
        auto [node, idx] = dfs.back();
        const bool isRootNode = dfs.size() == 1;

        if (idx < node->children.size()) {
            auto child = node->children[idx].get();
            dfs.emplace_back(child, std::numeric_limits<size_t>::max());
            if (isPathNeeded) {
                if (path) {
                    path->append(1, '.');
                    *path += child->name;
                } else {
                    path = child->name;
                }
            }

            if (hasPreVisit) {
                // Invoke the pre-visit callback.
                if (!invokeVisitPathTreeNodesCallback(preVisit, child, getPath(), dfs)) {
                    dfsPop();
                }
            }
        } else {
            if (hasPostVisit && (invokeCallbacksForRootNode || !isRootNode)) {
                // Invoke the post-visit callback.
                invokeVisitPathTreeNodesCallback(postVisit, node, getPath(), dfs);
            }
            dfsPop();
        }
    }
}

/**
 * This function extracts the dependencies for expressions that appear inside a projection. Note
 * that this function only looks at ExpressionASTNodes in the projection and ignores all other kinds
 * of projection AST nodes.
 *
 * For example, for the projection {a: 1, b: "$c"}, this function will only extract the dependencies
 * needed by the expression "$c".
 */
void addProjectionExprDependencies(const projection_ast::Projection& projection, DepsTracker* deps);

/**
 * This method retrieves the values of the specified field paths ('fields') from 'resultSlot'
 * and stores the values into slots.
 *
 * This method returns a pair containing: (1) the updated SBE plan stage tree and; (2) a vector of
 * the slots ('outSlots') containing the field path values.
 *
 * The order of slots in 'outSlots' will match the order of field paths in 'fields'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, sbe::value::SlotVector> projectFieldsToSlots(
    std::unique_ptr<sbe::PlanStage> stage,
    const std::vector<std::string>& fields,
    sbe::value::SlotId resultSlot,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    StageBuilderState& state,
    const PlanStageSlots* slots = nullptr);

template <typename T>
inline StringData getTopLevelField(const T& path) {
    auto idx = path.find('.');
    return StringData(getRawStringData(path), idx != std::string::npos ? idx : path.size());
}

template <typename T>
inline std::vector<std::string> getTopLevelFields(const T& setOfPaths) {
    std::vector<std::string> topLevelFields;
    if (!setOfPaths.empty()) {
        StringSet topLevelFieldsSet;

        for (const auto& path : setOfPaths) {
            auto field = getTopLevelField(path);
            if (!topLevelFieldsSet.count(field)) {
                topLevelFields.emplace_back(std::string(field));
                topLevelFieldsSet.emplace(std::string(field));
            }
        }
    }

    return topLevelFields;
}

template <typename T, typename FuncT>
inline std::vector<T> filterVector(std::vector<T> vec, FuncT fn) {
    std::vector<T> result;
    std::copy_if(std::make_move_iterator(vec.begin()),
                 std::make_move_iterator(vec.end()),
                 std::back_inserter(result),
                 fn);
    return result;
}

template <typename T, typename FuncT>
inline std::pair<std::vector<T>, std::vector<T>> splitVector(std::vector<T> vec, FuncT fn) {
    std::pair<std::vector<T>, std::vector<T>> result;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (fn(vec[i])) {
            result.first.emplace_back(std::move(vec[i]));
        } else {
            result.second.emplace_back(std::move(vec[i]));
        }
    }
    return result;
}

template <typename T>
inline std::vector<T> appendVectorUnique(std::vector<T> lhs, std::vector<T> rhs) {
    if (!rhs.empty()) {
        auto valueSet = std::set<T>{lhs.begin(), lhs.end()};
        for (size_t i = 0; i < rhs.size(); ++i) {
            if (valueSet.emplace(rhs[i]).second) {
                lhs.emplace_back(std::move(rhs[i]));
            }
        }
    }
    return lhs;
}

}  // namespace mongo::stage_builder
