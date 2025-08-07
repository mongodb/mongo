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

#include "mongo/db/query/stage_builder/sbe/gen_expression.h"

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/util/pcre.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm_datetime.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/abt_defs.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stack>
#include <string>
#include <utility>
#include <vector>


namespace mongo::stage_builder {
namespace {
size_t kArgumentCountForBinaryTree = 100;

struct ExpressionVisitorContext {
    struct VarsFrame {
        VarsFrame(const std::vector<Variables::Id>& variableIds,
                  sbe::value::FrameIdGenerator* frameIdGenerator)
            : currentBindingIndex(0) {
            bindings.reserve(variableIds.size());
            for (const auto& variableId : variableIds) {
                bindings.push_back({variableId, frameIdGenerator->generate(), SbExpr{}});
            }
        }

        struct Binding {
            Variables::Id variableId;
            sbe::FrameId frameId;
            SbExpr expr;
        };

        std::vector<Binding> bindings;
        size_t currentBindingIndex;
    };

    ExpressionVisitorContext(StageBuilderState& state,
                             boost::optional<SbSlot> rootSlot,
                             const PlanStageSlots& slots)
        : state(state), rootSlot(std::move(rootSlot)), slots(&slots) {}

    void ensureArity(size_t arity) {
        invariant(exprStack.size() >= arity);
    }

    void pushExpr(SbExpr expr) {
        exprStack.emplace_back(std::move(expr));
    }

    SbExpr popExpr() {
        tassert(7261700, "tried to pop from empty SbExpr stack", !exprStack.empty());

        auto expr = std::move(exprStack.back());
        exprStack.pop_back();
        return expr;
    }

    SbExpr done() {
        tassert(6987501, "expected exactly one SbExpr on the stack", exprStack.size() == 1);
        return popExpr();
    }

    StageBuilderState& state;

    std::vector<SbExpr> exprStack;

    boost::optional<SbSlot> rootSlot;

    // The lexical environment for the expression being traversed. A variable reference takes the
    // form "$$variable_name" in MQL's concrete syntax and gets transformed into a numeric
    // identifier (Variables::Id) in the AST. During this translation, we directly translate any
    // such variable to an SBE frame id using this mapping.
    std::map<Variables::Id, sbe::FrameId> environment;
    std::stack<VarsFrame> varsFrameStack;

    const PlanStageSlots* slots;
};

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
SbExpr generateTraverseHelper(SbExpr inputExpr,
                              const FieldPath& fp,
                              size_t level,
                              StageBuilderState& state,
                              boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    using namespace std::literals;

    SbExprBuilder b(state);
    invariant(level < fp.getPathLength());

    tassert(6950802,
            "Expected an input expression or top level field",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldName = b.makeStrConstant(fp.getFieldName(level));
    auto fieldExpr = topLevelFieldSlot
        ? b.makeVariable(*topLevelFieldSlot)
        : b.makeFunction("getField"_sd, std::move(inputExpr), std::move(fieldName));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse stage.
        return fieldExpr;
    }

    // Generate nested traversal.
    auto lambdaFrameId = state.frameId();
    auto lambdaParam = b.makeVariable(lambdaFrameId, 0);

    auto resultExpr = generateTraverseHelper(std::move(lambdaParam), fp, level + 1, state);

    auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(resultExpr));

    // Generate the traverse stage for the current nested level.
    return b.makeFunction(
        "traverseP"_sd, std::move(fieldExpr), std::move(lambdaExpr), b.makeInt32Constant(1));
}

SbExpr generateTraverse(SbExpr inputExpr,
                        bool expectsDocumentInputOnly,
                        const FieldPath& fp,
                        StageBuilderState& state,
                        boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    SbExprBuilder b(state);
    size_t level = 0;

    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputExpr' will be a document and _not_ an array (such as
        // when accessing a field on the root document), we can generate a simpler expression.
        return generateTraverseHelper(std::move(inputExpr), fp, level, state, topLevelFieldSlot);
    } else {
        tassert(6950803, "Expected an input expression", !inputExpr.isNull());
        // The general case: the value in the 'inputExpr' may be an array that will require
        // traversal.
        auto lambdaFrameId = state.frameId();
        auto lambdaParam = b.makeVariable(lambdaFrameId, 0);

        auto resultExpr = generateTraverseHelper(std::move(lambdaParam), fp, level, state);

        auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(resultExpr));

        return b.makeFunction(
            "traverseP"_sd, std::move(inputExpr), std::move(lambdaExpr), b.makeInt32Constant(1));
    }
}

/**
 * Generates an EExpression that converts the input to upper or lower case.
 */
void generateStringCaseConversionExpression(ExpressionVisitorContext* context,
                                            const std::string& caseConversionFunction) {
    SbExprBuilder b(context->state);

    uint32_t typeMask = (getBSONTypeMask(sbe::value::TypeTags::StringSmall) |
                         getBSONTypeMask(sbe::value::TypeTags::StringBig) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonString) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonSymbol) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDecimal) |
                         getBSONTypeMask(sbe::value::TypeTags::Date) |
                         getBSONTypeMask(sbe::value::TypeTags::Timestamp));

    auto binds = SbExpr::makeSeq(context->popExpr());
    auto frameId = context->state.frameId();
    SbVar var{frameId, 0};

    auto totalCaseConversionExpr = b.buildMultiBranchConditionalFromCaseValuePairs(
        SbExpr::makeExprPairVector(
            SbExprPair{b.generateNullMissingOrUndefined(var), b.makeStrConstant(""_sd)},
            SbExprPair{
                b.makeFunction("typeMatch"_sd, var, b.makeInt32Constant(typeMask)),
                b.makeFunction(caseConversionFunction, b.makeFunction("coerceToString"_sd, var))}),
        b.makeFail(ErrorCodes::Error{7158200},
                   str::stream() << "$" << caseConversionFunction
                                 << " input type is not supported"));

    context->pushExpr(b.makeLet(frameId, std::move(binds), std::move(totalCaseConversionExpr)));
}

class ExpressionPreVisitor final : public ExpressionConstVisitor {
public:
    ExpressionPreVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(const ExpressionConstant* expr) final {}
    void visit(const ExpressionAbs* expr) final {}
    void visit(const ExpressionAdd* expr) final {}
    void visit(const ExpressionAllElementsTrue* expr) final {}
    void visit(const ExpressionAnd* expr) final {}
    void visit(const ExpressionAnyElementTrue* expr) final {}
    void visit(const ExpressionArray* expr) final {}
    void visit(const ExpressionArrayElemAt* expr) final {}
    void visit(const ExpressionBitAnd* expr) final {}
    void visit(const ExpressionBitOr* expr) final {}
    void visit(const ExpressionBitXor* expr) final {}
    void visit(const ExpressionBitNot* expr) final {}
    void visit(const ExpressionFirst* expr) final {}
    void visit(const ExpressionLast* expr) final {}
    void visit(const ExpressionObjectToArray* expr) final {}
    void visit(const ExpressionArrayToObject* expr) final {}
    void visit(const ExpressionBsonSize* expr) final {}
    void visit(const ExpressionCeil* expr) final {}
    void visit(const ExpressionCompare* expr) final {}
    void visit(const ExpressionConcat* expr) final {}
    void visit(const ExpressionConcatArrays* expr) final {}
    void visit(const ExpressionCond* expr) final {}
    void visit(const ExpressionDateDiff* expr) final {}
    void visit(const ExpressionDateFromString* expr) final {}
    void visit(const ExpressionDateFromParts* expr) final {}
    void visit(const ExpressionDateToParts* expr) final {}
    void visit(const ExpressionDateToString* expr) final {}
    void visit(const ExpressionDateTrunc* expr) final {}
    void visit(const ExpressionDivide* expr) final {}
    void visit(const ExpressionExp* expr) final {}
    void visit(const ExpressionFieldPath* expr) final {}
    void visit(const ExpressionFilter* expr) final {}
    void visit(const ExpressionFloor* expr) final {}
    void visit(const ExpressionIfNull* expr) final {}
    void visit(const ExpressionIn* expr) final {}
    void visit(const ExpressionIndexOfArray* expr) final {}
    void visit(const ExpressionIndexOfBytes* expr) final {}
    void visit(const ExpressionIndexOfCP* expr) final {}
    void visit(const ExpressionIsNumber* expr) final {}
    void visit(const ExpressionLet* expr) final {
        _context->varsFrameStack.push(ExpressionVisitorContext::VarsFrame{
            expr->getOrderedVariableIds(), _context->state.frameIdGenerator});
    }
    void visit(const ExpressionLn* expr) final {}
    void visit(const ExpressionLog* expr) final {}
    void visit(const ExpressionLog10* expr) final {}
    void visit(const ExpressionInternalFLEBetween* expr) final {}
    void visit(const ExpressionInternalFLEEqual* expr) final {}
    void visit(const ExpressionEncStrStartsWith* expr) final {}
    void visit(const ExpressionEncStrEndsWith* expr) final {}
    void visit(const ExpressionEncStrContains* expr) final {}
    void visit(const ExpressionEncStrNormalizedEq* expr) final {}
    void visit(const ExpressionInternalRawSortKey* expr) final {}
    void visit(const ExpressionMap* expr) final {}
    void visit(const ExpressionMeta* expr) final {}
    void visit(const ExpressionMod* expr) final {}
    void visit(const ExpressionMultiply* expr) final {}
    void visit(const ExpressionNot* expr) final {}
    void visit(const ExpressionObject* expr) final {}
    void visit(const ExpressionOr* expr) final {}
    void visit(const ExpressionPow* expr) final {}
    void visit(const ExpressionRange* expr) final {}
    void visit(const ExpressionReduce* expr) final {}
    void visit(const ExpressionReplaceOne* expr) final {}
    void visit(const ExpressionReplaceAll* expr) final {}
    void visit(const ExpressionSetDifference* expr) final {}
    void visit(const ExpressionSetEquals* expr) final {}
    void visit(const ExpressionSetIntersection* expr) final {}
    void visit(const ExpressionSetIsSubset* expr) final {}
    void visit(const ExpressionSetUnion* expr) final {}
    void visit(const ExpressionSimilarityDotProduct* expr) final {}
    void visit(const ExpressionSimilarityCosine* expr) final {}
    void visit(const ExpressionSimilarityEuclidean* expr) final {}
    void visit(const ExpressionSize* expr) final {}
    void visit(const ExpressionReverseArray* expr) final {}
    void visit(const ExpressionSortArray* expr) final {}
    void visit(const ExpressionSlice* expr) final {}
    void visit(const ExpressionIsArray* expr) final {}
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {}
    void visit(const ExpressionRound* expr) final {}
    void visit(const ExpressionSplit* expr) final {}
    void visit(const ExpressionSqrt* expr) final {}
    void visit(const ExpressionStrcasecmp* expr) final {}
    void visit(const ExpressionSubstrBytes* expr) final {}
    void visit(const ExpressionSubstrCP* expr) final {}
    void visit(const ExpressionStrLenBytes* expr) final {}
    void visit(const ExpressionBinarySize* expr) final {}
    void visit(const ExpressionStrLenCP* expr) final {}
    void visit(const ExpressionSubtract* expr) final {}
    void visit(const ExpressionSwitch* expr) final {}
    void visit(const ExpressionTestApiVersion* expr) final {}
    void visit(const ExpressionToLower* expr) final {}
    void visit(const ExpressionToUpper* expr) final {}
    void visit(const ExpressionTrim* expr) final {}
    void visit(const ExpressionTrunc* expr) final {}
    void visit(const ExpressionType* expr) final {}
    void visit(const ExpressionSubtype* expr) final {}
    void visit(const ExpressionZip* expr) final {}
    void visit(const ExpressionConvert* expr) final {}
    void visit(const ExpressionRegexFind* expr) final {}
    void visit(const ExpressionRegexFindAll* expr) final {}
    void visit(const ExpressionRegexMatch* expr) final {}
    void visit(const ExpressionCosine* expr) final {}
    void visit(const ExpressionSine* expr) final {}
    void visit(const ExpressionTangent* expr) final {}
    void visit(const ExpressionArcCosine* expr) final {}
    void visit(const ExpressionArcSine* expr) final {}
    void visit(const ExpressionArcTangent* expr) final {}
    void visit(const ExpressionArcTangent2* expr) final {}
    void visit(const ExpressionHyperbolicArcTangent* expr) final {}
    void visit(const ExpressionHyperbolicArcCosine* expr) final {}
    void visit(const ExpressionHyperbolicArcSine* expr) final {}
    void visit(const ExpressionHyperbolicTangent* expr) final {}
    void visit(const ExpressionHyperbolicCosine* expr) final {}
    void visit(const ExpressionHyperbolicSine* expr) final {}
    void visit(const ExpressionDegreesToRadians* expr) final {}
    void visit(const ExpressionRadiansToDegrees* expr) final {}
    void visit(const ExpressionDayOfMonth* expr) final {}
    void visit(const ExpressionDayOfWeek* expr) final {}
    void visit(const ExpressionDayOfYear* expr) final {}
    void visit(const ExpressionHour* expr) final {}
    void visit(const ExpressionMillisecond* expr) final {}
    void visit(const ExpressionMinute* expr) final {}
    void visit(const ExpressionMonth* expr) final {}
    void visit(const ExpressionSecond* expr) final {}
    void visit(const ExpressionWeek* expr) final {}
    void visit(const ExpressionIsoWeekYear* expr) final {}
    void visit(const ExpressionIsoDayOfWeek* expr) final {}
    void visit(const ExpressionIsoWeek* expr) final {}
    void visit(const ExpressionYear* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(const ExpressionTests::Testable* expr) final {}
    void visit(const ExpressionInternalJsEmit* expr) final {}
    void visit(const ExpressionInternalFindSlice* expr) final {}
    void visit(const ExpressionInternalFindPositional* expr) final {}
    void visit(const ExpressionInternalFindElemMatch* expr) final {}
    void visit(const ExpressionFunction* expr) final {}
    void visit(const ExpressionRandom* expr) final {}
    void visit(const ExpressionCurrentDate* expr) final {}
    void visit(const ExpressionToHashedIndexKey* expr) final {}
    void visit(const ExpressionDateAdd* expr) final {}
    void visit(const ExpressionDateSubtract* expr) final {}
    void visit(const ExpressionGetField* expr) final {}
    void visit(const ExpressionSetField* expr) final {}
    void visit(const ExpressionTsSecond* expr) final {}
    void visit(const ExpressionTsIncrement* expr) final {}
    void visit(const ExpressionInternalOwningShard* expr) final {}
    void visit(const ExpressionInternalIndexKey* expr) final {}
    void visit(const ExpressionInternalKeyStringValue* expr) final {}
    void visit(const ExpressionCreateUUID* expr) final {}
    void visit(const ExpressionCreateObjectId* expr) final {}
    void visit(const ExpressionTestFeatureFlagLatest* expr) final {}
    void visit(const ExpressionTestFeatureFlagLastLTS* expr) final {}

private:
    ExpressionVisitorContext* _context;
};

class ExpressionInVisitor final : public ExpressionConstVisitor {
public:
    ExpressionInVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(const ExpressionConstant* expr) final {}
    void visit(const ExpressionAbs* expr) final {}
    void visit(const ExpressionAdd* expr) final {}
    void visit(const ExpressionAllElementsTrue* expr) final {}
    void visit(const ExpressionAnd* expr) final {}
    void visit(const ExpressionAnyElementTrue* expr) final {}
    void visit(const ExpressionArray* expr) final {}
    void visit(const ExpressionArrayElemAt* expr) final {}
    void visit(const ExpressionBitAnd* expr) final {}
    void visit(const ExpressionBitOr* expr) final {}
    void visit(const ExpressionBitXor* expr) final {}
    void visit(const ExpressionBitNot* expr) final {}
    void visit(const ExpressionFirst* expr) final {}
    void visit(const ExpressionLast* expr) final {}
    void visit(const ExpressionObjectToArray* expr) final {}
    void visit(const ExpressionArrayToObject* expr) final {}
    void visit(const ExpressionBsonSize* expr) final {}
    void visit(const ExpressionCeil* expr) final {}
    void visit(const ExpressionCompare* expr) final {}
    void visit(const ExpressionConcat* expr) final {}
    void visit(const ExpressionConcatArrays* expr) final {}
    void visit(const ExpressionCond* expr) final {}
    void visit(const ExpressionDateDiff* expr) final {}
    void visit(const ExpressionDateFromString* expr) final {}
    void visit(const ExpressionDateFromParts* expr) final {}
    void visit(const ExpressionDateToParts* expr) final {}
    void visit(const ExpressionDateToString* expr) final {}
    void visit(const ExpressionDateTrunc*) final {}
    void visit(const ExpressionDivide* expr) final {}
    void visit(const ExpressionExp* expr) final {}
    void visit(const ExpressionFieldPath* expr) final {}
    void visit(const ExpressionFilter* expr) final {}
    void visit(const ExpressionFloor* expr) final {}
    void visit(const ExpressionIfNull* expr) final {}
    void visit(const ExpressionIn* expr) final {}
    void visit(const ExpressionIndexOfArray* expr) final {}
    void visit(const ExpressionIndexOfBytes* expr) final {}
    void visit(const ExpressionIndexOfCP* expr) final {}
    void visit(const ExpressionIsNumber* expr) final {}
    void visit(const ExpressionLet* expr) final {
        // This visitor fires after each variable definition in a $let expression. The top of the
        // _context's expression stack will be an expression defining the variable initializer. We
        // use a separate frame stack ('varsFrameStack') to keep track of which variable we are
        // visiting, so we can appropriately bind the initializer.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();
        size_t& currentBindingIndex = currentFrame.currentBindingIndex;
        invariant(currentBindingIndex < currentFrame.bindings.size());

        auto& currentBinding = currentFrame.bindings[currentBindingIndex++];
        currentBinding.expr = _context->popExpr();

        // Second, we bind this variables AST-level name (with type Variable::Id) to the frame that
        // will be used for compilation and execution. Once this "stage builder" finishes, these
        // Variable::Id bindings will no longer be relevant.
        invariant(_context->environment.find(currentBinding.variableId) ==
                  _context->environment.end());
        _context->environment.emplace(currentBinding.variableId, currentBinding.frameId);
    }
    void visit(const ExpressionLn* expr) final {}
    void visit(const ExpressionLog* expr) final {}
    void visit(const ExpressionLog10* expr) final {}
    void visit(const ExpressionInternalFLEBetween* expr) final {}
    void visit(const ExpressionInternalFLEEqual* expr) final {}
    void visit(const ExpressionEncStrStartsWith* expr) final {}
    void visit(const ExpressionEncStrEndsWith* expr) final {}
    void visit(const ExpressionEncStrContains* expr) final {}
    void visit(const ExpressionEncStrNormalizedEq* expr) final {}
    void visit(const ExpressionInternalRawSortKey* expr) final {}
    void visit(const ExpressionMap* expr) final {}
    void visit(const ExpressionMeta* expr) final {}
    void visit(const ExpressionMod* expr) final {}
    void visit(const ExpressionMultiply* expr) final {}
    void visit(const ExpressionNot* expr) final {}
    void visit(const ExpressionObject* expr) final {}
    void visit(const ExpressionOr* expr) final {}
    void visit(const ExpressionPow* expr) final {}
    void visit(const ExpressionRange* expr) final {}
    void visit(const ExpressionReduce* expr) final {}
    void visit(const ExpressionReplaceOne* expr) final {}
    void visit(const ExpressionReplaceAll* expr) final {}
    void visit(const ExpressionSetDifference* expr) final {}
    void visit(const ExpressionSetEquals* expr) final {}
    void visit(const ExpressionSetIntersection* expr) final {}
    void visit(const ExpressionSetIsSubset* expr) final {}
    void visit(const ExpressionSetUnion* expr) final {}
    void visit(const ExpressionSimilarityDotProduct* expr) final {}
    void visit(const ExpressionSimilarityCosine* expr) final {}
    void visit(const ExpressionSimilarityEuclidean* expr) final {}
    void visit(const ExpressionSize* expr) final {}
    void visit(const ExpressionReverseArray* expr) final {}
    void visit(const ExpressionSortArray* expr) final {}
    void visit(const ExpressionSlice* expr) final {}
    void visit(const ExpressionIsArray* expr) final {}
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {}
    void visit(const ExpressionRound* expr) final {}
    void visit(const ExpressionSplit* expr) final {}
    void visit(const ExpressionSqrt* expr) final {}
    void visit(const ExpressionStrcasecmp* expr) final {}
    void visit(const ExpressionSubstrBytes* expr) final {}
    void visit(const ExpressionSubstrCP* expr) final {}
    void visit(const ExpressionStrLenBytes* expr) final {}
    void visit(const ExpressionBinarySize* expr) final {}
    void visit(const ExpressionStrLenCP* expr) final {}
    void visit(const ExpressionSubtract* expr) final {}
    void visit(const ExpressionSwitch* expr) final {}
    void visit(const ExpressionTestApiVersion* expr) final {}
    void visit(const ExpressionToLower* expr) final {}
    void visit(const ExpressionToUpper* expr) final {}
    void visit(const ExpressionTrim* expr) final {}
    void visit(const ExpressionTrunc* expr) final {}
    void visit(const ExpressionType* expr) final {}
    void visit(const ExpressionSubtype* expr) final {}
    void visit(const ExpressionZip* expr) final {}
    void visit(const ExpressionConvert* expr) final {}
    void visit(const ExpressionRegexFind* expr) final {}
    void visit(const ExpressionRegexFindAll* expr) final {}
    void visit(const ExpressionRegexMatch* expr) final {}
    void visit(const ExpressionCosine* expr) final {}
    void visit(const ExpressionSine* expr) final {}
    void visit(const ExpressionTangent* expr) final {}
    void visit(const ExpressionArcCosine* expr) final {}
    void visit(const ExpressionArcSine* expr) final {}
    void visit(const ExpressionArcTangent* expr) final {}
    void visit(const ExpressionArcTangent2* expr) final {}
    void visit(const ExpressionHyperbolicArcTangent* expr) final {}
    void visit(const ExpressionHyperbolicArcCosine* expr) final {}
    void visit(const ExpressionHyperbolicArcSine* expr) final {}
    void visit(const ExpressionHyperbolicTangent* expr) final {}
    void visit(const ExpressionHyperbolicCosine* expr) final {}
    void visit(const ExpressionHyperbolicSine* expr) final {}
    void visit(const ExpressionDegreesToRadians* expr) final {}
    void visit(const ExpressionRadiansToDegrees* expr) final {}
    void visit(const ExpressionDayOfMonth* expr) final {}
    void visit(const ExpressionDayOfWeek* expr) final {}
    void visit(const ExpressionDayOfYear* expr) final {}
    void visit(const ExpressionHour* expr) final {}
    void visit(const ExpressionMillisecond* expr) final {}
    void visit(const ExpressionMinute* expr) final {}
    void visit(const ExpressionMonth* expr) final {}
    void visit(const ExpressionSecond* expr) final {}
    void visit(const ExpressionWeek* expr) final {}
    void visit(const ExpressionIsoWeekYear* expr) final {}
    void visit(const ExpressionIsoDayOfWeek* expr) final {}
    void visit(const ExpressionIsoWeek* expr) final {}
    void visit(const ExpressionYear* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(const ExpressionTests::Testable* expr) final {}
    void visit(const ExpressionInternalJsEmit* expr) final {}
    void visit(const ExpressionInternalFindSlice* expr) final {}
    void visit(const ExpressionInternalFindPositional* expr) final {}
    void visit(const ExpressionInternalFindElemMatch* expr) final {}
    void visit(const ExpressionFunction* expr) final {}
    void visit(const ExpressionRandom* expr) final {}
    void visit(const ExpressionCurrentDate* expr) final {}
    void visit(const ExpressionToHashedIndexKey* expr) final {}
    void visit(const ExpressionDateAdd* expr) final {}
    void visit(const ExpressionDateSubtract* expr) final {}
    void visit(const ExpressionGetField* expr) final {}
    void visit(const ExpressionSetField* expr) final {}
    void visit(const ExpressionTsSecond* expr) final {}
    void visit(const ExpressionTsIncrement* expr) final {}
    void visit(const ExpressionInternalOwningShard* expr) final {}
    void visit(const ExpressionInternalIndexKey* expr) final {}
    void visit(const ExpressionInternalKeyStringValue* expr) final {}
    void visit(const ExpressionCreateUUID* expr) final {}
    void visit(const ExpressionCreateObjectId* expr) final {}
    void visit(const ExpressionTestFeatureFlagLatest* expr) final {}
    void visit(const ExpressionTestFeatureFlagLastLTS* expr) final {}

private:
    ExpressionVisitorContext* _context;
};


struct DoubleBound {
    DoubleBound(double b, bool isInclusive) : bound(b), inclusive(isInclusive) {}

    static DoubleBound minInfinity() {
        return DoubleBound(-std::numeric_limits<double>::infinity(), false);
    }
    static DoubleBound plusInfinity() {
        return DoubleBound(std::numeric_limits<double>::infinity(), false);
    }
    static DoubleBound plusInfinityInclusive() {
        return DoubleBound(std::numeric_limits<double>::infinity(), true);
    }
    std::string printLowerBound() const {
        return str::stream() << (inclusive ? "[" : "(") << bound;
    }
    std::string printUpperBound() const {
        return str::stream() << bound << (inclusive ? "]" : ")");
    }
    double bound;
    bool inclusive;
};

class ExpressionPostVisitor final : public ExpressionConstVisitor {
public:
    ExpressionPostVisitor(ExpressionVisitorContext* context)
        : _context{context}, _b{context->state} {}

    enum class SetOperation {
        Difference,
        Intersection,
        Union,
        Equals,
        IsSubset,
    };

    void visit(const ExpressionConstant* expr) final {
        auto [tag, val] = sbe::value::makeValue(expr->getValue());
        pushExpr(_b.makeConstant(tag, val));
    }

    void visit(const ExpressionAbs* expr) final {
        auto frameId = _context->state.frameId();
        SbVar input{frameId, 0};

        auto absExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(input), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(input),
                    _b.makeFail(ErrorCodes::Error{7157700}, "$abs only supports numeric types")},
                SbExprPair{
                    _b.generateLongLongMinCheck(input),
                    _b.makeFail(ErrorCodes::Error{7157701}, "can't take $abs of long long min")}),
            _b.makeFunction("abs", input));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(absExpr)));
    }

    void visit(const ExpressionAdd* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Build a linear tree for a small number of children so that we can pre-validate all
        // arguments.
        if (arity < kArgumentCountForBinaryTree ||
            feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            visitFast(expr);
            return;
        }

        auto checkLeaf = [&](SbExpr arg) {
            SbExpr::Vector binds;
            binds.emplace_back(std::move(arg));

            auto frameId = _context->state.frameId();
            SbVar var{frameId, 0};

            auto checkedLeaf = _b.buildMultiBranchConditional(
                SbExprPair{_b.makeBinaryOp(abt::Operations::Or,
                                           _b.makeFunction("isNumber", var),
                                           _b.makeFunction("isDate", var)),
                           var},
                _b.makeFail(ErrorCodes::Error{7315401},
                            "only numbers and dates are allowed in an $add expression"));

            return _b.makeLet(frameId, std::move(binds), std::move(checkedLeaf));
        };

        auto combineTwoTree = [&](SbExpr left, SbExpr right) {
            SbExpr::Vector binds;
            binds.emplace_back(std::move(left));
            binds.emplace_back(std::move(right));

            auto frameId = _context->state.frameId();
            SbVar varLeft{frameId, 0};
            SbVar varRight{frameId, 1};

            auto addExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.makeBinaryOp(abt::Operations::Or,
                                               _b.generateNullMissingOrUndefined(varLeft),
                                               _b.generateNullMissingOrUndefined(varRight)),
                               _b.makeNullConstant()},
                    SbExprPair{_b.makeBinaryOp(abt::Operations::And,
                                               _b.makeFunction("isDate", varLeft),
                                               _b.makeFunction("isDate", varRight)),
                               _b.makeFail(ErrorCodes::Error{7315402},
                                           "only one date allowed in an $add expression")}),
                _b.makeBinaryOp(abt::Operations::Add, varLeft, varRight));

            return _b.makeLet(frameId, std::move(binds), std::move(addExpr));
        };

        SbExpr::Vector leaves;
        leaves.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            leaves.emplace_back(checkLeaf(popExpr()));
        }
        std::reverse(std::begin(leaves), std::end(leaves));

        pushExpr(SbExpr::makeBalancedTree(combineTwoTree, std::move(leaves)));
    }

    void visitFast(const ExpressionAdd* expr) {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            // Return a zero constant if the expression has no operand children.
            pushExpr(_b.makeInt32Constant(0));
        } else {
            SbExpr::Vector binds;
            SbExpr::Vector variables;
            SbExpr::Vector checkArgIsNull;
            SbExpr::Vector checkArgHasValidType;
            binds.reserve(arity);
            variables.reserve(arity);
            checkArgIsNull.reserve(arity);
            checkArgHasValidType.reserve(arity);

            auto frameId = _context->state.frameId();
            sbe::value::SlotId numLocalVars = 0;

            for (size_t idx = 0; idx < arity; ++idx) {
                binds.push_back(popExpr());
                sbe::value::SlotId localOffset = numLocalVars++;
                SbVar var{frameId, localOffset};

                // Count the number of dates among children of this $add while verifying the types
                // so that we can later check that we have at most one date.
                checkArgHasValidType.emplace_back(_b.buildMultiBranchConditionalFromCaseValuePairs(
                    SbExpr::makeExprPairVector(
                        SbExprPair{_b.makeFunction("isNumber", var), _b.makeInt32Constant(0)},
                        SbExprPair{_b.makeFunction("isDate", var), _b.makeInt32Constant(1)}),
                    _b.makeFail(ErrorCodes::Error{7157723},
                                "only numbers and dates are allowed in an $add expression")));

                checkArgIsNull.push_back(_b.generateNullMissingOrUndefined(var));
                variables.emplace_back(var);
            }

            // At this point 'binds' vector contains arguments of $add expression in the reversed
            // order. We need to reverse it back to perform summation in the right order below.
            // Summation in different order can lead to different result because of accumulated
            // precision errors from floating point types.
            std::reverse(std::begin(binds), std::end(binds));

            auto checkNullAllArguments =
                _b.makeBooleanOpTree(abt::Operations::Or, std::move(checkArgIsNull));

            auto checkValidTypeAndCountDates =
                _b.makeNaryOp(abt::Operations::Add, std::move(checkArgHasValidType));

            auto addOp = _b.makeNaryOp(abt::Operations::Add, std::move(variables));

            auto addExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{std::move(checkNullAllArguments), _b.makeNullConstant()},
                    SbExprPair{_b.makeBinaryOp(abt::Operations::Gt,
                                               std::move(checkValidTypeAndCountDates),
                                               _b.makeInt32Constant(1)),
                               _b.makeFail(ErrorCodes::Error{7157722},
                                           "only one date allowed in an $add expression")}),
                std::move(addOp));

            addExpr = _b.makeLet(frameId, std::move(binds), std::move(addExpr));
            pushExpr(std::move(addExpr));
        }
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, abt::Operations::And);
    }
    void visit(const ExpressionAnyElementTrue* expr) final {
        auto binds = SbExpr::makeSeq(popExpr());
        auto frameId = _context->state.frameId();
        SbVar var{frameId, 0};

        auto lambdaFrameId = _context->state.frameId();
        SbVar lambdaVar{lambdaFrameId, 0};
        auto lambdaBody = _b.makeFillEmptyFalse(_b.makeFunction("coerceToBool", lambdaVar));
        auto lambdaExpr = _b.makeLocalLambda(lambdaFrameId, std::move(lambdaBody));

        auto resultExpr = _b.makeIf(
            _b.makeFillEmptyFalse(_b.makeFunction("isArray", var)),
            _b.makeFunction("traverseF", var, std::move(lambdaExpr), _b.makeBoolConstant(false)),
            _b.makeFail(ErrorCodes::Error{7158300}, "$anyElementTrue's argument must be an array"));

        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(resultExpr)));
    }

    void visit(const ExpressionArray* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            auto [emptyArrTag, emptyArrVal] = sbe::value::makeNewArray();
            pushExpr(_b.makeConstant(emptyArrTag, emptyArrVal));
            return;
        }

        SbExpr::Vector binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            binds.emplace_back(popExpr());
        }
        std::reverse(std::begin(binds), std::end(binds));

        auto frameId = _context->state.frameId();
        SbExpr::Vector args;

        sbe::value::SlotId numLocalVars = 0;
        for (size_t idx = 0; idx < arity; ++idx) {
            args.push_back(_b.makeFillEmptyNull(SbVar{frameId, numLocalVars++}));
        }

        auto arrayExpr = _b.makeFunction("newArray", std::move(args));

        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(arrayExpr)));
    }
    void visit(const ExpressionArrayElemAt* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitAnd* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitOr* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitXor* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitNot* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFirst* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionLast* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionObjectToArray* expr) final {
        auto binds = SbExpr::makeSeq(popExpr());
        auto frameId = _context->state.frameId();
        SbVar arg{frameId, 0};

        // Create an expression to invoke the built-in function.
        binds.emplace_back(_b.makeFunction("objectToArray"_sd, arg));
        SbVar func{frameId, 1};

        // Create validation checks when builtin returns nothing
        auto validationExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.makeFunction("exists"_sd, func), func},
                SbExprPair{_b.generateNullMissingOrUndefined(arg), _b.makeNullConstant()},
                SbExprPair{_b.generateNonObjectCheck(arg),
                           _b.makeFail(ErrorCodes::Error{5153215},
                                       "$objectToArray requires an object input")}),
            _b.makeNothingConstant());

        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(validationExpr)));
    }
    void visit(const ExpressionArrayToObject* expr) final {
        auto binds = SbExpr::makeSeq(popExpr());
        auto frameId = _context->state.frameId();
        SbVar arg{frameId, 0};

        // Create an expression to invoke the built-in function.
        binds.emplace_back(_b.makeFunction("arrayToObject"_sd, arg));
        SbVar func{frameId, 1};

        // Create validation checks when builtin returns nothing
        auto validationExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.makeFunction("exists"_sd, func), func},
                SbExprPair{_b.generateNullMissingOrUndefined(arg), _b.makeNullConstant()},
                SbExprPair{_b.generateNonArrayCheck(arg),
                           _b.makeFail(ErrorCodes::Error{5153200},
                                       "$arrayToObject requires an array input")}),
            _b.makeNothingConstant());

        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(validationExpr)));
    }
    void visit(const ExpressionBsonSize* expr) final {
        // Build an expression which evaluates the size of a BSON document and validates the input
        // argument.
        // 1. If the argument is null or empty, return null.
        // 2. Else, if the argument is a BSON document, return its size.
        // 3. Else, raise an error.
        auto binds = SbExpr::makeSeq(popExpr());
        auto frameId = _context->state.frameId();
        SbVar arg{frameId, 0};

        auto bsonSizeExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(arg), _b.makeNullConstant()},
                SbExprPair{_b.generateNonObjectCheck(arg),
                           _b.makeFail(ErrorCodes::Error{7158301},
                                       "$bsonSize requires a document input")}),
            _b.makeFunction("bsonSize", arg));

        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(bsonSizeExpr)));
    }

    void visit(const ExpressionCeil* expr) final {
        auto frameId = _context->state.frameId();
        SbVar input{frameId, 0};

        auto ceilExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(input), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(input),
                    _b.makeFail(ErrorCodes::Error{7157702}, "$ceil only supports numeric types")}),
            _b.makeFunction("ceil", input));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(ceilExpr)));
    }
    void visit(const ExpressionCompare* expr) final {
        _context->ensureArity(2);

        auto rhs = popExpr();
        auto lhs = popExpr();

        pushExpr(generateExpressionCompare(
            _context->state, expr->getOp(), std::move(lhs), std::move(rhs)));
    }

    void visit(const ExpressionConcat* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Concatenation of no strings is an empty string.
        if (arity == 0) {
            pushExpr(_b.makeStrConstant(""_sd));
            return;
        }

        SbExpr::Vector binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            binds.emplace_back(popExpr());
        }
        std::reverse(std::begin(binds), std::end(binds));

        auto frameId = _context->state.frameId();

        SbExpr::Vector checkNullArg;
        SbExpr::Vector checkStringArg;
        SbExpr::Vector args;

        sbe::value::SlotId numLocalVars = 0;
        for (size_t idx = 0; idx < arity; ++idx) {
            SbVar var{frameId, numLocalVars++};

            checkNullArg.push_back(_b.generateNullMissingOrUndefined(var));
            checkStringArg.push_back(_b.makeFunction("isString"_sd, var));
            args.emplace_back(SbExpr{var});
        }

        auto checkNullAnyArgument =
            _b.makeBooleanOpTree(abt::Operations::Or, std::move(checkNullArg));

        auto checkStringAllArguments =
            _b.makeBooleanOpTree(abt::Operations::And, std::move(checkStringArg));

        auto concatExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{std::move(checkNullAnyArgument), _b.makeNullConstant()},
                SbExprPair{std::move(checkStringAllArguments),
                           _b.makeFunction("concat", std::move(args))}),
            _b.makeFail(ErrorCodes::Error{7158201}, "$concat supports only strings"));

        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(concatExpr)));
    }

    void visit(const ExpressionConcatArrays* expr) final {
        auto numChildren = expr->getChildren().size();
        _context->ensureArity(numChildren);

        // If there are no children, return an empty array.
        if (numChildren == 0) {
            auto [emptyArrTag, emptyArrVal] = sbe::value::makeNewArray();
            pushExpr(_b.makeConstant(emptyArrTag, emptyArrVal));
            return;
        }

        SbExpr::Vector binds;
        binds.reserve(numChildren + 1);

        for (size_t i = 0; i < numChildren; ++i) {
            binds.emplace_back(popExpr());
        }
        std::reverse(binds.begin(), binds.end());

        SbExpr::Vector argIsNullOrMissing;
        SbExpr::Vector args;
        argIsNullOrMissing.reserve(numChildren);
        args.reserve(numChildren);

        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        for (size_t i = 0; i < numChildren; ++i) {
            SbVar var{frameId, numLocalVars++};

            args.emplace_back(SbExpr{var});
            argIsNullOrMissing.emplace_back(_b.generateNullMissingOrUndefined(var));
        }

        binds.emplace_back(_b.makeFunction("concatArrays", std::move(args)));
        SbVar result{frameId, numLocalVars++};

        pushExpr(_b.makeLet(
            frameId,
            std::move(binds),
            _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.makeFunction("exists"_sd, result), result},
                    SbExprPair{
                        _b.makeBooleanOpTree(abt::Operations::Or, std::move(argIsNullOrMissing)),
                        _b.makeNullConstant()}),
                _b.makeFail(ErrorCodes::Error{7158000}, "$concatArrays only supports arrays"))));
    }

    void visit(const ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionDateDiff* expr) final {
        using namespace std::literals;

        const auto& children = expr->getChildren();
        invariant(children.size() == 5);

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};

        // Get child expressions.
        SbExpr startOfWeekExpression;
        if (expr->isStartOfWeekSpecified()) {
            startOfWeekExpression = popExpr();
        }

        auto timezoneExpression =
            expr->isTimezoneSpecified() ? popExpr() : _b.makeStrConstant("UTC"_sd);
        auto unitExpression = popExpr();
        auto endDateExpression = popExpr();
        auto startDateExpression = popExpr();

        // Set up the frame and the 'bindings' vector.
        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        SbVar startDateVar{frameId, numLocalVars++};
        SbVar endDateVar{frameId, numLocalVars++};
        SbVar unitVar{frameId, numLocalVars++};
        SbVar timezoneVar{frameId, numLocalVars++};

        // These SbVars will be populated only if the "startOfWeek" parameter was specified.
        // 'unitIsWeekVar' is an auxiliary boolean variable to hold a value of a common
        // subexpression 'unit'=="week" (string).
        boost::optional<SbVar> startOfWeekVar;
        boost::optional<SbVar> unitIsWeekVar;

        SbExpr::Vector bindings;
        bindings.push_back(std::move(startDateExpression));
        bindings.push_back(std::move(endDateExpression));
        bindings.push_back(std::move(unitExpression));
        bindings.push_back(std::move(timezoneExpression));

        if (expr->isStartOfWeekSpecified()) {
            startOfWeekVar = SbVar{frameId, numLocalVars++};
            bindings.push_back(std::move(startOfWeekExpression));

            unitIsWeekVar = SbVar{frameId, numLocalVars++};
            bindings.push_back(generateIsEqualToStringCheck(unitVar, "week"_sd));
        }

        // Set parameters for an invocation of built-in "dateDiff" function.
        SbExpr::Vector arguments;
        arguments.emplace_back(timeZoneDBVar);
        arguments.emplace_back(startDateVar);
        arguments.emplace_back(endDateVar);
        arguments.emplace_back(unitVar);
        arguments.emplace_back(timezoneVar);

        if (expr->isStartOfWeekSpecified()) {
            // Parameter "startOfWeek" - if the time unit is the week, then pass value of parameter
            // "startOfWeek" of "$dateDiff" expression, otherwise pass a valid default value, since
            // "dateDiff" built-in function does not accept non-string type values for this
            // parameter.
            arguments.emplace_back(
                _b.makeIf(*unitIsWeekVar, *startOfWeekVar, _b.makeStrConstant("sun"_sd)));
        }

        // Create an expression to invoke built-in "dateDiff" function.
        auto dateDiffFunctionCall = _b.makeFunction("dateDiff", std::move(arguments));

        // Create expressions to check that each argument to "dateDiff" function exists, is not
        // null, and is of the correct type.
        std::vector<SbExprPair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.emplace_back(generateReturnNullIfNullMissingOrUndefined(startDateVar));
        inputValidationCases.emplace_back(generateReturnNullIfNullMissingOrUndefined(endDateVar));
        inputValidationCases.emplace_back(generateReturnNullIfNullMissingOrUndefined(unitVar));
        inputValidationCases.emplace_back(generateReturnNullIfNullMissingOrUndefined(timezoneVar));

        if (expr->isStartOfWeekSpecified()) {
            inputValidationCases.emplace_back(
                _b.makeBinaryOp(abt::Operations::And,
                                *unitIsWeekVar,
                                _b.generateNullMissingOrUndefined(*startOfWeekVar)),
                _b.makeNullConstant());
        }

        // "timezone" parameter validation.
        inputValidationCases.emplace_back(
            _b.generateNonStringCheck(timezoneVar),
            _b.makeFail(ErrorCodes::Error{7157919},
                        "$dateDiff parameter 'timezone' must be a string"));
        inputValidationCases.emplace_back(
            _b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, timezoneVar)),
            _b.makeFail(ErrorCodes::Error{7157920},
                        "$dateDiff parameter 'timezone' must be a valid timezone"));

        // "startDate" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            startDateVar, ErrorCodes::Error{7157921}, "$dateDiff"_sd, "startDate"_sd));

        // "endDate" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            endDateVar, ErrorCodes::Error{7157922}, "$dateDiff"_sd, "endDate"_sd));

        // "unit" parameter validation.
        inputValidationCases.emplace_back(
            _b.generateNonStringCheck(unitVar),
            _b.makeFail(ErrorCodes::Error{7157923}, "$dateDiff parameter 'unit' must be a string"));
        inputValidationCases.emplace_back(
            _b.makeNot(_b.makeFunction("isTimeUnit", unitVar)),
            _b.makeFail(ErrorCodes::Error{7157924},
                        "$dateDiff parameter 'unit' must be a valid time unit"));

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
            inputValidationCases.emplace_back(
                _b.makeBinaryOp(abt::Operations::And,
                                *unitIsWeekVar,
                                _b.generateNonStringCheck(*startOfWeekVar)),
                _b.makeFail(ErrorCodes::Error{7157925},
                            "$dateDiff parameter 'startOfWeek' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeBinaryOp(abt::Operations::And,
                                *unitIsWeekVar,
                                _b.makeNot(_b.makeFunction("isDayOfWeek", *startOfWeekVar))),
                _b.makeFail(ErrorCodes::Error{7157926},
                            "$dateDiff parameter 'startOfWeek' must be a valid day of the week"));
        }

        auto dateDiffExpression = _b.buildMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateDiffFunctionCall));

        pushExpr(_b.makeLet(frameId, std::move(bindings), std::move(dateDiffExpression)));
    }
    void visit(const ExpressionDateFromString* expr) final {
        const auto& children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(
            1 + (expr->isFormatSpecified() ? 1 : 0) + (expr->isTimezoneSpecified() ? 1 : 0) +
            (expr->isOnErrorSpecified() ? 1 : 0) + (expr->isOnNullSpecified() ? 1 : 0));

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};

        // Get child expressions.
        auto onErrorExpression = expr->isOnErrorSpecified() ? popExpr() : _b.makeNullConstant();
        auto onNullExpression = expr->isOnNullSpecified() ? popExpr() : _b.makeNullConstant();
        auto formatExpression = expr->isFormatSpecified() ? popExpr() : _b.makeNullConstant();

        auto timezoneExpression =
            expr->isTimezoneSpecified() ? popExpr() : _b.makeStrConstant("UTC"_sd);

        auto dateStringExpression = popExpr();

        const bool timezoneIsConstantExpr = timezoneExpression.isConstantExpr();
        const bool formatIsConstantExpr = formatExpression.isConstantExpr();

        // Set up the frame and the 'bindings' vector.
        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        // 'timezoneVar' will be populated only if the "timezone" parameter is a non-constant
        // expression. Likewise, 'formatVar' will be populated only if the "format" parameter
        // is a non-constant expression.
        SbVar dateStringVar{frameId, numLocalVars++};
        auto timezoneVar = !timezoneIsConstantExpr
            ? boost::make_optional(SbVar{frameId, numLocalVars++})
            : boost::none;
        auto formatVar = !formatIsConstantExpr
            ? boost::make_optional(SbVar{frameId, numLocalVars++})
            : boost::none;

        // Set up parameters for an invocation of built-in "dateFromString" function.
        SbExpr::Vector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(dateStringVar);

        if (timezoneIsConstantExpr) {
            arguments.push_back(timezoneExpression.clone());
        } else {
            arguments.push_back(*timezoneVar);
        }

        if (expr->isFormatSpecified()) {
            if (formatIsConstantExpr) {
                arguments.push_back(formatExpression.clone());
            } else {
                arguments.push_back(*formatVar);
            }
        }

        // Create an expression to invoke built-in "dateFromString" function.
        std::string functionName =
            expr->isOnErrorSpecified() ? "dateFromStringNoThrow" : "dateFromString";
        auto dateFromStringFunctionCall = _b.makeFunction(functionName, std::move(arguments));

        // Create expressions to check that each argument to "dateFromString" function exists, is
        // not null, and is of the correct type.
        std::vector<SbExprPair> inputValidationCases;

        // Return onNull if dateString is null or missing.
        inputValidationCases.emplace_back(_b.generateNullMissingOrUndefined(dateStringVar),
                                          std::move(onNullExpression));

        // Create an expression to return Nothing if specified, or raise a conversion failure.
        // As long as onError is specified, a Nothing return will always be filled with onError.
        auto nonStringReturn = expr->isOnErrorSpecified()
            ? _b.makeNothingConstant()
            : _b.makeFail(ErrorCodes::ConversionFailure,
                          "$dateFromString requires that 'dateString' be a string");

        inputValidationCases.emplace_back(_b.generateNonStringCheck(dateStringVar),
                                          std::move(nonStringReturn));

        if (expr->isTimezoneSpecified()) {
            if (timezoneIsConstantExpr) {
                // Return null if timezone is specified as either null or missing.
                inputValidationCases.push_back(
                    generateReturnNullIfNullMissingOrUndefined(timezoneExpression.clone()));
            } else {
                inputValidationCases.push_back(
                    generateReturnNullIfNullMissingOrUndefined(*timezoneVar));
            }
        }

        if (expr->isFormatSpecified()) {
            // validate "format" parameter only if it has been specified.
            if (formatIsConstantExpr) {
                auto [formatTag, formatVal] = formatExpression.getConstantValue();
                if (!sbe::value::isNullish(formatTag)) {
                    // We don't want to error on null.
                    uassert(4997802,
                            "$dateFromString requires that 'format' be a string",
                            sbe::value::isString(formatTag));
                    TimeZone::validateFromStringFormat(getStringView(formatTag, formatVal));
                }

                inputValidationCases.push_back(
                    generateReturnNullIfNullMissingOrUndefined(formatExpression.clone()));
            } else {
                inputValidationCases.push_back(
                    generateReturnNullIfNullMissingOrUndefined(*formatVar));

                inputValidationCases.emplace_back(
                    _b.generateNonStringCheck(*formatVar),
                    _b.makeFail(ErrorCodes::Error{4997803},
                                "$dateFromString requires that 'format' be a string"));

                inputValidationCases.emplace_back(
                    _b.makeNot(_b.makeFunction("validateFromStringFormat", *formatVar)),
                    // This should be unreachable. The validation function above will uassert on an
                    // invalid format string and then return true. It returns false on non-string
                    // input, but we already check for non-string format above.
                    _b.makeNullConstant());
            }
        }

        // "timezone" parameter validation.
        if (timezoneIsConstantExpr) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.getConstantValue();
            if (!sbe::value::isNullish(timezoneTag)) {
                // We don't want to error on null.
                uassert(4997805,
                        "$dateFromString parameter 'timezone' must be a string",
                        sbe::value::isString(timezoneTag));
                auto [timezoneDBTag, timezoneDBVal] =
                    _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
                uassert(4997801,
                        "$dateFromString first argument must be a timezoneDB object",
                        timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
                uassert(4997806,
                        "$dateFromString parameter 'timezone' must be a valid timezone",
                        sbe::vm::isValidTimezone(timezoneTag,
                                                 timezoneVal,
                                                 sbe::value::getTimeZoneDBView(timezoneDBVal)));
            }
        } else {
            inputValidationCases.emplace_back(
                _b.generateNonStringCheck(*timezoneVar),
                _b.makeFail(ErrorCodes::Error{4997807},
                            "$dateFromString parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, *timezoneVar)),
                _b.makeFail(ErrorCodes::Error{4997808},
                            "$dateFromString parameter 'timezone' must be a valid timezone"));
        }

        auto dateFromStringExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateFromStringFunctionCall));

        // If onError is specified, a Nothing return means that either dateString is not a string,
        // or the builtin dateFromStringNoThrow caught an error. We return onError in either case.
        if (expr->isOnErrorSpecified()) {
            dateFromStringExpr = _b.makeBinaryOp(abt::Operations::FillEmpty,
                                                 std::move(dateFromStringExpr),
                                                 std::move(onErrorExpression));
        }

        auto bindings = SbExpr::makeSeq(std::move(dateStringExpression));

        if (!timezoneIsConstantExpr) {
            bindings.push_back(std::move(timezoneExpression));
        }
        if (!formatIsConstantExpr) {
            bindings.push_back(std::move(formatExpression));
        }

        pushExpr(_b.makeLet(frameId, std::move(bindings), std::move(dateFromStringExpr)));
    }
    void visit(const ExpressionDateFromParts* expr) final {
        // This expression can carry null children depending on the set of fields provided,
        // to compute a date from parts so we only need to pop if a child exists.
        const auto& children = expr->getChildren();
        invariant(children.size() == 11);

        SbExpr eTimezone = children[10] ? popExpr() : SbExpr{};
        SbExpr eIsoDayOfWeek = children[9] ? popExpr() : SbExpr{};
        SbExpr eIsoWeek = children[8] ? popExpr() : SbExpr{};
        SbExpr eIsoWeekYear = children[7] ? popExpr() : SbExpr{};
        SbExpr eMillisecond = children[6] ? popExpr() : SbExpr{};
        SbExpr eSecond = children[5] ? popExpr() : SbExpr{};
        SbExpr eMinute = children[4] ? popExpr() : SbExpr{};
        SbExpr eHour = children[3] ? popExpr() : SbExpr{};
        SbExpr eDay = children[2] ? popExpr() : SbExpr{};
        SbExpr eMonth = children[1] ? popExpr() : SbExpr{};
        SbExpr eYear = children[0] ? popExpr() : SbExpr{};

        auto frameId = _context->state.frameId();
        SbVar yearVar{frameId, 0};
        SbVar monthVar{frameId, 1};
        SbVar dayVar{frameId, 2};
        SbVar hourVar{frameId, 3};
        SbVar minVar{frameId, 4};
        SbVar secVar{frameId, 5};
        SbVar millisecVar{frameId, 6};
        SbVar timeZoneVar{frameId, 7};

        std::string functionName = eIsoWeekYear ? "datePartsWeekYear" : "dateParts";

        // Build a chain of nested bounds checks for each date part that is provided in the
        // expression. We elide the checks in the case that default values are used. These bound
        // checks are then used by folding over pairs of ite tests and else branches to implement
        // short-circuiting in the case that checks fail. To emulate the control flow of MQL for
        // this expression we interleave type conversion checks with time component bound checks.
        const auto minInt16 = std::numeric_limits<int16_t>::lowest();
        const auto maxInt16 = std::numeric_limits<int16_t>::max();

        // Constructs an expression that does a bound check of var over a closed interval [lower,
        // upper].
        auto boundedCheck =
            [&](SbVar var, int16_t lower, int16_t upper, const std::string& varName) {
                std::string errMsg = (varName == "year" || varName == "isoWeekYear")
                    ? fmt::format("'{}' must evaluate to an integer in the range {} to {}",
                                  varName,
                                  lower,
                                  upper)
                    : fmt::format("'{}' must evaluate to a value in the range [{}, {}]",
                                  varName,
                                  lower,
                                  upper);

                return SbExprPair{
                    _b.makeBinaryOp(
                        abt::Operations::Or,
                        _b.makeBinaryOp(abt::Operations::Lt, var, _b.makeInt32Constant(lower)),
                        _b.makeBinaryOp(abt::Operations::Gt, var, _b.makeInt32Constant(upper))),
                    _b.makeFail(ErrorCodes::Error{7157916}, errMsg)};
            };

        // Here we want to validate each field that is provided as input to the agg expression. To
        // do this we implement the following checks:
        // 1) Check if the value in a given slot null or missing.
        // 2) Check if the value in a given slot is an integral int64.
        auto fieldConversionBinding = [&](SbExpr expr, const std::string& varName) {
            auto outerFrameId = _context->state.frameId();
            SbVar outerVar{outerFrameId, 0};

            auto innerFrameId = _context->state.frameId();
            SbVar convertedFieldVar{innerFrameId, 0};

            return _b.makeLet(
                outerFrameId,
                SbExpr::makeSeq(std::move(expr)),
                _b.makeIf(
                    _b.makeBinaryOp(abt::Operations::Or,
                                    _b.makeNot(_b.makeFunction("exists", outerVar)),
                                    _b.makeFunction("isNull", outerVar)),
                    _b.makeNullConstant(),
                    _b.makeLet(
                        innerFrameId,
                        SbExpr::makeSeq(_b.makeFunction("convert",
                                                        outerVar,
                                                        _b.makeInt32Constant(static_cast<int32_t>(
                                                            sbe::value::TypeTags::NumberInt64)))),
                        _b.makeIf(_b.makeFunction("exists", convertedFieldVar),
                                  convertedFieldVar,
                                  _b.makeFail(ErrorCodes::Error{7157917},
                                              str::stream() << "'" << varName << "'"
                                                            << " must evaluate to an integer")))));
        };

        // Build two vectors on the fly to elide bound and conversion for defaulted values.
        std::vector<SbExprPair> boundChecks;  // checks for lower and upper bounds of date fields.

        // Make a disjunction of null checks for each date part by over this vector. These checks
        // are necessary after the initial conversion computation because we need have the outer let
        // binding evaluate to null if any field is null.
        auto nullExprs = SbExpr::makeSeq(_b.generateNullMissingOrUndefined(timeZoneVar),
                                         _b.generateNullMissingOrUndefined(millisecVar),
                                         _b.generateNullMissingOrUndefined(secVar),
                                         _b.generateNullMissingOrUndefined(minVar),
                                         _b.generateNullMissingOrUndefined(hourVar),
                                         _b.generateNullMissingOrUndefined(dayVar),
                                         _b.generateNullMissingOrUndefined(monthVar),
                                         _b.generateNullMissingOrUndefined(yearVar));

        // The first "if" expression allows short-circuting of the null field case. If the nullish
        // checks pass, then we check the bounds of each field and invoke the builtins if all checks
        // pass.
        boundChecks.emplace_back(_b.makeBooleanOpTree(abt::Operations::Or, std::move(nullExprs)),
                                 _b.makeNullConstant());

        // Operands is for the outer let bindings.
        SbExpr::Vector operands;
        if (eIsoWeekYear) {
            boundChecks.push_back(boundedCheck(yearVar, 1, 9999, "isoWeekYear"));
            operands.push_back(fieldConversionBinding(std::move(eIsoWeekYear), "isoWeekYear"));
            if (!eIsoWeek) {
                operands.push_back(_b.makeInt32Constant(1));
            } else {
                boundChecks.push_back(boundedCheck(monthVar, minInt16, maxInt16, "isoWeek"));
                operands.push_back(fieldConversionBinding(std::move(eIsoWeek), "isoWeek"));
            }
            if (!eIsoDayOfWeek) {
                operands.push_back(_b.makeInt32Constant(1));
            } else {
                boundChecks.push_back(boundedCheck(dayVar, minInt16, maxInt16, "isoDayOfWeek"));
                operands.push_back(
                    fieldConversionBinding(std::move(eIsoDayOfWeek), "isoDayOfWeek"));
            }
        } else {
            // The regular year/month/day case.
            if (!eYear) {
                operands.push_back(_b.makeInt32Constant(1970));
            } else {
                boundChecks.push_back(boundedCheck(yearVar, 1, 9999, "year"));
                operands.push_back(fieldConversionBinding(std::move(eYear), "year"));
            }
            if (!eMonth) {
                operands.push_back(_b.makeInt32Constant(1));
            } else {
                boundChecks.push_back(boundedCheck(monthVar, minInt16, maxInt16, "month"));
                operands.push_back(fieldConversionBinding(std::move(eMonth), "month"));
            }
            if (!eDay) {
                operands.push_back(_b.makeInt32Constant(1));
            } else {
                boundChecks.push_back(boundedCheck(dayVar, minInt16, maxInt16, "day"));
                operands.push_back(fieldConversionBinding(std::move(eDay), "day"));
            }
        }
        if (!eHour) {
            operands.push_back(_b.makeInt32Constant(0));
        } else {
            boundChecks.push_back(boundedCheck(hourVar, minInt16, maxInt16, "hour"));
            operands.push_back(fieldConversionBinding(std::move(eHour), "hour"));
        }
        if (!eMinute) {
            operands.push_back(_b.makeInt32Constant(0));
        } else {
            boundChecks.push_back(boundedCheck(minVar, minInt16, maxInt16, "minute"));
            operands.push_back(fieldConversionBinding(std::move(eMinute), "minute"));
        }
        if (!eSecond) {
            operands.push_back(_b.makeInt32Constant(0));
        } else {
            // MQL doesn't place bound restrictions on the second field, because seconds carry over
            // to minutes and can be large ints such as 71,841,012 or even unix epochs.
            operands.push_back(fieldConversionBinding(std::move(eSecond), "second"));
        }
        if (!eMillisecond) {
            operands.push_back(_b.makeInt32Constant(0));
        } else {
            // MQL doesn't enforce bound restrictions on millisecond fields because milliseconds
            // carry over to seconds.
            operands.push_back(fieldConversionBinding(std::move(eMillisecond), "millisecond"));
        }
        if (!eTimezone) {
            operands.push_back(_b.makeStrConstant("UTC"));
        } else {
            // Validate that eTimezone is a string.
            auto innerFrameId = _context->state.frameId();
            SbVar timeZoneVar{innerFrameId, 0};

            operands.push_back(_b.makeLet(
                innerFrameId,
                SbExpr::makeSeq(std::move(eTimezone)),
                _b.makeIf(_b.makeFunction("isString", timeZoneVar),
                          timeZoneVar,
                          _b.makeFail(ErrorCodes::Error{7157918},
                                      str::stream() << "'timezone' must evaluate to a string"))));
        }

        // Invocation of the datePartsWeekYear and dateParts functions depend on a TimeZoneDatabase
        // for datetime computation. This global object is registered as an unowned value in the
        // runtime environment so we pass the corresponding slot to the datePartsWeekYear and
        // dateParts functions as a variable.
        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};

        auto computeDate = _b.makeFunction(functionName,
                                           timeZoneDBVar,
                                           yearVar,
                                           monthVar,
                                           dayVar,
                                           hourVar,
                                           minVar,
                                           secVar,
                                           millisecVar,
                                           timeZoneVar);

        pushExpr(_b.makeLet(frameId,
                            std::move(operands),
                            _b.buildMultiBranchConditionalFromCaseValuePairs(
                                std::move(boundChecks), std::move(computeDate))));
    }

    void visit(const ExpressionDateToParts* expr) final {
        const auto& children = expr->getChildren();

        auto frameId = _context->state.frameId();
        SbVar dateVar{frameId, 0};
        SbVar timezoneVar{frameId, 1};
        SbVar isoflagVar{frameId, 2};

        // Initialize arguments with values from stack or default values.
        SbExpr isoflag = children[2] ? popExpr() : _b.makeBoolConstant(false);
        SbExpr timezone = children[1] ? popExpr() : _b.makeStrConstant("UTC");

        if (!children[0]) {
            pushExpr(_b.makeFail(ErrorCodes::Error{7157911}, "$dateToParts must include a date"));
            return;
        }

        SbExpr date = popExpr();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};

        auto isoTypeMask = getBSONTypeMask(sbe::value::TypeTags::Boolean);

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDateToPartsFunc = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(timezoneVar), _b.makeNullConstant()},
                SbExprPair{_b.makeNot(_b.makeFunction("isString", timezoneVar)),
                           _b.makeFail(ErrorCodes::Error{7157912},
                                       "$dateToParts timezone must be a string")},
                SbExprPair{_b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                           _b.makeFail(ErrorCodes::Error{7157913},
                                       "$dateToParts timezone must be a valid timezone")},
                SbExprPair{_b.generateNullMissingOrUndefined(isoflagVar), _b.makeNullConstant()},
                SbExprPair{_b.makeNot(_b.makeFunction(
                               "typeMatch", isoflagVar, _b.makeInt32Constant(isoTypeMask))),
                           _b.makeFail(ErrorCodes::Error{7157914},
                                       "$dateToParts iso8601 must be a boolean")},
                SbExprPair{_b.generateNullMissingOrUndefined(dateVar), _b.makeNullConstant()},
                SbExprPair{_b.makeNot(_b.makeFunction(
                               "typeMatch", dateVar, _b.makeInt32Constant(dateTypeMask()))),
                           _b.makeFail(ErrorCodes::Error{7157915},
                                       "$dateToParts date must have the format of a date")},
                // Determine whether to call dateToParts or isoDateToParts.
                SbExprPair{
                    _b.makeBinaryOp(abt::Operations::Eq, isoflagVar, _b.makeBoolConstant(false)),
                    _b.makeFunction(
                        "dateToParts", timeZoneDBVar, dateVar, timezoneVar, isoflagVar)}),
            _b.makeFunction("isoDateToParts", timeZoneDBVar, dateVar, timezoneVar, isoflagVar));

        pushExpr(
            _b.makeLet(frameId,
                       SbExpr::makeSeq(std::move(date), std::move(timezone), std::move(isoflag)),
                       std::move(totalDateToPartsFunc)));
    }

    void visit(const ExpressionDateToString* expr) final {
        const auto& children = expr->getChildren();
        invariant(children.size() == 4);
        _context->ensureArity(1 + (expr->isFormatSpecified() ? 1 : 0) +
                              (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isOnNullSpecified() ? 1 : 0));

        // Get child expressions.
        SbExpr onNullExpression = expr->isOnNullSpecified() ? popExpr() : _b.makeNullConstant();

        SbExpr timezoneExpression =
            expr->isTimezoneSpecified() ? popExpr() : _b.makeStrConstant("UTC"_sd);

        SbExpr dateExpression = popExpr();

        SbExpr formatExpression = expr->isFormatSpecified()
            ? popExpr()
            : _b.makeStrConstant(kIsoFormatStringZ);  // assumes UTC until disproven

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};

        auto [timezoneDBTag, timezoneDBVal] =
            _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
        uassert(4997900,
                "$dateToString first argument must be a timezoneDB object",
                timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
        auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);

        // Local bind to hold the date, timezone and format expression results
        auto frameId = _context->state.frameId();
        SbVar dateVar{frameId, 0};
        SbVar formatVar{frameId, 1};
        SbVar timezoneVar{frameId, 2};
        SbVar dateToStringVar{frameId, 3};

        // Create expressions to check that each argument to "dateToString" function exists, is not
        // null, and is of the correct type.
        std::vector<SbExprPair> inputValidationCases;
        // Return the evaluation of the function, if the result is correct.
        inputValidationCases.emplace_back(_b.makeFunction("exists"_sd, dateToStringVar),
                                          dateToStringVar);
        // Return onNull if date is null or missing.
        inputValidationCases.emplace_back(_b.generateNullMissingOrUndefined(dateVar),
                                          std::move(onNullExpression));
        // Return null if format or timezone is null or missing.
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(formatVar));
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(timezoneVar));

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            dateVar, ErrorCodes::Error{4997901}, "$dateToString"_sd, "date"_sd));

        // "timezone" parameter validation.
        if (timezoneExpression.isConstantExpr()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.getConstantValue();
            if (!sbe::value::isNullish(timezoneTag)) {
                // If the query did not specify a format string and a non-UTC timezone was
                // specified, the default format should not use a 'Z' suffix.
                if (!expr->isFormatSpecified() &&
                    !(sbe::vm::getTimezone(timezoneTag, timezoneVal, timezoneDB).isUtcZone())) {
                    formatExpression = _b.makeStrConstant(kIsoFormatStringNonZ);
                }

                // We don't want to error on null.
                uassert(4997905,
                        "$dateToString parameter 'timezone' must be a string",
                        sbe::value::isString(timezoneTag));
                uassert(4997906,
                        "$dateToString parameter 'timezone' must be a valid timezone",
                        sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
            }
        } else {
            inputValidationCases.emplace_back(
                _b.generateNonStringCheck(timezoneVar),
                _b.makeFail(ErrorCodes::Error{4997907},
                            "$dateToString parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                _b.makeFail(ErrorCodes::Error{4997908},
                            "$dateToString parameter 'timezone' must be a valid timezone"));
        }

        // "format" parameter validation.
        if (formatExpression.isConstantExpr()) {
            auto [formatTag, formatVal] = formatExpression.getConstantValue();
            if (!sbe::value::isNullish(formatTag)) {
                // We don't want to return an error on null.
                uassert(4997902,
                        "$dateToString parameter 'format' must be a string",
                        sbe::value::isString(formatTag));
                TimeZone::validateToStringFormat(getStringView(formatTag, formatVal));
            }
        } else {
            inputValidationCases.emplace_back(
                _b.generateNonStringCheck(formatVar),
                _b.makeFail(ErrorCodes::Error{4997903},
                            "$dateToString parameter 'format' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeNot(_b.makeFunction("isValidToStringFormat", formatVar)),
                _b.makeFail(ErrorCodes::Error{4997904},
                            "$dateToString parameter 'format' must be a valid format"));
        }

        // Set parameters for an invocation of built-in "dateToString" function.
        SbExpr::Vector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(dateVar);
        arguments.push_back(formatVar);
        arguments.push_back(timezoneVar);

        // Create an expression to invoke built-in "dateToString" function.
        auto dateToStringFunctionCall = _b.makeFunction("dateToString", std::move(arguments));

        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(dateExpression),
                                            std::move(formatExpression),
                                            std::move(timezoneExpression),
                                            std::move(dateToStringFunctionCall)),
                            _b.buildMultiBranchConditionalFromCaseValuePairs(
                                std::move(inputValidationCases), _b.makeNothingConstant())));
    }
    void visit(const ExpressionDateTrunc* expr) final {
        const auto& children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(2 + (expr->isBinSizeSpecified() ? 1 : 0) +
                              (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isStartOfWeekSpecified() ? 1 : 0));

        // Get child expressions.
        auto startOfWeekExpression =
            expr->isStartOfWeekSpecified() ? popExpr() : _b.makeStrConstant("sun"_sd);
        auto timezoneExpression =
            expr->isTimezoneSpecified() ? popExpr() : _b.makeStrConstant("UTC"_sd);
        auto binSizeExpression = expr->isBinSizeSpecified() ? popExpr() : _b.makeInt64Constant(1);
        auto unitExpression = popExpr();
        auto dateExpression = popExpr();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};
        auto [timezoneDBTag, timezoneDBVal] =
            _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
        tassert(7157927,
                "$dateTrunc first argument must be a timezoneDB object",
                timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
        auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);

        // Local bind to hold the argument expression results
        auto frameId = _context->state.frameId();
        SbVar dateVar{frameId, 0};
        SbVar unitVar{frameId, 1};
        SbVar binSizeVar{frameId, 2};
        SbVar timezoneVar{frameId, 3};
        SbVar startOfWeekVar{frameId, 4};
        SbVar dateTruncVar{frameId, 5};

        // Set parameters for an invocation of built-in "dateTrunc" function.
        SbExpr::Vector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(dateVar);
        arguments.push_back(unitVar);
        arguments.push_back(binSizeVar);
        arguments.push_back(timezoneVar);
        arguments.push_back(startOfWeekVar);

        // Create an expression to invoke built-in "dateTrunc" function.
        auto dateTruncFunctionCall = _b.makeFunction("dateTrunc", std::move(arguments));

        // Local bind to hold the unitIsWeek common subexpression
        auto innerFrameId = _context->state.frameId();
        SbVar unitIsWeekVar{innerFrameId, 0};
        auto unitIsWeekExpression = generateIsEqualToStringCheck(unitVar, "week"_sd);

        // Create expressions to check that each argument to "dateTrunc" function exists, is not
        // null, and is of the correct type.
        std::vector<SbExprPair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(dateVar));
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(unitVar));
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(binSizeVar));
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(timezoneVar));
        inputValidationCases.emplace_back(
            _b.makeBinaryOp(abt::Operations::And,
                            unitIsWeekVar,
                            _b.generateNullMissingOrUndefined(startOfWeekVar)),
            _b.makeNullConstant());

        // "timezone" parameter validation.
        if (timezoneExpression.isConstantExpr()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.getConstantValue();
            tassert(7157928,
                    "$dateTrunc parameter 'timezone' must be a string",
                    sbe::value::isString(timezoneTag));
            tassert(7157929,
                    "$dateTrunc parameter 'timezone' must be a valid timezone",
                    sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
        } else {
            inputValidationCases.emplace_back(
                _b.generateNonStringCheck(timezoneVar),
                _b.makeFail(ErrorCodes::Error{7157930},
                            "$dateTrunc parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                _b.makeFail(ErrorCodes::Error{7157931},
                            "$dateTrunc parameter 'timezone' must be a valid timezone"));
        }

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            dateVar, ErrorCodes::Error{7157932}, "$dateTrunc"_sd, "date"_sd));

        // "unit" parameter validation.
        if (unitExpression.isConstantExpr()) {
            auto [unitTag, unitVal] = unitExpression.getConstantValue();
            tassert(7157933,
                    "$dateTrunc parameter 'unit' must be a string",
                    sbe::value::isString(unitTag));
            auto unitString = sbe::value::getStringView(unitTag, unitVal);
            tassert(7157934,
                    "$dateTrunc parameter 'unit' must be a valid time unit",
                    isValidTimeUnit(unitString));
        } else {
            inputValidationCases.emplace_back(
                _b.generateNonStringCheck(unitVar),
                _b.makeFail(ErrorCodes::Error{7157935},
                            "$dateTrunc parameter 'unit' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeNot(_b.makeFunction("isTimeUnit", unitVar)),
                _b.makeFail(ErrorCodes::Error{7157936},
                            "$dateTrunc parameter 'unit' must be a valid time unit"));
        }

        // "binSize" parameter validation.
        if (expr->isBinSizeSpecified()) {
            if (binSizeExpression.isConstantExpr()) {
                auto [binSizeTag, binSizeValue] = binSizeExpression.getConstantValue();
                tassert(7157937,
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer",
                        sbe::value::isNumber(binSizeTag));
                auto [binSizeLongOwn, binSizeLongTag, binSizeLongValue] =
                    sbe::value::genericNumConvert(
                        binSizeTag, binSizeValue, sbe::value::TypeTags::NumberInt64);
                tassert(7157938,
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer",
                        binSizeLongTag != sbe::value::TypeTags::Nothing);
                auto binSize = sbe::value::bitcastTo<int64_t>(binSizeLongValue);
                tassert(7157939,
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer",
                        binSize > 0);
            } else {
                inputValidationCases.emplace_back(
                    _b.makeNot(_b.makeBinaryOp(
                        abt::Operations::And,
                        _b.makeBinaryOp(
                            abt::Operations::And,
                            _b.makeFunction("isNumber", binSizeVar),
                            _b.makeFunction(
                                "exists",
                                _b.makeFunction("convert",
                                                binSizeVar,
                                                _b.makeInt32Constant(static_cast<int32_t>(
                                                    sbe::value::TypeTags::NumberInt64))))),
                        _b.generatePositiveCheck(binSizeVar))),
                    _b.makeFail(
                        ErrorCodes::Error{7157940},
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer"));
            }
        }

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            if (startOfWeekExpression.isConstantExpr()) {
                auto [startOfWeekTag, startOfWeekVal] = startOfWeekExpression.getConstantValue();
                tassert(7157941,
                        "$dateTrunc parameter 'startOfWeek' must be a string",
                        sbe::value::isString(startOfWeekTag));
                auto startOfWeekString = sbe::value::getStringView(startOfWeekTag, startOfWeekVal);
                tassert(7157942,
                        "$dateTrunc parameter 'startOfWeek' must be a valid day of the week",
                        isValidDayOfWeek(startOfWeekString));
            } else {
                // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
                inputValidationCases.emplace_back(
                    _b.makeBinaryOp(abt::Operations::And,
                                    unitIsWeekVar,
                                    _b.generateNonStringCheck(startOfWeekVar)),
                    _b.makeFail(ErrorCodes::Error{7157943},
                                "$dateTrunc parameter 'startOfWeek' must be a string"));
                inputValidationCases.emplace_back(
                    _b.makeBinaryOp(abt::Operations::And,
                                    unitIsWeekVar,
                                    _b.makeNot(_b.makeFunction("isDayOfWeek", startOfWeekVar))),
                    _b.makeFail(
                        ErrorCodes::Error{7157944},
                        "$dateTrunc parameter 'startOfWeek' must be a valid day of the week"));
            }
        }

        pushExpr(_b.makeLet(
            frameId,
            SbExpr::makeSeq(std::move(dateExpression),
                            std::move(unitExpression),
                            std::move(binSizeExpression),
                            std::move(timezoneExpression),
                            std::move(startOfWeekExpression),
                            std::move(dateTruncFunctionCall)),
            _b.makeIf(_b.makeFunction("exists", dateTruncVar),
                      dateTruncVar,
                      _b.makeLet(innerFrameId,
                                 SbExpr::makeSeq(std::move(unitIsWeekExpression)),
                                 _b.buildMultiBranchConditionalFromCaseValuePairs(
                                     std::move(inputValidationCases), _b.makeNothingConstant())))));
    }
    void visit(const ExpressionDivide* expr) final {
        _context->ensureArity(2);
        auto rhs = popExpr();
        auto lhs = popExpr();

        auto frameId = _context->state.frameId();
        SbVar lhsVar{frameId, 0};
        SbVar rhsVar{frameId, 1};

        auto checkIsNumber = _b.makeBinaryOp(abt::Operations::And,
                                             _b.makeFunction("isNumber", lhsVar),
                                             _b.makeFunction("isNumber", rhsVar));

        auto checkIsNullOrMissing = _b.makeBinaryOp(abt::Operations::Or,
                                                    _b.generateNullMissingOrUndefined(lhsVar),
                                                    _b.generateNullMissingOrUndefined(rhsVar));

        auto divideExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{std::move(checkIsNullOrMissing), _b.makeNullConstant()},
                SbExprPair{std::move(checkIsNumber),
                           _b.makeBinaryOp(abt::Operations::Div, lhsVar, rhsVar)}),
            _b.makeFail(ErrorCodes::Error{7157719}, "$divide only supports numeric types"));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(lhs), std::move(rhs)), std::move(divideExpr)));
    }
    void visit(const ExpressionExp* expr) final {
        auto frameId = _context->state.frameId();
        SbVar inputVar{frameId, 0};

        auto expExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(inputVar),
                    _b.makeFail(ErrorCodes::Error{7157704}, "$exp only supports numeric types")}),
            _b.makeFunction("exp", inputVar));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(expExpr)));
    }
    void visit(const ExpressionFieldPath* expr) final {
        pushExpr(generateExpressionFieldPath(_context->state,
                                             expr->getFieldPath(),
                                             expr->getVariableId(),
                                             _context->rootSlot,
                                             *_context->slots,
                                             &_context->environment));
    }
    void visit(const ExpressionFilter* expr) final {
        unsupportedExpression("$filter");
    }

    void visit(const ExpressionFloor* expr) final {
        auto frameId = _context->state.frameId();
        SbVar inputVar{frameId, 0};

        auto floorExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(inputVar),
                    _b.makeFail(ErrorCodes::Error{7157703}, "$floor only supports numeric types")}),
            _b.makeFunction("floor", inputVar));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(floorExpr)));
    }
    void visit(const ExpressionIfNull* expr) final {
        auto numChildren = expr->getChildren().size();
        invariant(numChildren >= 2);

        SbExpr::Vector values;
        values.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            values.emplace_back(popExpr());
        }
        std::reverse(values.begin(), values.end());

        auto resultExpr = _b.makeIfNullExpr(std::move(values));

        pushExpr(std::move(resultExpr));
    }
    void visit(const ExpressionIn* expr) final {
        auto arrExpArg = popExpr();
        auto expArg = popExpr();

        auto frameId = _context->state.frameId();
        SbVar expLocalVar{frameId, 0};
        SbVar arrLocalVar{frameId, 1};

        auto functionArgs = SbExpr::makeSeq(expLocalVar, arrLocalVar);
        auto collatorSlot = _context->state.getCollatorSlot();
        if (collatorSlot) {
            functionArgs.emplace_back(SbVar{*collatorSlot});
        }

        auto inExpr = _b.makeIf(
            // Check that the arr argument is an array and is not missing.
            _b.makeFillEmptyFalse(_b.makeFunction("isArray", arrLocalVar)),
            (collatorSlot ? _b.makeFunction("collIsMember", std::move(functionArgs))
                          : _b.makeFunction("isMember", std::move(functionArgs))),
            _b.makeFail(ErrorCodes::Error{5153700}, "$in requires an array as a second argument"));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(expArg), std::move(arrExpArg)), std::move(inExpr)));
    }
    void visit(const ExpressionIndexOfArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfBytes* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfBytes");
    }

    void visit(const ExpressionIndexOfCP* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfCP");
    }
    void visit(const ExpressionIsNumber* expr) final {
        auto arg = popExpr();
        auto frameId = _context->state.frameId();
        SbVar var{frameId, 0};

        auto exprIsNum = _b.makeIf(_b.makeFunction("exists", var),
                                   _b.makeFunction("isNumber", var),
                                   _b.makeBoolConstant(false));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(exprIsNum)));
    }
    void visit(const ExpressionLet* expr) final {
        invariant(!_context->varsFrameStack.empty());
        // The evaluated result of the $let is the evaluated result of its "in" field, which is
        // already on top of the stack. The "infix" visitor has already popped the variable
        // initializers off the expression stack.
        _context->ensureArity(1);

        // We should have bound all the variables from this $let expression.
        auto& currentFrame = _context->varsFrameStack.top();
        invariant(currentFrame.currentBindingIndex == currentFrame.bindings.size());

        auto resultExpr = popExpr();

        for (size_t i = currentFrame.bindings.size(); i > 0;) {
            auto& binding = currentFrame.bindings[--i];
            resultExpr = _b.makeLet(
                binding.frameId, SbExpr::makeSeq(std::move(binding.expr)), std::move(resultExpr));
        }

        pushExpr(std::move(resultExpr));

        // Pop the lexical frame for this $let and remove all its bindings, which are now out of
        // scope.
        for (const auto& binding : currentFrame.bindings) {
            _context->environment.erase(binding.variableId);
        }
        _context->varsFrameStack.pop();
    }

    void visit(const ExpressionLn* expr) final {
        auto frameId = _context->state.frameId();
        SbVar inputVar{frameId, 0};

        auto lnExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(inputVar),
                    _b.makeFail(ErrorCodes::Error{7157705}, "$ln only supports numeric types")},
                // Note: In MQL, $ln on a NumberDecimal NaN historically evaluates to a NumberDouble
                // NaN.
                SbExprPair{_b.generateNaNCheck(inputVar),
                           _b.makeFunction("convert",
                                           inputVar,
                                           _b.makeInt32Constant(static_cast<int32_t>(
                                               sbe::value::TypeTags::NumberDouble)))},
                SbExprPair{_b.generateNonPositiveCheck(inputVar),
                           _b.makeFail(ErrorCodes::Error{7157706},
                                       "$ln's argument must be a positive number")}),
            _b.makeFunction("ln", inputVar));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(lnExpr)));
    }
    void visit(const ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionLog10* expr) final {
        auto frameId = _context->state.frameId();
        SbVar inputVar{frameId, 0};

        auto log10Expr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(inputVar),
                    _b.makeFail(ErrorCodes::Error{7157707}, "$log10 only supports numeric types")},
                // Note: In MQL, $log10 on a NumberDecimal NaN historically evaluates to a
                // NumberDouble NaN.
                SbExprPair{_b.generateNaNCheck(inputVar),
                           _b.makeFunction("convert",
                                           inputVar,
                                           _b.makeInt32Constant(static_cast<int32_t>(
                                               sbe::value::TypeTags::NumberDouble)))},
                SbExprPair{_b.generateNonPositiveCheck(inputVar),
                           _b.makeFail(ErrorCodes::Error{7157708},
                                       "$log10's argument must be a positive number")}),
            _b.makeFunction("log10", inputVar));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(log10Expr)));
    }
    void visit(const ExpressionInternalFLEBetween* expr) final {
        unsupportedExpression("$_internalFleBetween");
    }
    void visit(const ExpressionInternalFLEEqual* expr) final {
        unsupportedExpression("$_internalFleEq");
    }
    void visit(const ExpressionEncStrStartsWith* expr) final {
        unsupportedExpression("$encStrStartsWith");
    }
    void visit(const ExpressionEncStrEndsWith* expr) final {
        unsupportedExpression("$encStrEndsWith");
    }
    void visit(const ExpressionEncStrContains* expr) final {
        unsupportedExpression("$encStrContains");
    }
    void visit(const ExpressionEncStrNormalizedEq* expr) final {
        unsupportedExpression("$encStrNormalizedEq");
    }

    void visit(const ExpressionInternalRawSortKey* expr) final {
        unsupportedExpression(ExpressionInternalRawSortKey::kName.data());
    }
    void visit(const ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }
    void visit(const ExpressionMeta* expr) final {
        auto pushMetadataExpr = [&](boost::optional<sbe::value::SlotId> slot, uint32_t typeMask) {
            if (slot) {
                pushExpr(
                    _b.makeIf(_b.makeFillEmptyTrue(_b.makeFunction(
                                  "typeMatch"_sd, SbVar{*slot}, _b.makeInt32Constant(typeMask))),
                              SbVar{*slot},
                              _b.makeFail(ErrorCodes::Error{8107800}, "Unexpected metadata type")));
            } else {
                pushExpr(_b.makeNothingConstant());
            }
        };
        switch (expr->getMetaType()) {
            case DocumentMetadataFields::MetaType::kSearchScore:
                pushMetadataExpr(_context->state.data->metadataSlots.searchScoreSlot,
                                 getBSONTypeMask(BSONType::numberDouble) |
                                     getBSONTypeMask(BSONType::numberLong));
                break;
            case DocumentMetadataFields::MetaType::kSearchHighlights:
                pushMetadataExpr(_context->state.data->metadataSlots.searchHighlightsSlot,
                                 getBSONTypeMask(BSONType::array));
                break;
            case DocumentMetadataFields::MetaType::kSearchScoreDetails:
                pushMetadataExpr(_context->state.data->metadataSlots.searchDetailsSlot,
                                 getBSONTypeMask(BSONType::object));
                break;
            case DocumentMetadataFields::MetaType::kSearchSequenceToken:
                pushMetadataExpr(_context->state.data->metadataSlots.searchSequenceToken,
                                 getBSONTypeMask(BSONType::string));
                break;
            default:
                unsupportedExpression("$meta");
        }
    }
    void visit(const ExpressionMod* expr) final {
        auto rhs = popExpr();
        auto lhs = popExpr();

        auto frameId = _context->state.frameId();
        SbVar lhsVar{frameId, 0};
        SbVar rhsVar{frameId, 1};

        auto modExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.makeBinaryOp(abt::Operations::Or,
                                           _b.generateNullMissingOrUndefined(lhsVar),
                                           _b.generateNullMissingOrUndefined(rhsVar)),
                           _b.makeNullConstant()},
                SbExprPair{
                    _b.makeBinaryOp(abt::Operations::Or,
                                    _b.generateNonNumericCheck(lhsVar),
                                    _b.generateNonNumericCheck(rhsVar)),
                    _b.makeFail(ErrorCodes::Error{7157718}, "$mod only supports numeric types")}),
            _b.makeFunction("mod", lhsVar, rhsVar));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(lhs), std::move(rhs)), std::move(modExpr)));
    }
    void visit(const ExpressionMultiply* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity < kArgumentCountForBinaryTree ||
            feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            visitFast(expr);
            return;
        }

        auto checkLeaf = [&](SbExpr arg) {
            auto frameId = _context->state.frameId();
            SbVar var{frameId, 0};
            auto checkedLeaf = _b.buildMultiBranchConditional(
                SbExprPair{_b.makeFunction("isNumber", var), var},
                _b.makeFail(ErrorCodes::Error{7315403},
                            "only numbers are allowed in an $multiply expression"));
            return _b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(checkedLeaf));
        };

        auto combineTwoTree = [&](SbExpr left, SbExpr right) {
            auto frameId = _context->state.frameId();
            SbVar varLeft{frameId, 0};
            SbVar varRight{frameId, 1};

            auto mulExpr = _b.buildMultiBranchConditional(
                SbExprPair{_b.makeBinaryOp(abt::Operations::Or,
                                           _b.generateNullMissingOrUndefined(varLeft),
                                           _b.generateNullMissingOrUndefined(varRight)),
                           _b.makeNullConstant()},
                _b.makeBinaryOp(abt::Operations::Mult, varLeft, varRight));

            return _b.makeLet(
                frameId, SbExpr::makeSeq(std::move(left), std::move(right)), std::move(mulExpr));
        };

        SbExpr::Vector leaves;
        leaves.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            leaves.emplace_back(checkLeaf(popExpr()));
        }
        std::reverse(std::begin(leaves), std::end(leaves));

        pushExpr(SbExpr::makeBalancedTree(combineTwoTree, std::move(leaves)));
    }
    void visitFast(const ExpressionMultiply* expr) {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Return multiplicative identity if the $multiply expression has no operands.
        if (arity == 0) {
            pushExpr(_b.makeInt32Constant(1));
            return;
        }

        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        SbExpr::Vector binds;
        SbExpr::Vector checkExprsNull;
        SbExpr::Vector checkExprsNumber;
        SbExpr::Vector variables;
        binds.reserve(arity);
        variables.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNumber.reserve(arity);

        for (size_t idx = 0; idx < arity; ++idx) {
            binds.push_back(popExpr());

            SbVar currentVar{frameId, numLocalVars++};

            checkExprsNull.push_back(_b.generateNullMissingOrUndefined(currentVar));
            checkExprsNumber.push_back(_b.makeFunction("isNumber", currentVar));
            variables.push_back(currentVar);
        }

        // At this point 'binds' vector contains arguments of $multiply expression in the reversed
        // order. We need to reverse it back to perform multiplication in the right order below.
        // Multiplication in different order can lead to different result because of accumulated
        // precision errors from floating point types.
        std::reverse(std::begin(binds), std::end(binds));

        auto checkNullAnyArgument =
            _b.makeBooleanOpTree(abt::Operations::Or, std::move(checkExprsNull));
        auto checkNumberAllArguments =
            _b.makeBooleanOpTree(abt::Operations::And, std::move(checkExprsNumber));
        auto multiplication = _b.makeNaryOp(abt::Operations::Mult, std::move(variables));

        auto multiplyExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{std::move(checkNullAnyArgument), _b.makeNullConstant()},
                SbExprPair{std::move(checkNumberAllArguments), std::move(multiplication)}),
            _b.makeFail(ErrorCodes::Error{7157721},
                        "only numbers are allowed in an $multiply expression"));

        multiplyExpr = _b.makeLet(frameId, std::move(binds), std::move(multiplyExpr));
        pushExpr(std::move(multiplyExpr));
    }
    void visit(const ExpressionNot* expr) final {
        pushExpr(_b.makeNot(_b.makeFillEmptyFalse(_b.makeFunction("coerceToBool", popExpr()))));
    }
    void visit(const ExpressionObject* expr) final {
        const auto& childExprs = expr->getChildExpressions();
        size_t childSize = childExprs.size();
        _context->ensureArity(childSize);

        // The expression argument for 'newObj' must be a sequence of a field name constant
        // expression and an expression for the value. So, we need 2 * childExprs.size() elements in
        // the expressions vector.
        SbExpr::Vector exprs;
        exprs.reserve(childSize * 2);

        // We iterate over child expressions in reverse, because they will be popped from stack in
        // reverse order.
        for (auto rit = childExprs.rbegin(); rit != childExprs.rend(); ++rit) {
            exprs.push_back(popExpr());
            exprs.push_back(_b.makeStrConstant(rit->first));
        }

        // Lastly we need to reverse it to get the correct order of arguments.
        std::reverse(exprs.begin(), exprs.end());

        pushExpr(_b.makeFunction("newObj", std::move(exprs)));
    }

    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, abt::Operations::Or);
    }
    void visit(const ExpressionPow* expr) final {
        _context->ensureArity(2);
        auto rhs = popExpr();
        auto lhs = popExpr();

        auto frameId = _context->state.frameId();
        SbVar lhsVar{frameId, 0};
        SbVar rhsVar{frameId, 1};
        // Local bind to hold the result of the built-in "pow" function
        SbVar powResVar{frameId, 2};

        auto checkIsNotNumber = _b.makeBinaryOp(abt::Operations::Or,
                                                _b.generateNonNumericCheck(lhsVar),
                                                _b.generateNonNumericCheck(rhsVar));

        auto checkBaseIsZero =
            _b.makeBinaryOp(abt::Operations::Eq, lhsVar, _b.makeInt32Constant(0));

        auto checkIsZeroAndNegative = _b.makeBinaryOp(
            abt::Operations::And, std::move(checkBaseIsZero), _b.generateNegativeCheck(rhsVar));

        auto checkIsNullOrMissing = _b.makeBinaryOp(abt::Operations::Or,
                                                    _b.generateNullMissingOrUndefined(lhsVar),
                                                    _b.generateNullMissingOrUndefined(rhsVar));

        // Create an expression to invoke built-in "pow" function
        auto powFunctionCall = _b.makeFunction("pow", lhsVar, rhsVar);

        // Return the result or check for issues if result is empty (Nothing)
        auto checkPowRes = _b.makeBinaryOp(
            abt::Operations::FillEmpty,
            powResVar,
            _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{std::move(checkIsNullOrMissing), _b.makeNullConstant()},
                    SbExprPair{std::move(checkIsNotNumber),
                               _b.makeFail(ErrorCodes::Error{5154200},
                                           "$pow only supports numeric types")},
                    SbExprPair{std::move(checkIsZeroAndNegative),
                               _b.makeFail(ErrorCodes::Error{5154201},
                                           "$pow cannot raise 0 to a negative exponent")}),
                _b.makeNothingConstant()));

        pushExpr(
            _b.makeLet(frameId,
                       SbExpr::makeSeq(std::move(lhs), std::move(rhs), std::move(powFunctionCall)),
                       std::move(checkPowRes)));
    }
    void visit(const ExpressionRange* expr) final {
        auto outerFrameId = _context->state.frameId();
        SbVar startVar{outerFrameId, 0};
        SbVar endVar{outerFrameId, 1};
        SbVar stepVar{outerFrameId, 2};

        auto innerFrameId = _context->state.frameId();
        SbVar convertedStartVar{innerFrameId, 0};
        SbVar convertedEndVar{innerFrameId, 1};
        SbVar convertedStepVar{innerFrameId, 2};

        auto step = expr->getChildren().size() == 3 ? popExpr() : _b.makeInt32Constant(1);
        auto end = popExpr();
        auto start = popExpr();

        auto rangeExpr = _b.makeLet(
            outerFrameId,
            SbExpr::makeSeq(std::move(start), std::move(end), std::move(step)),
            _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNonNumericCheck(startVar),
                               _b.makeFail(ErrorCodes::Error{7157711},
                                           "$range only supports numeric types for start")},
                    SbExprPair{_b.generateNonNumericCheck(endVar),
                               _b.makeFail(ErrorCodes::Error{7157712},
                                           "$range only supports numeric types for end")},
                    SbExprPair{_b.generateNonNumericCheck(stepVar),
                               _b.makeFail(ErrorCodes::Error{7157713},
                                           "$range only supports numeric types for step")}),
                _b.makeLet(
                    innerFrameId,
                    SbExpr::makeSeq(_b.makeFunction("convert",
                                                    startVar,
                                                    _b.makeInt32Constant(static_cast<int32_t>(
                                                        sbe::value::TypeTags::NumberInt32))),
                                    _b.makeFunction("convert",
                                                    endVar,
                                                    _b.makeInt32Constant(static_cast<int32_t>(
                                                        sbe::value::TypeTags::NumberInt32))),
                                    _b.makeFunction("convert",
                                                    stepVar,
                                                    _b.makeInt32Constant(static_cast<int32_t>(
                                                        sbe::value::TypeTags::NumberInt32)))),
                    _b.buildMultiBranchConditionalFromCaseValuePairs(
                        SbExpr::makeExprPairVector(
                            SbExprPair{_b.makeNot(_b.makeFunction("exists", convertedStartVar)),
                                       _b.makeFail(ErrorCodes::Error{7157714},
                                                   "$range start argument cannot be "
                                                   "represented as a 32-bit integer")},
                            SbExprPair{_b.makeNot(_b.makeFunction("exists", convertedEndVar)),
                                       _b.makeFail(ErrorCodes::Error{7157715},
                                                   "$range end argument cannot be represented "
                                                   "as a 32-bit integer")},
                            SbExprPair{_b.makeNot(_b.makeFunction("exists", convertedStepVar)),
                                       _b.makeFail(ErrorCodes::Error{7157716},
                                                   "$range step argument cannot be "
                                                   "represented as a 32-bit integer")},
                            SbExprPair{_b.makeBinaryOp(abt::Operations::Eq,
                                                       convertedStepVar,
                                                       _b.makeInt32Constant(0)),
                                       _b.makeFail(ErrorCodes::Error{7157717},
                                                   "$range requires a non-zero step value")}),
                        _b.makeFunction("newArrayFromRange",
                                        convertedStartVar,
                                        convertedEndVar,
                                        convertedStepVar)))));

        pushExpr(std::move(rangeExpr));
    }

    void visit(const ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(const ExpressionReplaceOne* expr) final {
        unsupportedExpression(expr->getOpName());

        // TODO(SERVER-108244): Update code below to have replaceOne support regex in SBE.
        _context->ensureArity(3);

        auto replacementArg = popExpr();
        auto findArg = popExpr();
        auto inputArg = popExpr();

        auto frameId = _context->state.frameId();
        SbVar replacementArgVar{frameId, 0};
        SbVar findArgVar{frameId, 1};
        SbVar inputArgVar{frameId, 2};
        SbVar replacementArgNullVar{frameId, 3};
        SbVar findArgNullVar{frameId, 4};
        SbVar inputArgNullVar{frameId, 5};

        auto checkNull =
            _b.makeBinaryOp(abt::Operations::Or,
                            _b.makeBinaryOp(abt::Operations::Or, inputArgNullVar, findArgNullVar),
                            replacementArgNullVar);

        // Check if find string is empty, and if so return the the concatenation of the replacement
        // string and the input string, otherwise replace the first occurrence of the find string.
        auto isEmptyFindStr =
            _b.makeBinaryOp(abt::Operations::Eq, findArgVar, _b.makeStrConstant(""_sd));

        auto generateTypeCheckCaseValuePair = [&](SbVar paramVar,
                                                  SbVar paramIsNullVar,
                                                  StringData param) {
            return SbExprPair{_b.makeNot(_b.makeBinaryOp(abt::Operations::Or,
                                                         paramIsNullVar,
                                                         _b.makeFunction("isString", paramVar))),
                              _b.makeFail(ErrorCodes::Error{7158302},
                                          str::stream() << "$replaceOne requires that '" << param
                                                        << "' be a string")};
        };

        // Order here is important because we want to preserve the precedence of failures in MQL.
        auto replaceOneExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                generateTypeCheckCaseValuePair(inputArgVar, inputArgNullVar, "input"),
                generateTypeCheckCaseValuePair(findArgVar, findArgNullVar, "find"),
                generateTypeCheckCaseValuePair(
                    replacementArgVar, replacementArgNullVar, "replacement"),
                SbExprPair{std::move(checkNull), _b.makeNullConstant()}),
            _b.makeIf(std::move(isEmptyFindStr),
                      _b.makeFunction("concat", replacementArgVar, inputArgVar),
                      _b.makeFunction("replaceOne", inputArgVar, findArgVar, replacementArgVar)));

        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(replacementArg),
                                            std::move(findArg),
                                            std::move(inputArg),
                                            _b.generateNullMissingOrUndefined(replacementArgVar),
                                            _b.generateNullMissingOrUndefined(findArgVar),
                                            _b.generateNullMissingOrUndefined(inputArgVar)),
                            std::move(replaceOneExpr)));
    }

    void visit(const ExpressionReplaceAll* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSetDifference* expr) final {
        invariant(expr->getChildren().size() == 2);

        generateSetExpression(expr, SetOperation::Difference);
    }
    void visit(const ExpressionSetEquals* expr) final {
        invariant(expr->getChildren().size() >= 2);

        generateSetExpression(expr, SetOperation::Equals);
    }
    void visit(const ExpressionSetIntersection* expr) final {
        if (expr->getChildren().size() == 0) {
            auto [emptySetTag, emptySetValue] = sbe::value::makeNewArraySet();
            pushExpr(_b.makeConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Intersection);
    }
    void visit(const ExpressionSetIsSubset* expr) final {
        tassert(5154700,
                "$setIsSubset expects two expressions in the input",
                expr->getChildren().size() == 2);

        generateSetExpression(expr, SetOperation::IsSubset);
    }
    void visit(const ExpressionSetUnion* expr) final {
        if (expr->getChildren().size() == 0) {
            auto [emptySetTag, emptySetValue] = sbe::value::makeNewArraySet();
            pushExpr(_b.makeConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Union);
    }

    void visit(const ExpressionSimilarityDotProduct* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSimilarityCosine* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSimilarityEuclidean* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionReverseArray* expr) final {
        auto arg = popExpr();
        auto frameId = _context->state.frameId();
        SbVar var{frameId, 0};

        auto argumentIsNotArray = _b.makeNot(_b.makeFunction("isArray", var));

        auto exprReverseArr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(var), _b.makeNullConstant()},
                SbExprPair{std::move(argumentIsNotArray),
                           _b.makeFail(ErrorCodes::Error{7158002},
                                       "$reverseArray argument must be an array")}),
            _b.makeFunction("reverseArray", var));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(exprReverseArr)));
    }

    void visit(const ExpressionSortArray* expr) final {
        auto arg = popExpr();
        auto frameId = _context->state.frameId();
        SbVar var{frameId, 0};

        auto [specTag, specVal] = makeValue(expr->getSortPattern());
        auto specConstant = _b.makeConstant(specTag, specVal);

        auto argumentIsNotArray = _b.makeNot(_b.makeFunction("isArray", var));

        auto functionArgs = SbExpr::makeSeq(var, std::move(specConstant));

        auto collatorSlot = _context->state.getCollatorSlot();
        if (collatorSlot) {
            functionArgs.emplace_back(SbVar{*collatorSlot});
        }

        auto exprSortArr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(var), _b.makeNullConstant()},
                SbExprPair{std::move(argumentIsNotArray),
                           _b.makeFail(ErrorCodes::Error{7158001},
                                       "$sortArray input argument must be an array")}),
            _b.makeFunction("sortArray", std::move(functionArgs)));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(exprSortArr)));
    }

    void visit(const ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionIsArray* expr) final {
        pushExpr(_b.makeFillEmptyFalse(_b.makeFunction("isArray", popExpr())));
    }
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionRound* expr) final {
        visitRoundTruncExpression(expr);
    }
    void visit(const ExpressionSplit* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto [arrayWithEmptyStringTag, arrayWithEmptyStringVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrayWithEmptyStringGuard{arrayWithEmptyStringTag,
                                                         arrayWithEmptyStringVal};
        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        sbe::value::getArrayView(arrayWithEmptyStringVal)->push_back(emptyStrTag, emptyStrVal);

        auto delimiter = popExpr();
        auto stringExpression = popExpr();

        auto frameId = _context->state.frameId();
        SbVar varString{frameId, 0};
        SbVar varDelimiter{frameId, 1};

        auto emptyResult = _b.makeConstant(arrayWithEmptyStringTag, arrayWithEmptyStringVal);
        arrayWithEmptyStringGuard.reset();

        // In order to maintain MQL semantics, first check both the string expression
        // (first agument), and delimiter string (second argument) for null, undefined, or
        // missing, and if either is nullish make the entire expression return null. Only
        // then make further validity checks against the input. Fail if the delimiter is an
        // empty string. Return [""] if the string expression is an empty string.
        auto totalSplitFunc = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.makeBinaryOp(abt::Operations::Or,
                                           _b.generateNullMissingOrUndefined(varString),
                                           _b.generateNullMissingOrUndefined(varDelimiter)),
                           _b.makeNullConstant()},
                SbExprPair{_b.makeNot(_b.makeFunction("isString"_sd, varString)),
                           _b.makeFail(ErrorCodes::Error{7158202},
                                       "$split string expression must be a string")},
                SbExprPair{
                    _b.makeNot(_b.makeFunction("isString"_sd, varDelimiter)),
                    _b.makeFail(ErrorCodes::Error{7158203}, "$split delimiter must be a string")},
                SbExprPair{
                    _b.makeBinaryOp(abt::Operations::Eq, varDelimiter, _b.makeStrConstant(""_sd)),
                    _b.makeFail(ErrorCodes::Error{7158204},
                                "$split delimiter must not be an empty string")},
                SbExprPair{
                    _b.makeBinaryOp(abt::Operations::Eq, varString, _b.makeStrConstant(""_sd)),
                    std::move(emptyResult)}),
            _b.makeFunction("split"_sd, varString, varDelimiter));

        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(stringExpression), std::move(delimiter)),
                            std::move(totalSplitFunc)));
    }
    void visit(const ExpressionSqrt* expr) final {
        auto frameId = _context->state.frameId();
        SbVar inputVar{frameId, 0};

        auto sqrtExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar), _b.makeNullConstant()},
                SbExprPair{
                    _b.generateNonNumericCheck(inputVar),
                    _b.makeFail(ErrorCodes::Error{7157709}, "$sqrt only supports numeric types")},
                SbExprPair{_b.generateNegativeCheck(inputVar),
                           _b.makeFail(ErrorCodes::Error{7157710},
                                       "$sqrt's argument must be greater than or equal to 0")}),
            _b.makeFunction("sqrt", inputVar));

        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(popExpr()), std::move(sqrtExpr)));
    }
    void visit(const ExpressionStrcasecmp* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        generateStringCaseConversionExpression(_context, "toUpper");
        SbExpr rhs = popExpr();
        generateStringCaseConversionExpression(_context, "toUpper");
        SbExpr lhs = popExpr();

        pushExpr(_b.makeBinaryOp(abt::Operations::Cmp3w, std::move(lhs), std::move(rhs)));
    }
    void visit(const ExpressionSubstrBytes* expr) final {
        invariant(expr->getChildren().size() == 3);
        _context->ensureArity(3);

        SbExpr byteCount = popExpr();
        SbExpr startIndex = popExpr();
        SbExpr stringExpr = popExpr();

        auto frameId = _context->state.frameId();
        SbVar byteCountVar{frameId, 0};
        SbVar startIndexVar{frameId, 1};
        SbVar stringExprVar{frameId, 2};

        SbExpr::Vector functionArgs;

        SbExpr validStringExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(stringExprVar),
                           _b.makeStrConstant(""_sd)},
                SbExprPair{
                    _b.makeFillEmptyTrue(_b.makeFunction("coerceToString", stringExprVar)),
                    _b.makeFail(
                        ErrorCodes::Error(5155608),
                        "$substrBytes: string expression could not be resolved to a string")}),
            _b.makeFunction("coerceToString", stringExprVar));
        functionArgs.push_back(std::move(validStringExpr));

        SbExpr validStartIndexExpr = _b.makeIf(
            _b.makeBinaryOp(
                abt::Operations::Or,
                _b.generateNullMissingOrUndefined(startIndexVar),
                _b.makeBinaryOp(
                    abt::Operations::Or,
                    _b.generateNonNumericCheck(startIndexVar),
                    _b.makeBinaryOp(abt::Operations::Lt, startIndexVar, _b.makeInt32Constant(0)))),
            _b.makeFail(ErrorCodes::Error{5155603},
                        "Starting index must be non-negative numeric type"),
            _b.makeFunction(
                "convert",
                startIndexVar,
                _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
        functionArgs.push_back(std::move(validStartIndexExpr));

        SbExpr validLengthExpr = _b.makeIf(
            _b.makeBinaryOp(abt::Operations::Or,
                            _b.generateNullMissingOrUndefined(byteCountVar),
                            _b.generateNonNumericCheck(byteCountVar)),
            _b.makeFail(ErrorCodes::Error{5155602}, "Length must be a numeric type"),
            _b.makeFunction(
                "convert",
                byteCountVar,
                _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
        functionArgs.push_back(std::move(validLengthExpr));

        pushExpr(_b.makeLet(
            frameId,
            SbExpr::makeSeq(std::move(byteCount), std::move(startIndex), std::move(stringExpr)),
            _b.makeFunction("substrBytes", std::move(functionArgs))));
    }
    void visit(const ExpressionSubstrCP* expr) final {
        invariant(expr->getChildren().size() == 3);
        _context->ensureArity(3);

        SbExpr len = popExpr();
        SbExpr startIndex = popExpr();
        SbExpr stringExpr = popExpr();

        auto frameId = _context->state.frameId();
        SbVar lenVar{frameId, 0};
        SbVar startIndexVar{frameId, 1};
        SbVar stringExprVar{frameId, 2};

        SbExpr::Vector functionArgs;

        SbExpr validStringExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(stringExprVar),
                           _b.makeStrConstant(""_sd)},
                SbExprPair{_b.makeFillEmptyTrue(_b.makeFunction("coerceToString", stringExprVar)),
                           _b.makeFail(ErrorCodes::Error(5155708),
                                       "$substrCP: string expression could not be resolved to a "
                                       "string")}),
            _b.makeFunction("coerceToString", stringExprVar));
        functionArgs.push_back(std::move(validStringExpr));

        SbExpr validStartIndexExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{
                    _b.generateNullishOrNotRepresentableInt32Check(startIndexVar),
                    _b.makeFail(ErrorCodes::Error{5155700},
                                "$substrCP: starting index must be numeric type representable as a "
                                "32-bit integral value")},
                SbExprPair{
                    _b.makeBinaryOp(abt::Operations::Lt, startIndexVar, _b.makeInt32Constant(0)),
                    _b.makeFail(ErrorCodes::Error{5155701},
                                "$substrCP: starting index must be a non-negative integer")}),
            _b.makeFunction(
                "convert",
                startIndexVar,
                _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt32))));
        functionArgs.push_back(std::move(validStartIndexExpr));

        SbExpr validLengthExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullishOrNotRepresentableInt32Check(lenVar),
                           _b.makeFail(ErrorCodes::Error{5155702},
                                       "$substrCP: length must be numeric type representable as "
                                       "a 32-bit integral value")},
                SbExprPair{_b.makeBinaryOp(abt::Operations::Lt, lenVar, _b.makeInt32Constant(0)),
                           _b.makeFail(ErrorCodes::Error{5155703},
                                       "$substrCP: length must be a non-negative integer")}),
            _b.makeFunction(
                "convert",
                lenVar,
                _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt32))));
        functionArgs.push_back(std::move(validLengthExpr));

        pushExpr(_b.makeLet(
            frameId,
            SbExpr::makeSeq(std::move(len), std::move(startIndex), std::move(stringExpr)),
            _b.makeFunction("substrCP", std::move(functionArgs))));
    }
    void visit(const ExpressionStrLenBytes* expr) final {
        tassert(5155802, "expected 'expr' to have 1 child", expr->getChildren().size() == 1);
        _context->ensureArity(1);

        auto frameId = _context->state.frameId();
        SbVar strVar{frameId, 0};

        SbExpr strExpression = popExpr();

        auto strLenBytesExpr = _b.makeIf(
            _b.makeFillEmptyFalse(_b.makeFunction("isString", strVar)),
            _b.makeFunction("strLenBytes", strVar),
            _b.makeFail(ErrorCodes::Error{5155800}, "$strLenBytes requires a string argument"));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(strExpression)), std::move(strLenBytesExpr)));
    }
    void visit(const ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionStrLenCP* expr) final {
        tassert(5155902, "expected 'expr' to have 1 child", expr->getChildren().size() == 1);
        _context->ensureArity(1);

        auto frameId = _context->state.frameId();
        SbVar strVar{frameId, 0};
        SbExpr strExpression = popExpr();

        auto strLenCPExpr = _b.makeIf(
            _b.makeFillEmptyFalse(_b.makeFunction("isString", strVar)),
            _b.makeFunction("strLenCP", strVar),
            _b.makeFail(ErrorCodes::Error{5155900}, "$strLenCP requires a string argument"));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(strExpression)), std::move(strLenCPExpr)));
    }
    void visit(const ExpressionSubtract* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto rhs = popExpr();
        auto lhs = popExpr();

        auto frameId = _context->state.frameId();
        SbVar lhsVar{frameId, 0};
        SbVar rhsVar{frameId, 1};

        auto checkNullArguments = _b.makeBinaryOp(abt::Operations::Or,
                                                  _b.generateNullMissingOrUndefined(lhsVar),
                                                  _b.generateNullMissingOrUndefined(rhsVar));

        auto checkArgumentTypes = _b.makeNot(
            _b.makeIf(_b.makeFunction("isNumber", lhsVar),
                      _b.makeFunction("isNumber", rhsVar),
                      _b.makeBinaryOp(abt::Operations::And,
                                      _b.makeFunction("isDate", lhsVar),
                                      _b.makeBinaryOp(abt::Operations::Or,
                                                      _b.makeFunction("isNumber", rhsVar),
                                                      _b.makeFunction("isDate", rhsVar)))));

        auto subtractOp = _b.makeBinaryOp(abt::Operations::Sub, lhsVar, rhsVar);
        auto subtractExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{std::move(checkNullArguments), _b.makeNullConstant()},
                SbExprPair{
                    std::move(checkArgumentTypes),
                    _b.makeFail(
                        ErrorCodes::Error{7157720},
                        "Only numbers and dates are allowed in an $subtract expression. To "
                        "subtract a number from a date, the date must be the first argument.")}),
            std::move(subtractOp));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(lhs), std::move(rhs)), std::move(subtractExpr)));
    }
    void visit(const ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionTestApiVersion* expr) final {
        pushExpr(_b.makeInt32Constant(1));
    }
    void visit(const ExpressionTestFeatureFlagLatest* expr) final {
        pushExpr(_b.makeInt32Constant(1));
    }
    void visit(const ExpressionTestFeatureFlagLastLTS* expr) final {
        pushExpr(_b.makeInt32Constant(1));
    }
    void visit(const ExpressionToLower* expr) final {
        generateStringCaseConversionExpression(_context, "toLower");
    }
    void visit(const ExpressionToUpper* expr) final {
        generateStringCaseConversionExpression(_context, "toUpper");
    }
    void visit(const ExpressionTrim* expr) final {
        tassert(5156301,
                "trim expressions must have spots in their children vector for 'input' and "
                "'chars' fields",
                expr->getChildren().size() == 2);

        auto numProvidedArgs = 1;
        if (expr->hasCharactersExpr()) {
            // 'chars' is not null
            ++numProvidedArgs;
        }

        _context->ensureArity(numProvidedArgs);
        auto isCharsProvided = numProvidedArgs == 2;

        auto frameId = _context->state.frameId();
        SbVar inputVar{frameId, 0};
        SbVar charsVar{frameId, 1};

        auto charsString = isCharsProvided ? popExpr() : _b.makeNullConstant();
        auto inputString = popExpr();
        auto trimBuiltinName = expr->getTrimTypeString();

        auto checkCharsNullish = isCharsProvided ? _b.generateNullMissingOrUndefined(charsVar)
                                                 : _b.makeBoolConstant(false);

        auto checkCharsNotString = isCharsProvided
            ? _b.makeNot(_b.makeFunction("isString"_sd, charsVar))
            : _b.makeBoolConstant(false);

        /*
           Trim Functionality (invariant that 'input' has been provided, otherwise would've failed
           at parse time)

           if ('input' is nullish) {
                -> return null
           }
           else if ('input' is not a string) {
                ->  fail with error code 5156302
           }
           else if ('chars' is provided and nullish) {
                -> return null
           }
           else if ('chars' is provided but is not a string) {
                ->  fail with error code 5156303
           }
           else {
                -> make a function for the correct $trim variant with 'input' and 'chars'
                   parameters
           }
        */
        auto trimFunc = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar), _b.makeNullConstant()},
                SbExprPair{
                    _b.makeNot(_b.makeFunction("isString"_sd, inputVar)),
                    _b.makeFail(ErrorCodes::Error{5156302},
                                "$" + trimBuiltinName + " input expression must be a string")},
                SbExprPair{std::move(checkCharsNullish), _b.makeNullConstant()},
                SbExprPair{std::move(checkCharsNotString),
                           _b.makeFail(ErrorCodes::Error{5156303},
                                       "$" + trimBuiltinName +
                                           " chars expression must be a string if provided")}),
            _b.makeFunction(trimBuiltinName, inputVar, charsVar));

        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(inputString), std::move(charsString)),
                            std::move(trimFunc)));
    }
    void visit(const ExpressionTrunc* expr) final {
        visitRoundTruncExpression(expr);
    }
    void visit(const ExpressionType* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSubtype* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionZip* expr) final {
        unsupportedExpression("$zip");
    }
    void visit(const ExpressionConvert* expr) final {
        unsupportedExpression("$convert");
    }
    void visit(const ExpressionRegexFind* expr) final {
        generateRegexExpression(expr, "regexFind");
    }
    void visit(const ExpressionRegexFindAll* expr) final {
        generateRegexExpression(expr, "regexFindAll");
    }
    void visit(const ExpressionRegexMatch* expr) final {
        generateRegexExpression(expr, "regexMatch");
    }
    void visit(const ExpressionCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "cos", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(const ExpressionSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "sin", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(const ExpressionTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "tan", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(const ExpressionArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acos", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(const ExpressionArcSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "asin", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(const ExpressionArcTangent* expr) final {
        generateTrigonometricExpression("atan");
    }
    void visit(const ExpressionArcTangent2* expr) final {
        generateTrigonometricExpressionBinary("atan2");
    }
    void visit(const ExpressionHyperbolicArcTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "atanh", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(const ExpressionHyperbolicArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acosh", DoubleBound(1.0, true), DoubleBound::plusInfinityInclusive());
    }
    void visit(const ExpressionHyperbolicArcSine* expr) final {
        generateTrigonometricExpression("asinh");
    }
    void visit(const ExpressionHyperbolicCosine* expr) final {
        generateTrigonometricExpression("cosh");
    }
    void visit(const ExpressionHyperbolicSine* expr) final {
        generateTrigonometricExpression("sinh");
    }
    void visit(const ExpressionHyperbolicTangent* expr) final {
        generateTrigonometricExpression("tanh");
    }
    void visit(const ExpressionDegreesToRadians* expr) final {
        generateTrigonometricExpression("degreesToRadians");
    }
    void visit(const ExpressionRadiansToDegrees* expr) final {
        generateTrigonometricExpression("radiansToDegrees");
    }
    void visit(const ExpressionDayOfMonth* expr) final {
        generateDateExpressionAcceptingTimeZone("dayOfMonth", expr);
    }
    void visit(const ExpressionDayOfWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("dayOfWeek", expr);
    }
    void visit(const ExpressionDayOfYear* expr) final {
        generateDateExpressionAcceptingTimeZone("dayOfYear", expr);
    }
    void visit(const ExpressionHour* expr) final {
        generateDateExpressionAcceptingTimeZone("hour", expr);
    }
    void visit(const ExpressionMillisecond* expr) final {
        generateDateExpressionAcceptingTimeZone("millisecond", expr);
    }
    void visit(const ExpressionMinute* expr) final {
        generateDateExpressionAcceptingTimeZone("minute", expr);
    }
    void visit(const ExpressionMonth* expr) final {
        generateDateExpressionAcceptingTimeZone("month", expr);
    }
    void visit(const ExpressionSecond* expr) final {
        generateDateExpressionAcceptingTimeZone("second", expr);
    }
    void visit(const ExpressionWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("week", expr);
    }
    void visit(const ExpressionIsoWeekYear* expr) final {
        generateDateExpressionAcceptingTimeZone("isoWeekYear", expr);
    }
    void visit(const ExpressionIsoDayOfWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("isoDayOfWeek", expr);
    }
    void visit(const ExpressionIsoWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("isoWeek", expr);
    }
    void visit(const ExpressionYear* expr) final {
        generateDateExpressionAcceptingTimeZone("year", expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        if (arity == 0) {
            pushExpr(_b.makeNullConstant());
        } else if (arity == 1) {
            SbExpr singleInput = popExpr();
            auto frameId = _context->state.frameId();
            SbVar singleInputVar{frameId, 0};

            SbExpr avgOfArrayExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNullMissingOrUndefined(singleInputVar),
                               _b.makeNullConstant()},
                    SbExprPair{_b.makeFunction("isArray", singleInputVar),
                               _b.makeFillEmptyNull(_b.makeFunction("avgOfArray", singleInputVar))},
                    SbExprPair{_b.makeFunction("isNumber", singleInputVar), singleInputVar}),
                _b.makeNullConstant());

            pushExpr(_b.makeLet(
                frameId, SbExpr::makeSeq(std::move(singleInput)), std::move(avgOfArrayExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "avgOfArray");
        }
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        visitMaxMinFunction(expr, _context, "maxOfArray");
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        visitMaxMinFunction(expr, _context, "minOfArray");
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            pushExpr(_b.makeNullConstant());
        } else if (arity == 1) {
            SbExpr singleInput = popExpr();

            auto frameId = _context->state.frameId();
            SbVar singleInputVar{frameId, 0};

            SbExpr stdDevPopExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNullMissingOrUndefined(singleInputVar),
                               _b.makeNullConstant()},
                    SbExprPair{_b.makeFunction("isArray", singleInputVar),
                               _b.makeFillEmptyNull(_b.makeFunction("stdDevPop", singleInputVar))},
                    SbExprPair{
                        _b.makeFunction("isNumber", singleInputVar),
                        // Population standard deviation for a single numeric input is always 0.
                        _b.makeInt32Constant(0)}),
                _b.makeNullConstant());

            pushExpr(_b.makeLet(
                frameId, SbExpr::makeSeq(std::move(singleInput)), std::move(stdDevPopExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "stdDevPop");
        }
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            pushExpr(_b.makeNullConstant());
        } else if (arity == 1) {
            SbExpr singleInput = popExpr();

            auto frameId = _context->state.frameId();
            SbVar singleInputVar{frameId, 0};

            SbExpr stdDevSampExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNullMissingOrUndefined(singleInputVar),
                               _b.makeNullConstant()},
                    SbExprPair{
                        _b.makeFunction("isArray", singleInputVar),
                        _b.makeFillEmptyNull(_b.makeFunction("stdDevSamp", singleInputVar))}),
                // Sample standard deviation is undefined for a single input.
                _b.makeNullConstant());

            pushExpr(_b.makeLet(
                frameId, SbExpr::makeSeq(std::move(singleInput)), std::move(stdDevSampExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "stdDevSamp");
        }
    }
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        if (arity == 0) {
            pushExpr(_b.makeNullConstant());
        } else if (arity == 1) {
            SbExpr singleInput = popExpr();

            auto frameId = _context->state.frameId();
            SbVar singleInputVar{frameId, 0};

            // $sum returns 0 if the operand is missing, undefined, or non-numeric.
            SbExpr sumOfArrayExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNullMissingOrUndefined(singleInputVar),
                               _b.makeInt32Constant(0)},
                    SbExprPair{_b.makeFunction("isArray", singleInputVar),
                               _b.makeFillEmptyNull(_b.makeFunction("sumOfArray", singleInputVar))},
                    SbExprPair{_b.makeFunction("isNumber", singleInputVar), singleInputVar}),
                _b.makeInt32Constant(0));

            pushExpr(_b.makeLet(
                frameId, SbExpr::makeSeq(std::move(singleInput)), std::move(sumOfArrayExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "sumOfArray");
        }
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionTests::Testable* expr) final {
        unsupportedExpression("$test");
    }
    void visit(const ExpressionInternalJsEmit* expr) final {
        unsupportedExpression("$internalJsEmit");
    }
    void visit(const ExpressionInternalFindSlice* expr) final {
        unsupportedExpression("$internalFindSlice");
    }
    void visit(const ExpressionInternalFindPositional* expr) final {
        unsupportedExpression("$internalFindPositional");
    }
    void visit(const ExpressionInternalFindElemMatch* expr) final {
        unsupportedExpression("$internalFindElemMatch");
    }
    void visit(const ExpressionFunction* expr) final {
        unsupportedExpression("$function");
    }

    void visit(const ExpressionRandom* expr) final {
        uassert(
            5155201, "$rand does not currently accept arguments", expr->getChildren().size() == 0);
        auto expression = _b.makeFunction("rand");
        pushExpr(std::move(expression));
    }

    void visit(const ExpressionCurrentDate* expr) final {
        uassert(9940500,
                "$currentDate does not currently accept arguments",
                expr->getChildren().size() == 0);
        auto expression = _b.makeFunction("currentDate");
        pushExpr(std::move(expression));
    }

    void visit(const ExpressionToHashedIndexKey* expr) final {
        unsupportedExpression("$toHashedIndexKey");
    }

    void visit(const ExpressionDateAdd* expr) final {
        generateDateArithmeticsExpression(expr, "dateAdd");
    }

    void visit(const ExpressionDateSubtract* expr) final {
        generateDateArithmeticsExpression(expr, "dateSubtract");
    }

    void visit(const ExpressionGetField* expr) final {
        unsupportedExpression("$getField");
    }

    void visit(const ExpressionSetField* expr) final {
        unsupportedExpression("$setField");
    }

    void visit(const ExpressionCreateUUID* expr) final {
        // TODO(SERVER-101161): Support $createUUID in SBE.
        unsupportedExpression("$createUUID");
    }

    void visit(const ExpressionCreateObjectId* expr) final {
        // TODO(SERVER-107710): Support $createObjectId in SBE.
        unsupportedExpression("$createObjectId");
    }

    void visit(const ExpressionTsSecond* expr) final {
        _context->ensureArity(1);

        auto arg = popExpr();

        auto frameId = _context->state.frameId();
        SbVar var{frameId, 0};

        auto tsSecondExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(var), _b.makeNullConstant()},
                SbExprPair{_b.generateNonTimestampCheck(var),
                           _b.makeFail(ErrorCodes::Error{7157900},
                                       str::stream() << expr->getOpName()
                                                     << " expects argument of type timestamp")}),
            _b.makeFunction("tsSecond", var));
        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(tsSecondExpr)));
    }

    void visit(const ExpressionTsIncrement* expr) final {
        _context->ensureArity(1);

        auto arg = popExpr();

        auto frameId = _context->state.frameId();
        SbVar var{frameId, 0};

        auto tsIncrementExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(var), _b.makeNullConstant()},
                SbExprPair{_b.generateNonTimestampCheck(var),
                           _b.makeFail(ErrorCodes::Error{7157901},
                                       str::stream() << expr->getOpName()
                                                     << " expects argument of type timestamp")}),
            _b.makeFunction("tsIncrement", var));
        pushExpr(_b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(tsIncrementExpr)));
    }

    void visit(const ExpressionInternalOwningShard* expr) final {
        unsupportedExpression("$_internalOwningShard");
    }

    void visit(const ExpressionInternalIndexKey* expr) final {
        unsupportedExpression("$_internalIndexKey");
    }

    void visit(const ExpressionInternalKeyStringValue* expr) final {
        unsupportedExpression(expr->getOpName());
    }

private:
    /**
     * Shared logic for $round and $trunc expressions
     */
    template <typename ExprType>
    void visitRoundTruncExpression(const ExprType* expr) {
        const std::string opName(expr->getOpName());
        invariant(opName == "$round" || opName == "$trunc");

        const auto& children = expr->getChildren();
        invariant(children.size() == 1 || children.size() == 2);
        const bool hasPlaceArg = (children.size() == 2);
        _context->ensureArity(children.size());

        auto frameId = _context->state.frameId();
        SbVar inputNumVar{frameId, 0};
        SbVar inputPlaceVar{frameId, 1};

        // We always need to validate the number parameter, since it will always exist.
        std::vector<SbExprPair> inputValidationCases = SbExpr::makeExprPairVector(
            generateReturnNullIfNullMissingOrUndefined(inputNumVar),
            SbExprPair{
                _b.generateNonNumericCheck(inputNumVar),
                _b.makeFail(ErrorCodes::Error{5155300}, opName + " only supports numeric types")});

        // Only add these cases if we have a "place" argument.
        if (hasPlaceArg) {
            inputValidationCases.emplace_back(
                generateReturnNullIfNullMissingOrUndefined(inputPlaceVar));
            inputValidationCases.emplace_back(_b.generateInvalidRoundPlaceArgCheck(inputPlaceVar),
                                              _b.makeFail(ErrorCodes::Error{5155301},
                                                          opName +
                                                              " requires \"place\" argument to be "
                                                              "an integer between -20 and 100"));
        }

        SbExpr abtExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases),
            _b.makeFunction(
                (opName == "$round" ? "round"_sd : "trunc"_sd), inputNumVar, inputPlaceVar));

        // "place" argument defaults to 0.
        SbExpr placeExpr = hasPlaceArg ? popExpr() : _b.makeInt32Constant(0);
        SbExpr inputExpr = popExpr();
        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(inputExpr), std::move(placeExpr)),
                            std::move(abtExpr)));
    }

    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(const Expression* expr, abt::Operations logicOp) {
        invariant(logicOp == abt::Operations::And || logicOp == abt::Operations::Or);

        size_t numChildren = expr->getChildren().size();
        if (numChildren == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value:
            // true and false, respectively.
            auto logicIdentityVal = (logicOp == abt::Operations::And);
            pushExpr(_b.makeBoolConstant(logicIdentityVal));
            return;
        }

        SbExpr::Vector exprs;
        exprs.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            exprs.emplace_back(_b.makeFillEmptyFalse(_b.makeFunction("coerceToBool", popExpr())));
        }
        std::reverse(exprs.begin(), exprs.end());

        pushExpr(_b.makeBooleanOpTree(logicOp, std::move(exprs)));
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(const Expression* expr) {
        // The default case is always the last child in the ExpressionSwitch. If it is unspecified
        // in the user's query, it is a nullptr. In ExpressionCond, the last child is the "else"
        // branch, and it is guaranteed not to be nullptr.
        auto defaultExpr = expr->getChildren().back() != nullptr
            ? popExpr()
            : _b.makeFail(ErrorCodes::Error{7158303},
                          "$switch could not find a matching branch for an "
                          "input, and no default was specified.");

        size_t numCases = expr->getChildren().size() / 2;
        std::vector<SbExprPair> cases;
        cases.reserve(numCases);

        for (size_t i = 0; i < numCases; ++i) {
            auto valueExpr = popExpr();
            auto conditionExpr = _b.makeFillEmptyFalse(_b.makeFunction("coerceToBool", popExpr()));
            cases.emplace_back(std::move(conditionExpr), std::move(valueExpr));
        }

        std::reverse(cases.begin(), cases.end());

        pushExpr(_b.buildMultiBranchConditionalFromCaseValuePairs(std::move(cases),
                                                                  std::move(defaultExpr)));
    }

    void generateDateExpressionAcceptingTimeZone(StringData exprName, const Expression* expr) {
        const auto& children = expr->getChildren();
        invariant(children.size() == 2);

        auto timezoneExpression = children[1] ? popExpr() : _b.makeStrConstant("UTC"_sd);
        auto dateExpression = popExpr();

        auto frameId = _context->state.frameId();

        // Local bind to hold the date expression result
        SbVar dateVar{frameId, 0};
        // Local bind to hold the timezone expression result
        SbVar timezoneVar{frameId, 1};
        // Create a variable to hold the built-in function.
        SbVar funcVar{frameId, 2};

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        SbVar timeZoneDBVar{timeZoneDBSlot};

        // Set parameters for an invocation of the built-in function.
        SbExpr::Vector arguments;
        arguments.push_back(dateVar);

        // Create expressions to check that each argument to the function exists, is not
        // null, and is of the correct type.
        std::vector<SbExprPair> inputValidationCases;
        // Return the evaluation of the function, if it exists.
        inputValidationCases.emplace_back(_b.makeFunction("exists"_sd, funcVar), funcVar);
        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateReturnNullIfNullMissingOrUndefined(dateVar));

        // "timezone" parameter validation.
        if (timezoneExpression.isConstantExpr()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.getConstantValue();
            auto [timezoneDBTag, timezoneDBVal] =
                _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
            auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);
            uassert(5157900,
                    str::stream() << "$" << exprName << " parameter 'timezone' must be a string",
                    sbe::value::isString(timezoneTag));
            uassert(5157901,
                    str::stream() << "$" << exprName
                                  << " parameter 'timezone' must be a valid timezone",
                    sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
            auto [timezoneObjTag, timezoneObjVal] = sbe::value::makeCopyTimeZone(
                sbe::vm::getTimezone(timezoneTag, timezoneVal, timezoneDB));
            auto timezoneConst = _b.makeConstant(timezoneObjTag, timezoneObjVal);
            arguments.push_back(std::move(timezoneConst));
        } else {
            inputValidationCases.push_back(
                generateReturnNullIfNullMissingOrUndefined(timezoneExpression.clone()));
            inputValidationCases.emplace_back(
                _b.generateNonStringCheck(timezoneVar),
                _b.makeFail(ErrorCodes::Error{5157902},
                            str::stream() << "$" << std::string{exprName}
                                          << " parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                _b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                _b.makeFail(ErrorCodes::Error{5157903},
                            str::stream() << "$" << std::string{exprName}
                                          << " parameter 'timezone' must be a valid timezone"));
            arguments.push_back(timeZoneDBVar);
            arguments.push_back(timezoneVar);
        }

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            dateVar, ErrorCodes::Error{5157904}, exprName, "date"_sd));

        pushExpr(_b.makeLet(
            frameId,
            SbExpr::makeSeq(std::move(dateExpression),
                            std::move(timezoneExpression),
                            _b.makeFunction(std::string{exprName}, std::move(arguments))),
            _b.buildMultiBranchConditionalFromCaseValuePairs(std::move(inputValidationCases),
                                                             _b.makeNothingConstant())));
    }

    /**
     * Creates a case/value pair such that an exception is thrown if a value of the parameter
     * denoted by variable 'dateRef' is of a type that is not coercible to a date.
     *
     * dateRef - a variable corresponding to the parameter.
     * errorCode - error code of the type mismatch error.
     * expressionName - a name of an expression the parameter belongs to.
     * parameterName - a name of the parameter corresponding to variable 'dateRef'.
     */
    SbExprPair generateFailIfNotCoercibleToDate(SbVar dateVar,
                                                ErrorCodes::Error errorCode,
                                                StringData expressionName,
                                                StringData parameterName) {
        return {
            _b.makeNot(_b.makeFunction("typeMatch", dateVar, _b.makeInt32Constant(dateTypeMask()))),
            _b.makeFail(errorCode,
                        str::stream() << expressionName << " parameter '" << parameterName
                                      << "' must be coercible to date")};
    }

    /**
     * Creates a case/value pair such that Null value is returned if a value of variable denoted by
     * 'variable' is null, missing, or undefined.
     */
    SbExprPair generateReturnNullIfNullMissingOrUndefined(SbVar var) {
        return {_b.generateNullMissingOrUndefined(var), _b.makeNullConstant()};
    }

    SbExprPair generateReturnNullIfNullMissingOrUndefined(SbExpr expr) {
        return {_b.generateNullMissingOrUndefined(std::move(expr)), _b.makeNullConstant()};
    }

    /**
     * Creates a boolean expression to check if 'variable' is equal to string 'string'.
     */
    SbExpr generateIsEqualToStringCheck(SbVar var, StringData string) {
        return _b.makeBinaryOp(
            abt::Operations::And,
            _b.makeFunction("isString", var),
            _b.makeBinaryOp(abt::Operations::Eq, var, _b.makeStrConstant(string)));
    }

    /**
     * Shared expression building logic for trignometric expressions to make sure the operand
     * is numeric and is not null.
     */
    void generateTrigonometricExpression(StringData exprName) {
        auto arg = popExpr();

        auto frameId = _context->state.frameId();
        SbVar argVar{frameId, 0};

        auto genericTrigonometricExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(argVar), _b.makeNullConstant()},
                SbExprPair{_b.makeFunction("isNumber", argVar), _b.makeFunction(exprName, argVar)}),
            _b.makeFail(ErrorCodes::Error{7157800},
                        str::stream()
                            << "$" << std::string{exprName} << " supports only numeric types"));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(arg)), std::move(genericTrigonometricExpr)));
    }

    /**
     * Shared expression building logic for binary trigonometric expressions to make sure the
     * operands are numeric and are not null.
     */
    void generateTrigonometricExpressionBinary(StringData exprName) {
        _context->ensureArity(2);
        auto rhs = popExpr();
        auto lhs = popExpr();

        auto frameId = _context->state.frameId();
        SbVar lhsVar{frameId, 0};
        SbVar rhsVar{frameId, 1};

        auto checkNullOrMissing = _b.makeBinaryOp(abt::Operations::Or,
                                                  _b.generateNullMissingOrUndefined(lhsVar),
                                                  _b.generateNullMissingOrUndefined(rhsVar));

        auto checkIsNumber = _b.makeBinaryOp(abt::Operations::And,
                                             _b.makeFunction("isNumber", lhsVar),
                                             _b.makeFunction("isNumber", rhsVar));

        auto genericTrigonometricExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{std::move(checkNullOrMissing), _b.makeNullConstant()},
                SbExprPair{std::move(checkIsNumber), _b.makeFunction(exprName, lhsVar, rhsVar)}),
            _b.makeFail(ErrorCodes::Error{7157801},
                        str::stream() << "$" << exprName << " supports only numeric types"));

        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(lhs), std::move(rhs)),
                            std::move(genericTrigonometricExpr)));
    }

    /**
     * Shared expression building logic for trignometric expressions with bounds for the valid
     * values of the argument.
     */
    void generateTrigonometricExpressionWithBounds(StringData exprName,
                                                   const DoubleBound& lowerBound,
                                                   const DoubleBound& upperBound) {
        auto arg = popExpr();

        auto frameId = _context->state.frameId();
        SbVar argVar{frameId, 0};

        abt::Operations lowerCmp =
            lowerBound.inclusive ? abt::Operations::Gte : abt::Operations::Gt;
        abt::Operations upperCmp =
            upperBound.inclusive ? abt::Operations::Lte : abt::Operations::Lt;
        auto checkBounds = _b.makeBinaryOp(
            abt::Operations::And,
            _b.makeBinaryOp(lowerCmp, argVar, _b.makeDoubleConstant(lowerBound.bound)),
            _b.makeBinaryOp(upperCmp, argVar, _b.makeDoubleConstant(upperBound.bound)));

        auto checkIsNumber = _b.makeFunction("isNumber", argVar);
        auto trigonometricExpr = _b.makeFunction(exprName, argVar);

        auto genericTrigonometricExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(argVar), _b.makeNullConstant()},
                SbExprPair{_b.makeNot(std::move(checkIsNumber)),
                           _b.makeFail(ErrorCodes::Error{7157802},
                                       str::stream() << "$" << std::string{exprName}
                                                     << " supports only numeric types")},
                SbExprPair{_b.generateNaNCheck(argVar), argVar},
                SbExprPair{std::move(checkBounds), std::move(trigonometricExpr)}),
            _b.makeFail(ErrorCodes::Error{7157803},
                        str::stream() << "Cannot apply $" << std::string{exprName}
                                      << ", value must be in " << lowerBound.printLowerBound()
                                      << ", " << upperBound.printUpperBound()));

        pushExpr(_b.makeLet(
            frameId, SbExpr::makeSeq(std::move(arg)), std::move(genericTrigonometricExpr)));
    }

    /*
     * Generates an EExpression that returns an index for $indexOfBytes or $indexOfCP.
     */
    void visitIndexOfFunction(const Expression* expr,
                              ExpressionVisitorContext* _context,
                              const std::string& indexOfFunction) {
        const auto& children = expr->getChildren();
        invariant(children.size() >= 2 && children.size() <= 4);

        // Set up the frame and get arguments from the stack.
        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        SbVar strVar{frameId, numLocalVars++};
        SbVar substrVar{frameId, numLocalVars++};
        SbVar startIndexVar{frameId, numLocalVars++};

        boost::optional<SbVar> endIndexVar;
        SbExpr endIndexExpr;
        if (children.size() >= 4) {
            endIndexVar = SbVar{frameId, numLocalVars++};
            endIndexExpr = popExpr();
        }

        SbExpr startIndexExpr = children.size() >= 3 ? popExpr() : _b.makeInt64Constant(0);
        SbExpr substrExpr = popExpr();
        SbExpr strExpr = popExpr();

        SbExpr::Vector binds =
            SbExpr::makeSeq(std::move(strExpr), std::move(substrExpr), std::move(startIndexExpr));

        if (endIndexVar) {
            binds.emplace_back(std::move(endIndexExpr));
        }

        // Add string and substring binds.
        SbExpr::Vector functionArgs = SbExpr::makeSeq(strVar, substrVar);

        // Add start index operand.
        auto checkValidStartIndex = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullishOrNotRepresentableInt32Check(startIndexVar),
                           _b.makeFail(ErrorCodes::Error{7158003},
                                       str::stream() << "$" << indexOfFunction
                                                     << " start index must resolve to a number")},
                SbExprPair{_b.generateNegativeCheck(startIndexVar),
                           _b.makeFail(ErrorCodes::Error{7158004},
                                       str::stream() << "$" << indexOfFunction
                                                     << " start index must be positive")}),
            _b.makeFunction(
                "convert",
                startIndexVar,
                _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
        functionArgs.push_back(std::move(checkValidStartIndex));

        // Add end index operand.
        if (endIndexVar) {
            auto checkValidEndIndex = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNullishOrNotRepresentableInt32Check(*endIndexVar),
                               _b.makeFail(ErrorCodes::Error{7158005},
                                           str::stream() << "$" << indexOfFunction
                                                         << " end index must resolve to a number")},
                    SbExprPair{_b.generateNegativeCheck(*endIndexVar),
                               _b.makeFail(ErrorCodes::Error{7158006},
                                           str::stream() << "$" << indexOfFunction
                                                         << " end index must be positive")}),
                _b.makeFunction(
                    "convert",
                    *endIndexVar,
                    _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
            functionArgs.push_back(std::move(checkValidEndIndex));
        }

        // Check if string or substring are null or missing before calling indexOfFunction.
        auto resultExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(strVar), _b.makeNullConstant()},
                SbExprPair{_b.generateNonStringCheck(strVar),
                           _b.makeFail(ErrorCodes::Error{7158007},
                                       str::stream()
                                           << "$" << indexOfFunction
                                           << " string must resolve to a string or null")},
                SbExprPair{_b.generateNullMissingOrUndefined(substrVar),
                           _b.makeFail(ErrorCodes::Error{7158008},
                                       str::stream() << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
                SbExprPair{_b.generateNonStringCheck(substrVar),
                           _b.makeFail(ErrorCodes::Error{7158009},
                                       str::stream() << "$" << indexOfFunction
                                                     << " substring must resolve to a string")}),
            _b.makeFunction(indexOfFunction, std::move(functionArgs)));

        // Build local binding tree.
        pushExpr(_b.makeLet(frameId, std::move(binds), std::move(resultExpr)));
    }

    /*
     * Generates an EExpression that returns the maximum for $max and minimum for $min.
     */
    void visitMaxMinFunction(const Expression* expr,
                             ExpressionVisitorContext* _context,
                             const std::string& maxMinFunction) {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            pushExpr(_b.makeNullConstant());
        } else if (arity == 1) {
            SbExpr singleInput = popExpr();

            auto frameId = _context->state.frameId();
            SbVar singleInputVar{frameId, 0};

            SbExpr maxMinExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
                SbExpr::makeExprPairVector(
                    SbExprPair{_b.generateNullMissingOrUndefined(singleInputVar),
                               _b.makeNullConstant()},
                    SbExprPair{
                        _b.makeFunction("isArray", singleInputVar),
                        // In the case of a single argument, if the input is an array, $min or $max
                        // operates on the elements of array to return a single value.
                        _b.makeFillEmptyNull(_b.makeFunction(maxMinFunction, singleInputVar))}),
                singleInputVar);

            pushExpr(_b.makeLet(
                frameId, SbExpr::makeSeq(std::move(singleInput)), std::move(maxMinExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, maxMinFunction);
        }
    }

    /*
     * Converts n > 1 children into an array and generates an EExpression for
     * ExpressionFromAccumulator expressions. Accepts an Expression, ExpressionVisitorContext, and
     * the name of a builtin function.
     */
    void generateExpressionFromAccumulatorExpression(const Expression* expr,
                                                     ExpressionVisitorContext* _context,
                                                     const std::string& functionCall) {
        size_t arity = expr->getChildren().size();

        SbExpr::Vector binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            binds.emplace_back(popExpr());
        }
        std::reverse(std::begin(binds), std::end(binds));

        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        SbExpr::Vector argVars;
        for (size_t idx = 0; idx < arity; ++idx) {
            argVars.push_back(SbVar{frameId, numLocalVars++});
        }

        // Take in all arguments and construct an array.
        auto arrayExpr =
            _b.makeLet(frameId, std::move(binds), _b.makeFunction("newArray", std::move(argVars)));

        pushExpr(_b.makeFillEmptyNull(_b.makeFunction(functionCall, std::move(arrayExpr))));
    }

    /**
     * Generic logic for building set expressions: setUnion, setIntersection, etc.
     */
    void generateSetExpression(const Expression* expr, SetOperation setOp) {
        using namespace std::literals;

        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        SbExpr::Vector args;
        for (size_t idx = 0; idx < arity; ++idx) {
            args.push_back(popExpr());
        }
        // Reverse the args array to preserve the original order of the arguments, since some set
        // operations, such as $setDifference, are not commutative.
        std::reverse(std::begin(args), std::end(args));

        SbExpr::Vector variables;
        SbExpr::Vector checkNulls;
        SbExpr::Vector checkNotArrays;

        auto collatorSlot = _context->state.getCollatorSlot();

        variables.reserve(arity + (collatorSlot.has_value() ? 1 : 0));
        checkNulls.reserve(arity);
        checkNotArrays.reserve(arity);

        auto operatorNameSetFunctionNamePair =
            getSetOperatorAndFunctionNames(setOp, collatorSlot.has_value());
        auto operatorName = operatorNameSetFunctionNamePair.first;
        auto setFunctionName = operatorNameSetFunctionNamePair.second;
        if (collatorSlot) {
            variables.push_back(SbVar{*collatorSlot});
        }

        auto frameId = _context->state.frameId();
        sbe::value::SlotId numLocalVars = 0;

        for (size_t idx = 0; idx < arity; ++idx) {
            SbVar argVar{frameId, numLocalVars++};

            variables.push_back(argVar);
            checkNulls.push_back(_b.generateNullMissingOrUndefined(argVar));
            checkNotArrays.push_back(_b.generateNonArrayCheck(argVar));
        }

        auto checkNullAnyArgument =
            _b.makeBooleanOpTree(abt::Operations::Or, std::move(checkNulls));
        auto checkNotArrayAnyArgument =
            _b.makeBooleanOpTree(abt::Operations::Or, std::move(checkNotArrays));
        SbExpr setExpr = [&]() -> SbExpr {
            // To match classic engine semantics, $setEquals and $setIsSubset should throw an error
            // for any non-array arguments including null and missing values.
            if (setOp == SetOperation::Equals || setOp == SetOperation::IsSubset) {
                return _b.makeIf(
                    _b.makeFillEmptyTrue(std::move(checkNotArrayAnyArgument)),
                    _b.makeFail(ErrorCodes::Error{7158100},
                                str::stream()
                                    << "All operands of $" << operatorName << " must be arrays."),
                    _b.makeFunction(std::string{setFunctionName}, std::move(variables)));
            } else {
                return _b.buildMultiBranchConditionalFromCaseValuePairs(
                    SbExpr::makeExprPairVector(
                        SbExprPair{std::move(checkNullAnyArgument), _b.makeNullConstant()},
                        SbExprPair{std::move(checkNotArrayAnyArgument),
                                   _b.makeFail(ErrorCodes::Error{7158101},
                                               str::stream() << "All operands of $" << operatorName
                                                             << " must be arrays.")}),
                    _b.makeFunction(std::string{setFunctionName}, std::move(variables)));
            }
        }();

        setExpr = _b.makeLet(frameId, std::move(args), std::move(setExpr));
        pushExpr(std::move(setExpr));
    }

    std::pair<StringData, StringData> getSetOperatorAndFunctionNames(SetOperation setOp,
                                                                     bool hasCollator) const {
        switch (setOp) {
            case SetOperation::Difference:
                return std::make_pair("setDifference"_sd,
                                      hasCollator ? "collSetDifference"_sd : "setDifference"_sd);
            case SetOperation::Intersection:
                return std::make_pair("setIntersection"_sd,
                                      hasCollator ? "collSetIntersection"_sd
                                                  : "setIntersection"_sd);
            case SetOperation::Union:
                return std::make_pair("setUnion"_sd,
                                      hasCollator ? "collSetUnion"_sd : "setUnion"_sd);
            case SetOperation::Equals:
                return std::make_pair("setEquals"_sd,
                                      hasCollator ? "collSetEquals"_sd : "setEquals"_sd);
            case SetOperation::IsSubset:
                return std::make_pair("setIsSubset"_sd,
                                      hasCollator ? "collSetIsSubset"_sd : "setIsSubset"_sd);
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Shared expression building logic for regex expressions.
     */
    void generateRegexExpression(const ExpressionRegex* expr, StringData exprName) {
        size_t arity = expr->hasOptions() ? 3 : 2;
        _context->ensureArity(arity);

        SbExpr options = expr->hasOptions() ? popExpr() : SbExpr{};
        auto pattern = popExpr();
        auto input = popExpr();

        auto outerFrameId = _context->state.frameId();
        SbVar inputVar{outerFrameId, 0};
        SbVar patternVar{outerFrameId, 1};

        auto generateRegexNullResponse = [&]() {
            if (exprName == "regexMatch"_sd) {
                return _b.makeBoolConstant(false);
            } else if (exprName == "regexFindAll"_sd) {
                auto [emptyArrTag, emptyArrVal] = sbe::value::makeNewArray();
                return _b.makeConstant(emptyArrTag, emptyArrVal);
            } else {
                return _b.makeNullConstant();
            }
        };

        auto makeError = [&](int errorCode, StringData message) {
            return _b.makeFail(ErrorCodes::Error{errorCode},
                               str::stream() << "$" << std::string{exprName} << ": " << message);
        };

        auto makeRegexFunctionCall = [&](SbExpr compiledRegex) {
            auto resultFrameId = _context->state.frameId();
            SbVar resultVar{resultFrameId, 0};

            return _b.makeLet(
                resultFrameId,
                SbExpr::makeSeq(_b.makeFunction(exprName, std::move(compiledRegex), inputVar)),
                _b.makeIf(
                    _b.makeFunction("exists"_sd, resultVar),
                    resultVar,
                    makeError(5073403, "error occurred while executing the regular expression")));
        };

        auto regexFunctionResult = [&]() {
            if (auto patternAndOptions = expr->getConstantPatternAndOptions(); patternAndOptions) {
                auto [pattern, options] = *patternAndOptions;
                if (!pattern) {
                    // Pattern is null, just generate null result.
                    return generateRegexNullResponse();
                }

                // Create the compiled Regex from constant pattern and options.
                auto [regexTag, regexVal] = sbe::makeNewPcreRegex(*pattern, options);
                auto compiledRegex = _b.makeConstant(regexTag, regexVal);
                return makeRegexFunctionCall(std::move(compiledRegex));
            }

            // 'patternArgument' contains the following expression:
            //
            // if isString(pattern) {
            //     if hasNullBytes(pattern) {
            //         fail('pattern cannot have null bytes in it')
            //     } else {
            //         pattern
            //     }
            // } else if isBsonRegex(pattern) {
            //     getRegexPattern(pattern)
            // } else {
            //     fail('pattern must be either string or BSON RegEx')
            // }
            auto patternArgument = _b.makeIf(
                _b.makeFunction("isString"_sd, patternVar),
                _b.makeIf(_b.makeFunction("hasNullBytes"_sd, patternVar),
                          makeError(5126602, "regex pattern must not have embedded null bytes"),
                          patternVar),
                _b.makeIf(_b.makeFunction("typeMatch"_sd,
                                          patternVar,
                                          _b.makeInt32Constant(getBSONTypeMask(BSONType::regEx))),
                          _b.makeFunction("getRegexPattern"_sd, patternVar),
                          makeError(5126601,
                                    "regex pattern must have either string or BSON RegEx type")));

            if (options.isNull()) {
                // If no options are passed to the expression, try to extract them from the
                // pattern.
                auto optionsArgument = _b.makeIf(
                    _b.makeFunction("typeMatch"_sd,
                                    patternVar,
                                    _b.makeInt32Constant(getBSONTypeMask(BSONType::regEx))),
                    _b.makeFunction("getRegexFlags"_sd, patternVar),
                    _b.makeStrConstant(""_sd));
                auto compiledRegex = _b.makeFunction(
                    "regexCompile"_sd, std::move(patternArgument), std::move(optionsArgument));
                return _b.makeIf(_b.makeFunction("isNull"_sd, patternVar),
                                 generateRegexNullResponse(),
                                 makeRegexFunctionCall(std::move(compiledRegex)));
            }

            // If there are options passed to the expression, we construct local bind with
            // options argument because it needs to be validated even when pattern is null.
            auto optionsFrameId = _context->state.frameId();
            SbVar userOptionsVar{optionsFrameId, 0};
            SbVar optionsVar{optionsFrameId, 1};

            auto optionsArgument = [&]() {
                // The code below generates the following expression:
                //
                // let stringOptions =
                //     if isString(options) {
                //         if hasNullBytes(options) {
                //             fail('options cannot have null bytes in it')
                //         } else {
                //             options
                //         }
                //     } else if isNull(options) {
                //         ''
                //     } else {
                //         fail('options must be either string or null')
                //     }
                // in
                //     if isBsonRegex(pattern) {
                //         let bsonOptions = getRegexFlags(pattern)
                //         in
                //             if stringOptions == "" {
                //                 bsonOptions
                //             } else if bsonOptions == "" {
                //                 stringOptions
                //             } else {
                //                 fail('multiple options specified')
                //             }
                //     } else {
                //         stringOptions
                //     }
                auto stringOptions = _b.makeIf(
                    _b.makeFunction("isString"_sd, userOptionsVar),
                    _b.makeIf(_b.makeFunction("hasNullBytes"_sd, userOptionsVar),
                              makeError(5126604, "regex flags must not have embedded null bytes"),
                              userOptionsVar),
                    _b.makeIf(
                        _b.makeFunction("isNull"_sd, userOptionsVar),
                        _b.makeStrConstant(""_sd),
                        makeError(5126603, "regex flags must have either string or null type")));

                auto generateIsEmptyString = [&](const SbVar& var) {
                    return _b.makeBinaryOp(abt::Operations::Eq, var, _b.makeStrConstant(""_sd));
                };

                auto stringFrameId = _context->state.frameId();
                SbVar stringVar{stringFrameId, 0};

                auto bsonPatternFrameId = _context->state.frameId();
                SbVar bsonPatternVar{bsonPatternFrameId, 0};

                return _b.makeLet(
                    stringFrameId,
                    SbExpr::makeSeq(std::move(stringOptions)),
                    _b.makeIf(
                        _b.makeFunction("typeMatch"_sd,
                                        patternVar,
                                        _b.makeInt32Constant(getBSONTypeMask(BSONType::regEx))),
                        _b.makeLet(
                            bsonPatternFrameId,
                            SbExpr::makeSeq(_b.makeFunction("getRegexFlags", patternVar)),
                            _b.buildMultiBranchConditionalFromCaseValuePairs(
                                SbExpr::makeExprPairVector(
                                    SbExprPair{generateIsEmptyString(stringVar), bsonPatternVar},
                                    SbExprPair{generateIsEmptyString(bsonPatternVar), stringVar}),
                                makeError(5126605,
                                          "regex options cannot be specified in both BSON "
                                          "RegEx and 'options' field"))),
                        stringVar));
            }();

            return _b.makeLet(optionsFrameId,
                              SbExpr::makeSeq(std::move(options), std::move(optionsArgument)),
                              _b.makeIf(_b.makeFunction("isNull"_sd, patternVar),
                                        generateRegexNullResponse(),
                                        makeRegexFunctionCall(_b.makeFunction(
                                            "regexCompile"_sd, patternVar, optionsVar))));
        }();

        auto regexCall = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{_b.generateNullMissingOrUndefined(inputVar),
                           generateRegexNullResponse()},
                SbExprPair{_b.makeNot(_b.makeFunction("isString"_sd, inputVar)),
                           makeError(5073401, "input must be of type string")}),
            std::move(regexFunctionResult));

        pushExpr(_b.makeLet(outerFrameId,
                            SbExpr::makeSeq(std::move(input), std::move(pattern)),
                            std::move(regexCall)));
    }

    /**
     * Generic logic for building $dateAdd and $dateSubtract expressions.
     */
    void generateDateArithmeticsExpression(const ExpressionDateArithmetics* expr,
                                           const std::string& dateExprName) {
        const auto& children = expr->getChildren();
        auto arity = children.size();
        invariant(arity == 4);
        auto timezoneExpr = children[3] ? popExpr() : _b.makeStrConstant("UTC"_sd);
        auto amountExpr = popExpr();
        auto unitExpr = popExpr();
        auto startDateExpr = popExpr();

        auto frameId = _context->state.frameId();
        SbVar startDateVar{frameId, 0};
        SbVar unitVar{frameId, 1};
        SbVar origAmountVar{frameId, 2};
        SbVar tzVar{frameId, 3};
        SbVar amountVar{frameId, 4};

        auto convertedAmountInt64 = [&]() {
            if (dateExprName == "dateAdd") {
                return _b.makeFunction(
                    "convert",
                    origAmountVar,
                    _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64)));
            } else if (dateExprName == "dateSubtract") {
                return _b.makeFunction(
                    "convert",
                    _b.makeUnaryOp(abt::Operations::Neg, origAmountVar),
                    _b.makeInt32Constant(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64)));
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto timeZoneDBVar = SbVar{timeZoneDBSlot};

        SbExpr::Vector checkNullArg;
        checkNullArg.push_back(_b.generateNullMissingOrUndefined(startDateVar));
        checkNullArg.push_back(_b.generateNullMissingOrUndefined(unitVar));
        checkNullArg.push_back(_b.generateNullMissingOrUndefined(origAmountVar));
        checkNullArg.push_back(_b.generateNullMissingOrUndefined(tzVar));

        auto checkNullAnyArgument =
            _b.makeBooleanOpTree(abt::Operations::Or, std::move(checkNullArg));

        auto dateAddExpr = _b.buildMultiBranchConditionalFromCaseValuePairs(
            SbExpr::makeExprPairVector(
                SbExprPair{std::move(checkNullAnyArgument), _b.makeNullConstant()},
                SbExprPair{_b.generateNonStringCheck(tzVar),
                           _b.makeFail(ErrorCodes::Error{7157902},
                                       str::stream()
                                           << "$" << dateExprName
                                           << " expects timezone argument of type string")},
                SbExprPair{_b.makeNot(_b.makeFunction("isTimezone", timeZoneDBVar, tzVar)),
                           _b.makeFail(ErrorCodes::Error{7157903},
                                       str::stream()
                                           << "$" << dateExprName << " expects a valid timezone")},
                SbExprPair{_b.makeNot(_b.makeFunction(
                               "typeMatch", startDateVar, _b.makeInt32Constant(dateTypeMask()))),
                           _b.makeFail(ErrorCodes::Error{7157904},
                                       str::stream()
                                           << "$" << dateExprName
                                           << " must have startDate argument convertable to date")},
                SbExprPair{_b.generateNonStringCheck(unitVar),
                           _b.makeFail(ErrorCodes::Error{7157905},
                                       str::stream() << "$" << dateExprName
                                                     << " expects unit argument of type string")},
                SbExprPair{_b.makeNot(_b.makeFunction("isTimeUnit", unitVar)),
                           _b.makeFail(ErrorCodes::Error{7157906},
                                       str::stream()
                                           << "$" << dateExprName << " expects a valid time unit")},
                SbExprPair{_b.makeNot(_b.makeFunction("exists", amountVar)),
                           _b.makeFail(ErrorCodes::Error{7157907},
                                       str::stream() << "invalid $" << dateExprName
                                                     << " 'amount' argument value")}),
            _b.makeFunction("dateAdd", timeZoneDBVar, startDateVar, unitVar, amountVar, tzVar));

        pushExpr(_b.makeLet(frameId,
                            SbExpr::makeSeq(std::move(startDateExpr),
                                            std::move(unitExpr),
                                            std::move(amountExpr),
                                            std::move(timezoneExpr),
                                            std::move(convertedAmountInt64)),
                            std::move(dateAddExpr)));
    }

    void unsupportedExpression(const char* op) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(5182300, str::stream() << "Unsupported expression in SBE stage builder: " << op);
    }

private:
    void pushExpr(SbExpr expr) {
        _context->pushExpr(std::move(expr));
    }

    SbExpr popExpr() {
        return _context->popExpr();
    }

    ExpressionVisitorContext* _context;

    SbExprBuilder _b;
};
}  // namespace

SbExpr generateExpression(StageBuilderState& state,
                          const Expression* expr,
                          boost::optional<SbSlot> rootSlot,
                          const PlanStageSlots& slots) {
    ExpressionVisitorContext context(state, std::move(rootSlot), slots);

    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk<const Expression>(expr, &walker);

    return context.done();
}

SbExpr generateExpressionFieldPath(StageBuilderState& state,
                                   const FieldPath& fieldPath,
                                   boost::optional<Variables::Id> variableId,
                                   boost::optional<SbSlot> rootSlot,
                                   const PlanStageSlots& slots,
                                   std::map<Variables::Id, sbe::FrameId>* environment) {
    SbExprBuilder b(state);
    invariant(fieldPath.getPathLength() >= 1);

    boost::optional<SbSlot> topLevelFieldSlot;
    bool expectsDocumentInputOnly = false;
    auto fp = fieldPath.getPathLength() > 1 ? boost::make_optional(fieldPath.tail()) : boost::none;

    if (!variableId) {
        auto it = Variables::kBuiltinVarNameToId.find(fieldPath.front());

        if (it != Variables::kBuiltinVarNameToId.end()) {
            variableId.emplace(it->second);
        } else if (fieldPath.front() == "CURRENT"_sd) {
            variableId.emplace(Variables::kRootId);
        } else {
            tasserted(8859700,
                      str::stream() << "Expected variableId to be provided for user variable: '$$"
                                    << fieldPath.fullPath() << "'");
        }
    }

    auto varId = *variableId;

    SbExpr inputExpr;

    if (!Variables::isUserDefinedVariable(varId)) {
        if (varId == Variables::kRootId) {
            if (fp) {
                // Check if we already have a slot containing an expression corresponding
                // to 'expr'.
                auto fpe = std::make_pair(PlanStageSlots::kPathExpr, fp->fullPath());
                if (slots.has(fpe)) {
                    return SbExpr{slots.get(fpe)};
                }

                // Obtain a slot for the top-level field referred to by 'expr', if one
                // exists.
                auto topLevelField = std::make_pair(PlanStageSlots::kField, fp->front());
                topLevelFieldSlot = slots.getIfExists(topLevelField);
            }

            // Set inputExpr to refer to the root document.
            inputExpr = SbExpr{rootSlot};
            expectsDocumentInputOnly = true;
        } else if (varId == Variables::kRemoveId) {
            // For the field paths that begin with "$$REMOVE", we always produce Nothing,
            // so no traversal is necessary.
            return b.makeNothingConstant();
        } else {
            auto slot = state.getBuiltinVarSlot(varId);
            if (!slot.has_value()) {
                std::stringstream message;
                message << "Builtin variable '$$" << fieldPath.fullPath() << "' (id=" << varId
                        << ") is not available";
                if (varId == Variables::kUserRolesId && !enableAccessToUserRoles.load()) {
                    message << " as the server is not configured to accept it";
                }
                uasserted(5611301, message.str());
            }
            inputExpr = SbExpr{SbSlot{*slot}};
        }
    } else {
        if (environment) {
            auto it = environment->find(varId);
            if (it != environment->end()) {
                inputExpr = b.makeVariable(it->second, 0);
            }
        }

        if (!inputExpr) {
            inputExpr = SbSlot{state.getGlobalVariableSlot(varId)};
        }
    }

    if (fieldPath.getPathLength() == 1) {
        tassert(6929400, "Expected a valid input expression", !inputExpr.isNull());

        // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
        // traversal.
        return inputExpr;
    }

    tassert(6929401,
            "Expected a valid input expression or a valid field slot",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    // Dereference a dotted path, which may contain arrays requiring implicit traversal.
    return generateTraverse(
        std::move(inputExpr), expectsDocumentInputOnly, *fp, state, topLevelFieldSlot);
}

SbExpr generateExpressionCompare(StageBuilderState& state,
                                 ExpressionCompare::CmpOp op,
                                 SbExpr lhs,
                                 SbExpr rhs) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto lhsVar = SbLocalVar{frameId, 0};
    auto rhsVar = SbLocalVar{frameId, 1};

    auto binds = SbExpr::makeSeq(std::move(lhs), std::move(rhs));

    auto comparisonOperator = [op]() {
        switch (op) {
            case ExpressionCompare::CmpOp::EQ:
                return abt::Operations::Eq;
            case ExpressionCompare::CmpOp::NE:
                return abt::Operations::Neq;
            case ExpressionCompare::CmpOp::GT:
                return abt::Operations::Gt;
            case ExpressionCompare::CmpOp::GTE:
                return abt::Operations::Gte;
            case ExpressionCompare::CmpOp::LT:
                return abt::Operations::Lt;
            case ExpressionCompare::CmpOp::LTE:
                return abt::Operations::Lte;
            case ExpressionCompare::CmpOp::CMP:
                return abt::Operations::Cmp3w;
        }
        MONGO_UNREACHABLE;
    }();

    // We use the "cmp3w" primitive for every comparison, because it "type brackets" its
    // comparisons (for example, a number will always compare as less than a string). The
    // other comparison primitives are designed for comparing values of the same type.
    auto cmp3w = b.makeBinaryOp(abt::Operations::Cmp3w, lhsVar, rhsVar);

    auto cmp = (comparisonOperator == abt::Operations::Cmp3w)
        ? std::move(cmp3w)
        : b.makeBinaryOp(comparisonOperator, std::move(cmp3w), b.makeInt32Constant(0));

    // If either operand evaluates to "Nothing", then the entire operation expressed by
    // 'cmp' will also evaluate to "Nothing". MQL comparisons, however, treat "Nothing" as
    // if it is a value that is less than everything other than MinKey. (Notably, two
    // expressions that evaluate to "Nothing" are considered equal to each other.) We also
    // need to explicitly check for 'bsonUndefined' type because it is considered equal to
    // "Nothing" according to MQL semantics.
    auto generateExists = [&](SbLocalVar var) {
        auto undefinedTypeMask = static_cast<int32_t>(getBSONTypeMask(BSONType::undefined));
        return b.makeBinaryOp(
            abt::Operations::And,
            b.makeFunction("exists"_sd, var),
            b.makeFunction("typeMatch"_sd, var, b.makeInt32Constant(~undefinedTypeMask)));
    };

    auto nothingFallbackCmp =
        b.makeBinaryOp(comparisonOperator, generateExists(lhsVar), generateExists(rhsVar));

    auto cmpWithFallback = b.makeFillEmpty(std::move(cmp), std::move(nothingFallbackCmp));

    return b.makeLet(frameId, std::move(binds), std::move(cmpWithFallback));
}
}  // namespace mongo::stage_builder
