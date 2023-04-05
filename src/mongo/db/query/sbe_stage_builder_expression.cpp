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

#include <absl/container/flat_hash_set.h>

#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/util/make_data_structure.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_abt_helpers.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/util/str.h"


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
                bindings.push_back({variableId, frameIdGenerator->generate(), EvalExpr{}});
            }
        }

        struct Binding {
            Variables::Id variableId;
            sbe::FrameId frameId;
            EvalExpr expr;
        };

        std::vector<Binding> bindings;
        size_t currentBindingIndex;
    };

    ExpressionVisitorContext(StageBuilderState& state,
                             boost::optional<sbe::value::SlotId> rootSlot,
                             const PlanStageSlots* slots = nullptr)
        : state(state), rootSlot(std::move(rootSlot)), slots(slots) {}

    void ensureArity(size_t arity) {
        invariant(exprStack.size() >= arity);
    }

    void pushExpr(abt::HolderPtr expr) {
        exprStack.emplace_back(std::move(expr));
    }
    void pushExpr(sbe::value::SlotId slotId) {
        exprStack.emplace_back(slotId);
    }

    optimizer::ABT popABTExpr() {
        tassert(6987504, "tried to pop from empty EvalExpr stack", !exprStack.empty());

        auto expr = std::move(exprStack.back());
        exprStack.pop_back();
        return abt::unwrap(expr.extractABT(state.slotVarMap));
    }

    EvalExpr popEvalExpr() {
        tassert(7261700, "tried to pop from empty EvalExpr stack", !exprStack.empty());

        auto expr = std::move(exprStack.back());
        exprStack.pop_back();
        return expr;
    }

    EvalExpr done() {
        tassert(6987501, "expected exactly one EvalExpr on the stack", exprStack.size() == 1);
        return popEvalExpr();
    }

    optimizer::ProjectionName registerVariable(sbe::value::SlotId slotId) {
        auto varName = stage_builder::makeVariableName(slotId);
        state.slotVarMap.emplace(varName, slotId);
        return varName;
    }

    StageBuilderState& state;

    std::vector<EvalExpr> exprStack;

    boost::optional<sbe::value::SlotId> rootSlot;

    // The lexical environment for the expression being traversed. A variable reference takes the
    // form "$$variable_name" in MQL's concrete syntax and gets transformed into a numeric
    // identifier (Variables::Id) in the AST. During this translation, we directly translate any
    // such variable to an SBE frame id using this mapping.
    std::map<Variables::Id, sbe::FrameId> environment;
    std::stack<VarsFrame> varsFrameStack;

    const PlanStageSlots* slots = nullptr;
};

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
optimizer::ABT generateTraverseHelper(
    ExpressionVisitorContext* context,
    boost::optional<optimizer::ABT>&& inputExpr,
    const FieldPath& fp,
    size_t level,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    boost::optional<sbe::value::SlotId> topLevelFieldSlot = boost::none) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());
    tassert(6950802,
            "Expected an input expression or top level field",
            inputExpr.has_value() || topLevelFieldSlot.has_value());

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldName = makeABTConstant(fp.getFieldName(level));
    auto fieldExpr = topLevelFieldSlot
        ? makeVariable(context->registerVariable(*topLevelFieldSlot))
        : makeABTFunction("getField"_sd, std::move(*inputExpr), std::move(fieldName));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse stage.
        return fieldExpr;
    }

    // Generate nested traversal.
    auto lambdaFrameId = frameIdGenerator->generate();
    auto lambdaParamName = makeLocalVariableName(lambdaFrameId, 0);
    boost::optional<optimizer::ABT> lambdaParam = makeVariable(lambdaParamName);

    auto resultExpr =
        generateTraverseHelper(context, std::move(lambdaParam), fp, level + 1, frameIdGenerator);

    auto lambdaExpr =
        optimizer::make<optimizer::LambdaAbstraction>(lambdaParamName, std::move(resultExpr));

    // Generate the traverse stage for the current nested level.
    return makeABTFunction(
        "traverseP"_sd, std::move(fieldExpr), std::move(lambdaExpr), optimizer::Constant::int32(1));
}

optimizer::ABT generateTraverse(
    ExpressionVisitorContext* context,
    boost::optional<optimizer::ABT>&& inputExpr,
    bool expectsDocumentInputOnly,
    const FieldPath& fp,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    boost::optional<sbe::value::SlotId> topLevelFieldSlot = boost::none) {
    size_t level = 0;

    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputExpr' will be a document and _not_ an array (such as
        // when accessing a field on the root document), we can generate a simpler expression.
        return generateTraverseHelper(
            context, std::move(inputExpr), fp, level, frameIdGenerator, topLevelFieldSlot);
    } else {
        tassert(6950803, "Expected an input expression", inputExpr.has_value());
        // The general case: the value in the 'inputExpr' may be an array that will require
        // traversal.
        auto lambdaFrameId = frameIdGenerator->generate();
        auto lambdaParamName = makeLocalVariableName(lambdaFrameId, 0);
        boost::optional<optimizer::ABT> lambdaParam = makeVariable(lambdaParamName);

        auto resultExpr =
            generateTraverseHelper(context, std::move(lambdaParam), fp, level, frameIdGenerator);

        auto lambdaExpr =
            optimizer::make<optimizer::LambdaAbstraction>(lambdaParamName, std::move(resultExpr));

        return makeABTFunction("traverseP"_sd,
                               std::move(*inputExpr),
                               std::move(lambdaExpr),
                               optimizer::Constant::int32(1));
    }
}

/**
 * Generates an EExpression that converts the input to upper or lower case.
 */
void generateStringCaseConversionExpression(ExpressionVisitorContext* _context,
                                            const std::string& caseConversionFunction) {
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

    auto str = _context->popABTExpr();
    auto varStr = makeLocalVariableName(_context->state.frameId(), 0);

    auto totalCaseConversionExpr = optimizer::make<optimizer::If>(
        generateABTNullOrMissing(varStr),
        makeABTConstant(""_sd),
        optimizer::make<optimizer::If>(
            makeABTFunction(
                "typeMatch"_sd, makeVariable(varStr), optimizer::Constant::int32(typeMask)),
            makeABTFunction(caseConversionFunction,
                            makeABTFunction("coerceToString"_sd, makeVariable(varStr))),
            makeABTFail(ErrorCodes::Error{7158200},
                        str::stream()
                            << "$" << caseConversionFunction << " input type is not supported")));

    _context->pushExpr(abt::wrap(optimizer::make<optimizer::Let>(
        varStr, std::move(str), std::move(totalCaseConversionExpr))));
}

/**
 * Generate an EExpression representing a Regex function result upon null argument(s) depending on
 * the type of the function: $regexMatch - false, $regexFind - null, $RegexFindAll - [].
 */
std::unique_ptr<sbe::EExpression> generateRegexNullResponse(StringData exprName) {
    if (exprName.toString().compare(std::string("regexMatch")) == 0) {
        return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                          sbe::value::bitcastFrom<bool>(false));
    } else if (exprName.toString().compare("regexFindAll") == 0) {
        auto [arrTag, arrVal] = sbe::value::makeNewArray();
        return sbe::makeE<sbe::EConstant>(arrTag, arrVal);
    }
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0);
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
    void visit(const ExpressionCoerceToBool* expr) final {}
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
    void visit(const ExpressionToHashedIndexKey* expr) final {}
    void visit(const ExpressionDateAdd* expr) final {}
    void visit(const ExpressionDateSubtract* expr) final {}
    void visit(const ExpressionGetField* expr) final {}
    void visit(const ExpressionSetField* expr) final {}
    void visit(const ExpressionTsSecond* expr) final {}
    void visit(const ExpressionTsIncrement* expr) final {}
    void visit(const ExpressionInternalOwningShard* expr) final {}
    void visit(const ExpressionInternalIndexKey* expr) final {}

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
    void visit(const ExpressionCoerceToBool* expr) final {}
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
        currentBinding.expr = _context->popEvalExpr();

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
    void visit(const ExpressionToHashedIndexKey* expr) final {}
    void visit(const ExpressionDateAdd* expr) final {}
    void visit(const ExpressionDateSubtract* expr) final {}
    void visit(const ExpressionGetField* expr) final {}
    void visit(const ExpressionSetField* expr) final {}
    void visit(const ExpressionTsSecond* expr) final {}
    void visit(const ExpressionTsIncrement* expr) final {}
    void visit(const ExpressionInternalOwningShard* expr) final {}
    void visit(const ExpressionInternalIndexKey* expr) final {}

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
    ExpressionPostVisitor(ExpressionVisitorContext* context) : _context{context} {}

    enum class SetOperation {
        Difference,
        Intersection,
        Union,
        Equals,
    };

    void visit(const ExpressionConstant* expr) final {
        auto [tag, val] = sbe::value::makeValue(expr->getValue());
        pushABT(makeABTConstant(tag, val));
    }

    void visit(const ExpressionAbs* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto absExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157700}, "$abs only supports numeric types")},
            ABTCaseValuePair{
                generateABTLongLongMinCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157701}, "can't take $abs of long long min")},
            makeABTFunction("abs", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(absExpr)));
    }

    void visit(const ExpressionAdd* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Build a linear tree for a small number of children so that we can pre-validate all
        // arguments.
        if (arity < kArgumentCountForBinaryTree) {
            visitFast(expr);
            return;
        }

        auto checkLeaf = [&](optimizer::ABT arg) {
            auto name = makeLocalVariableName(_context->state.frameId(), 0);
            auto var = makeVariable(name);
            auto checkedLeaf = buildABTMultiBranchConditional(
                ABTCaseValuePair{
                    optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                         makeABTFunction("isNumber", var),
                                                         makeABTFunction("isDate", var)),
                    var},
                makeABTFail(ErrorCodes::Error{7315401},
                            "only numbers and dates are allowed in an $add expression"));
            return optimizer::make<optimizer::Let>(
                std::move(name), std::move(arg), std::move(checkedLeaf));
        };

        auto combineTwoTree = [&](optimizer::ABT left, optimizer::ABT right) {
            auto nameLeft = makeLocalVariableName(_context->state.frameId(), 0);
            auto nameRight = makeLocalVariableName(_context->state.frameId(), 0);
            auto varLeft = makeVariable(nameLeft);
            auto varRight = makeVariable(nameRight);

            auto addExpr = buildABTMultiBranchConditional(
                ABTCaseValuePair{
                    optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                         generateABTNullOrMissing(nameLeft),
                                                         generateABTNullOrMissing(nameRight)),
                    optimizer::Constant::null()},
                ABTCaseValuePair{
                    optimizer::make<optimizer::BinaryOp>(optimizer::Operations::And,
                                                         makeABTFunction("isDate", varLeft),
                                                         makeABTFunction("isDate", varRight)),
                    makeABTFail(ErrorCodes::Error{7315402},
                                "only one date allowed in an $add expression")},
                optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Add, varLeft, varRight));
            return optimizer::make<optimizer::Let>(
                std::move(nameLeft),
                std::move(left),
                optimizer::make<optimizer::Let>(
                    std::move(nameRight), std::move(right), std::move(addExpr)));
        };

        optimizer::ABTVector leaves;
        leaves.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            leaves.emplace_back(checkLeaf(_context->popABTExpr()));
        }
        std::reverse(std::begin(leaves), std::end(leaves));

        pushABT(makeBalancedTree(combineTwoTree, std::move(leaves)));
    }

    void visitFast(const ExpressionAdd* expr) {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            // Return a zero constant if the expression has no operand children.
            pushABT(optimizer::Constant::int32(0));
        } else {
            optimizer::ABTVector binds;
            optimizer::ProjectionNameVector names;
            optimizer::ABTVector checkArgIsNull;
            optimizer::ABTVector checkArgHasValidType;
            binds.reserve(arity);
            names.reserve(arity);
            checkArgIsNull.reserve(arity);
            checkArgHasValidType.reserve(arity);

            for (size_t idx = 0; idx < arity; ++idx) {
                binds.push_back(_context->popABTExpr());
                auto name = makeLocalVariableName(_context->state.frameId(), 0);

                // Count the number of dates among children of this $add while verifying the types
                // so that we can later check that we have at most one date.
                checkArgHasValidType.emplace_back(buildABTMultiBranchConditional(
                    ABTCaseValuePair{makeABTFunction("isNumber", makeVariable(name)),
                                     optimizer::Constant::int32(0)},
                    ABTCaseValuePair{makeABTFunction("isDate", makeVariable(name)),
                                     optimizer::Constant::int32(1)},
                    makeABTFail(ErrorCodes::Error{7157723},
                                "only numbers and dates are allowed in an $add expression")));

                checkArgIsNull.push_back(generateABTNullOrMissing(name));
                names.push_back(std::move(name));
            }

            // At this point 'binds' vector contains arguments of $add expression in the reversed
            // order. We need to reverse it back to perform summation in the right order below.
            // Summation in different order can lead to different result because of accumulated
            // precision errors from floating point types.
            std::reverse(std::begin(binds), std::end(binds));

            auto checkNullAllArguments =
                makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(checkArgIsNull));

            auto checkValidTypeAndCountDates = makeBalancedBooleanOpTree(
                optimizer::Operations::Add, std::move(checkArgHasValidType));

            auto addOp = makeVariable(names[0]);
            for (size_t idx = 1; idx < arity; ++idx) {
                addOp = optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Add, std::move(addOp), makeVariable(names[idx]));
            }

            auto addExpr = buildABTMultiBranchConditional(
                ABTCaseValuePair{std::move(checkNullAllArguments), optimizer::Constant::null()},
                ABTCaseValuePair{
                    optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Gt,
                                                         std::move(checkValidTypeAndCountDates),
                                                         optimizer::Constant::int32(1)),
                    makeABTFail(ErrorCodes::Error{7157722},
                                "only one date allowed in an $add expression")},
                std::move(addOp));

            for (size_t idx = 0; idx < arity; ++idx) {
                addExpr = optimizer::make<optimizer::Let>(
                    std::move(names[idx]), std::move(binds[idx]), std::move(addExpr));
            }

            pushABT(std::move(addExpr));
        }
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, optimizer::Operations::And);
    }
    void visit(const ExpressionAnyElementTrue* expr) final {
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        auto lambdaArgName = makeLocalVariableName(_context->state.frameId(), 0);
        auto lambdaBody =
            makeFillEmptyFalse(makeABTFunction("coerceToBool", makeVariable(lambdaArgName)));
        auto lambdaExpr = optimizer::make<optimizer::LambdaAbstraction>(std::move(lambdaArgName),
                                                                        std::move(lambdaBody));

        auto resultExpr = optimizer::make<optimizer::If>(
            makeFillEmptyFalse(makeABTFunction("isArray", makeVariable(argName))),
            makeABTFunction("traverseF",
                            makeVariable(argName),
                            std::move(lambdaExpr),
                            optimizer::Constant::boolean(false)),
            makeABTFail(ErrorCodes::Error{7158300}, "$anyElementTrue's argument must be an array"));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(argName), std::move(arg), std::move(resultExpr)));
    }

    void visit(const ExpressionArray* expr) final {
        unsupportedExpression(expr->getOpName());
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
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create an expression to invoke the built-in function.
        auto funcCall = makeABTFunction("objectToArray", makeVariable(argName));
        auto funcName = makeLocalVariableName(_context->state.frameId(), 0);
        auto funcVar = makeVariable(funcName);

        // Create validation checks when builtin returns nothing
        auto validationExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(argName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonObjectCheck(argName),
                makeABTFail(ErrorCodes::Error{5153215}, "$objectToArray requires an object input")},
            optimizer::Constant::nothing());

        auto existCheck = makeABTFunction("exists", funcVar);

        pushABT(optimizer::make<optimizer::Let>(
            std::move(argName),
            std::move(arg),
            optimizer::make<optimizer::Let>(
                std::move(funcName),
                std::move(funcCall),
                optimizer::make<optimizer::If>(
                    existCheck, std::move(funcVar), std::move(validationExpr)))));
    }
    void visit(const ExpressionArrayToObject* expr) final {
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create an expression to invoke the built-in function.
        auto funcCall = makeABTFunction("arrayToObject", makeVariable(argName));
        auto funcName = makeLocalVariableName(_context->state.frameId(), 0);
        auto funcVar = makeVariable(funcName);

        // Create validation checks when builtin returns nothing
        auto validationExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(argName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonArrayCheck(argName),
                makeABTFail(ErrorCodes::Error{5153200}, "$arrayToObject requires an array input")},
            optimizer::Constant::nothing());

        auto existCheck = makeABTFunction("exists", funcVar);

        pushABT(optimizer::make<optimizer::Let>(
            std::move(argName),
            std::move(arg),
            optimizer::make<optimizer::Let>(
                std::move(funcName),
                std::move(funcCall),
                optimizer::make<optimizer::If>(
                    existCheck, std::move(funcVar), std::move(validationExpr)))));
    }
    void visit(const ExpressionBsonSize* expr) final {
        // Build an expression which evaluates the size of a BSON document and validates the input
        // argument.
        // 1. If the argument is null or empty, return null.
        // 2. Else, if the argument is a BSON document, return its size.
        // 3. Else, raise an error.
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        auto bsonSizeExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(argName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonObjectCheck(argName),
                makeABTFail(ErrorCodes::Error{7158301}, "$bsonSize requires a document input")},
            makeABTFunction("bsonSize", makeVariable(argName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(argName), std::move(arg), std::move(bsonSizeExpr)));
    }

    void visit(const ExpressionCeil* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto ceilExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157702}, "$ceil only supports numeric types")},
            makeABTFunction("ceil", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(ceilExpr)));
    }
    void visit(const ExpressionCoerceToBool* expr) final {
        // Since $coerceToBool is internal-only and there are not yet any input expressions that
        // generate an ExpressionCoerceToBool expression, we will leave it as unreachable for now.
        MONGO_UNREACHABLE;
    }
    void visit(const ExpressionCompare* expr) final {
        _context->ensureArity(2);
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();
        auto lhsRef = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsRef = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto comparisonOperator = [expr]() {
            switch (expr->getOp()) {
                case ExpressionCompare::CmpOp::EQ:
                    return optimizer::Operations::Eq;
                case ExpressionCompare::CmpOp::NE:
                    return optimizer::Operations::Neq;
                case ExpressionCompare::CmpOp::GT:
                    return optimizer::Operations::Gt;
                case ExpressionCompare::CmpOp::GTE:
                    return optimizer::Operations::Gte;
                case ExpressionCompare::CmpOp::LT:
                    return optimizer::Operations::Lt;
                case ExpressionCompare::CmpOp::LTE:
                    return optimizer::Operations::Lte;
                case ExpressionCompare::CmpOp::CMP:
                    return optimizer::Operations::Cmp3w;
            }
            MONGO_UNREACHABLE;
        }();

        // We use the "cmp3w" primitive for every comparison, because it "type brackets" its
        // comparisons (for example, a number will always compare as less than a string). The
        // other comparison primitives are designed for comparing values of the same type.
        auto cmp3w = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Cmp3w, makeVariable(lhsRef), makeVariable(rhsRef));
        auto cmp = (comparisonOperator == optimizer::Operations::Cmp3w)
            ? std::move(cmp3w)
            : optimizer::make<optimizer::BinaryOp>(
                  comparisonOperator, std::move(cmp3w), optimizer::Constant::int32(0));

        // If either operand evaluates to "Nothing", then the entire operation expressed by
        // 'cmp' will also evaluate to "Nothing". MQL comparisons, however, treat "Nothing" as
        // if it is a value that is less than everything other than MinKey. (Notably, two
        // expressions that evaluate to "Nothing" are considered equal to each other.) We also
        // need to explicitly check for 'bsonUndefined' type because it is considered equal to
        // "Nothing" according to MQL semantics.
        auto generateExists = [&](const optimizer::ProjectionName& var) {
            auto undefinedTypeMask = static_cast<int32_t>(getBSONTypeMask(BSONType::Undefined));
            return optimizer::make<optimizer::BinaryOp>(
                optimizer::Operations::And,
                makeABTFunction("exists"_sd, makeVariable(var)),
                makeABTFunction("typeMatch"_sd,
                                makeVariable(var),
                                optimizer::Constant::int32(~undefinedTypeMask)));
        };

        auto nothingFallbackCmp = optimizer::make<optimizer::BinaryOp>(
            comparisonOperator, generateExists(lhsRef), generateExists(rhsRef));

        auto cmpWithFallback = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::FillEmpty, std::move(cmp), std::move(nothingFallbackCmp));

        pushABT(optimizer::make<optimizer::Let>(
            lhsRef, lhs, optimizer::make<optimizer::Let>(rhsRef, rhs, cmpWithFallback)));
    }

    void visit(const ExpressionConcat* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Concatenation of no strings is an empty string.
        if (arity == 0) {
            pushABT(makeABTConstant(""_sd));
            return;
        }

        std::vector<std::pair<optimizer::ProjectionName, optimizer::ABT>> binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            // ABT can bind a single variable at a time, so create a new frame for each
            // argument.
            binds.emplace_back(makeLocalVariableName(_context->state.frameId(), 0),
                               _context->popABTExpr());
        }
        std::reverse(std::begin(binds), std::end(binds));

        optimizer::ABTVector checkNullArg;
        optimizer::ABTVector checkStringArg;
        optimizer::ABTVector argVars;
        for (auto& bind : binds) {
            checkNullArg.push_back(generateABTNullOrMissing(bind.first));
            checkStringArg.push_back(makeABTFunction("isString"_sd, makeVariable(bind.first)));
            argVars.push_back(makeVariable(bind.first));
        }

        auto checkNullAnyArgument =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(checkNullArg));

        auto checkStringAllArguments =
            makeBalancedBooleanOpTree(optimizer::Operations::And, std::move(checkStringArg));

        auto concatExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkNullAnyArgument), optimizer::Constant::null()},
            ABTCaseValuePair{
                std::move(checkStringAllArguments),
                optimizer::make<optimizer::FunctionCall>("concat", std::move(argVars))},
            makeABTFail(ErrorCodes::Error{7158201}, "$concat supports only strings"));

        for (auto it = binds.begin(); it != binds.end(); it++) {
            concatExpr = optimizer::make<optimizer::Let>(
                it->first, std::move(it->second), std::move(concatExpr));
        }

        pushABT(std::move(concatExpr));
        return;
    }

    void visit(const ExpressionConcatArrays* expr) final {
        auto numChildren = expr->getChildren().size();
        _context->ensureArity(numChildren);

        // If there are no children, return an empty array.
        if (numChildren == 0) {
            pushABT(optimizer::Constant::emptyArray());
            return;
        }

        std::vector<optimizer::ABT> args;
        args.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            args.emplace_back(_context->popABTExpr());
        }
        std::reverse(args.begin(), args.end());

        std::vector<optimizer::ABT> argIsNullOrMissing;
        optimizer::ProjectionNameVector argNames;
        optimizer::ABTVector argVars;
        argIsNullOrMissing.reserve(numChildren);
        argNames.reserve(numChildren);
        argVars.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            argNames.emplace_back(makeLocalVariableName(_context->state.frameId(), 0));
            argVars.emplace_back(makeVariable(argNames.back()));
            argIsNullOrMissing.emplace_back(generateABTNullOrMissing(argNames.back()));
        }

        auto anyArgumentNullOrMissing =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(argIsNullOrMissing));

        auto nullOrFailExpr = optimizer::make<optimizer::If>(
            std::move(anyArgumentNullOrMissing),
            optimizer::Constant::null(),
            makeABTFail(ErrorCodes::Error{7158000}, "$concatArrays only supports arrays"));

        optimizer::ProjectionName resultName = makeLocalVariableName(_context->state.frameId(), 0);
        auto resultExpr = optimizer::make<optimizer::Let>(
            resultName,
            optimizer::make<optimizer::FunctionCall>("concatArrays", std::move(argVars)),
            optimizer::make<optimizer::If>(makeABTFunction("exists", makeVariable(resultName)),
                                           makeVariable(resultName),
                                           std::move(nullOrFailExpr)));

        for (size_t i = 0; i < numChildren; ++i) {
            resultExpr = optimizer::make<optimizer::Let>(
                std::move(argNames[i]), std::move(args[i]), std::move(resultExpr));
        }

        pushABT(std::move(resultExpr));
    }

    void visit(const ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionDateDiff* expr) final {
        using namespace std::literals;

        auto children = expr->getChildren();
        invariant(children.size() == 5);

        auto startDateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto endDateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        auto startOfWeekName = makeLocalVariableName(_context->state.frameId(), 0);

        // An auxiliary boolean variable to hold a value of a common subexpression 'unit'=="week"
        // (string).
        auto unitIsWeekName = makeLocalVariableName(_context->state.frameId(), 0);

        auto startDateVar = makeVariable(startDateName);
        auto endDateVar = makeVariable(endDateName);
        auto unitVar = makeVariable(unitName);
        auto timezoneVar = makeVariable(timezoneName);
        auto startOfWeekVar = makeVariable(startOfWeekName);
        auto unitIsWeekVar = makeVariable(unitIsWeekName);

        // Get child expressions.
        boost::optional<optimizer::ABT> startOfWeekExpression;
        if (expr->isStartOfWeekSpecified()) {
            startOfWeekExpression = _context->popABTExpr();
        }
        auto timezoneExpression = expr->isTimezoneSpecified() ? _context->popABTExpr()
                                                              : optimizer::Constant::str("UTC"_sd);
        auto unitExpression = _context->popABTExpr();
        auto endDateExpression = _context->popABTExpr();
        auto startDateExpression = _context->popABTExpr();

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);
        auto timeZoneDBVar = makeVariable(timeZoneDBName);

        //  Set parameters for an invocation of built-in "dateDiff" function.
        optimizer::ABTVector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(startDateVar);
        arguments.push_back(endDateVar);
        arguments.push_back(unitVar);
        arguments.push_back(timezoneVar);
        if (expr->isStartOfWeekSpecified()) {
            // Parameter "startOfWeek" - if the time unit is the week, then pass value of parameter
            // "startOfWeek" of "$dateDiff" expression, otherwise pass a valid default value, since
            // "dateDiff" built-in function does not accept non-string type values for this
            // parameter.
            arguments.push_back(optimizer::make<optimizer::If>(
                unitIsWeekVar, startOfWeekVar, optimizer::Constant::str("sun"_sd)));
        }

        // Set bindings for the frame.
        optimizer::ABTVector bindings;
        optimizer::ProjectionNameVector bindingNames;
        bindingNames.push_back(startDateName);
        bindings.push_back(std::move(startDateExpression));
        bindingNames.push_back(endDateName);
        bindings.push_back(std::move(endDateExpression));
        bindingNames.push_back(unitName);
        bindings.push_back(std::move(unitExpression));
        bindingNames.push_back(timezoneName);
        bindings.push_back(std::move(timezoneExpression));
        if (expr->isStartOfWeekSpecified()) {
            bindingNames.push_back(startOfWeekName);
            bindings.push_back(*startOfWeekExpression);
            bindingNames.push_back(unitIsWeekName);
            bindings.push_back(generateABTIsEqualToStringCheck(unitVar, "week"_sd));
        }

        // Create an expression to invoke built-in "dateDiff" function.
        auto dateDiffFunctionCall =
            optimizer::make<optimizer::FunctionCall>("dateDiff", std::move(arguments));

        // Create expressions to check that each argument to "dateDiff" function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(startDateVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(endDateVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(unitVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(timezoneVar));
        if (expr->isStartOfWeekSpecified()) {
            inputValidationCases.emplace_back(
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::And,
                                                     unitIsWeekVar,
                                                     generateABTNullOrMissing(startOfWeekName)),
                optimizer::Constant::null());
        }

        // "timezone" parameter validation.
        inputValidationCases.emplace_back(
            generateABTNonStringCheck(timezoneName),
            makeABTFail(ErrorCodes::Error{7157919},
                        "$dateDiff parameter 'timezone' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneVar)),
            makeABTFail(ErrorCodes::Error{7157920},
                        "$dateDiff parameter 'timezone' must be a valid timezone"));

        // "startDate" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            startDateVar, ErrorCodes::Error{7157921}, "$dateDiff"_sd, "startDate"_sd));

        // "endDate" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            endDateVar, ErrorCodes::Error{7157922}, "$dateDiff"_sd, "endDate"_sd));

        // "unit" parameter validation.
        inputValidationCases.emplace_back(
            generateABTNonStringCheck(unitName),
            makeABTFail(ErrorCodes::Error{7157923}, "$dateDiff parameter 'unit' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeABTFunction("isTimeUnit", unitVar)),
            makeABTFail(ErrorCodes::Error{7157924},
                        "$dateDiff parameter 'unit' must be a valid time unit"));

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
            inputValidationCases.emplace_back(
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::And,
                                                     unitIsWeekVar,
                                                     generateABTNonStringCheck(startOfWeekName)),
                makeABTFail(ErrorCodes::Error{7157925},
                            "$dateDiff parameter 'startOfWeek' must be a string"));
            inputValidationCases.emplace_back(
                optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::And,
                    unitIsWeekVar,
                    makeNot(makeABTFunction("isDayOfWeek", startOfWeekVar))),
                makeABTFail(ErrorCodes::Error{7157926},
                            "$dateDiff parameter 'startOfWeek' must be a valid day of the week"));
        }

        auto dateDiffExpression = buildABTMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateDiffFunctionCall));

        for (int i = bindings.size() - 1; i >= 0; --i) {
            dateDiffExpression = optimizer::make<optimizer::Let>(
                std::move(bindingNames[i]), std::move(bindings[i]), std::move(dateDiffExpression));
        }

        pushABT(std::move(dateDiffExpression));
    }
    void visit(const ExpressionDateFromString* expr) final {
        auto children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(
            1 + (expr->isFormatSpecified() ? 1 : 0) + (expr->isTimezoneSpecified() ? 1 : 0) +
            (expr->isOnErrorSpecified() ? 1 : 0) + (expr->isOnNullSpecified() ? 1 : 0));

        // Get child expressions.
        auto onErrorExpression =
            expr->isOnErrorSpecified() ? _context->popABTExpr() : optimizer::Constant::null();

        auto onNullExpression =
            expr->isOnNullSpecified() ? _context->popABTExpr() : optimizer::Constant::null();

        auto formatExpression =
            expr->isFormatSpecified() ? _context->popABTExpr() : optimizer::Constant::null();
        auto formatName = makeLocalVariableName(_context->state.frameId(), 0);

        auto timezoneExpression = expr->isTimezoneSpecified() ? _context->popABTExpr()
                                                              : optimizer::Constant::str("UTC"_sd);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);

        auto dateStringExpression = _context->popABTExpr();
        auto dateStringName = makeLocalVariableName(_context->state.frameId(), 0);

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);

        // Set parameters for an invocation of built-in "dateFromString" function.
        optimizer::ABTVector arguments;
        arguments.push_back(makeVariable(timeZoneDBName));
        // Set bindings for the frame.
        optimizer::ABTVector bindings;
        optimizer::ProjectionNameVector bindingNames;
        bindingNames.push_back(dateStringName);
        bindings.push_back(dateStringExpression);
        arguments.push_back(makeVariable(dateStringName));
        if (timezoneExpression.is<optimizer::Constant>()) {
            arguments.push_back(timezoneExpression);
        } else {
            bindingNames.push_back(timezoneName);
            bindings.push_back(timezoneExpression);
            arguments.push_back(makeVariable(timezoneName));
        }
        if (expr->isFormatSpecified()) {
            if (formatExpression.is<optimizer::Constant>()) {
                arguments.push_back(formatExpression);
            } else {
                bindingNames.push_back(formatName);
                bindings.push_back(formatExpression);
                arguments.push_back(makeVariable(formatName));
            }
        }

        // Create an expression to invoke built-in "dateFromString" function.
        std::string functionName =
            expr->isOnErrorSpecified() ? "dateFromStringNoThrow" : "dateFromString";
        auto dateFromStringFunctionCall =
            optimizer::make<optimizer::FunctionCall>(functionName, std::move(arguments));

        // Create expressions to check that each argument to "dateFromString" function exists, is
        // not null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return onNull if dateString is null or missing.
        inputValidationCases.push_back(
            {generateABTNullOrMissing(makeVariable(dateStringName)), onNullExpression});

        // Create an expression to return Nothing if specified, or raise a conversion failure.
        // As long as onError is specified, a Nothing return will always be filled with onError.
        auto nonStringReturn = expr->isOnErrorSpecified()
            ? optimizer::Constant::nothing()
            : makeABTFail(ErrorCodes::ConversionFailure,
                          "$dateFromString requires that 'dateString' be a string");

        inputValidationCases.push_back(
            {generateABTNonStringCheck(makeVariable(dateStringName)), nonStringReturn});

        if (expr->isTimezoneSpecified()) {
            if (timezoneExpression.is<optimizer::Constant>()) {
                // Return null if timezone is specified as either null or missing.
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullOrMissing(timezoneExpression));
            } else {
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullOrMissing(makeVariable(timezoneName)));
            }
        }

        if (expr->isFormatSpecified()) {
            // validate "format" parameter only if it has been specified.
            if (auto* formatExpressionConst = formatExpression.cast<optimizer::Constant>();
                formatExpressionConst) {
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullOrMissing(formatExpression));
                auto [formatTag, formatVal] = formatExpressionConst->get();
                if (!sbe::value::isNullish(formatTag)) {
                    // We don't want to error on null.
                    uassert(4997802,
                            "$dateFromString requires that 'format' be a string",
                            sbe::value::isString(formatTag));
                    TimeZone::validateFromStringFormat(getStringView(formatTag, formatVal));
                }
            } else {
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullOrMissing(makeVariable(formatName)));
                inputValidationCases.emplace_back(
                    generateABTNonStringCheck(makeVariable(formatName)),
                    makeABTFail(ErrorCodes::Error{4997803},
                                "$dateFromString requires that 'format' be a string"));
                inputValidationCases.emplace_back(
                    makeNot(makeABTFunction("validateFromStringFormat", makeVariable(formatName))),
                    // This should be unreachable. The validation function above will uassert on an
                    // invalid format string and then return true. It returns false on non-string
                    // input, but we already check for non-string format above.
                    optimizer::Constant::null());
            }
        }

        // "timezone" parameter validation.
        if (auto* timezoneExpressionConst = timezoneExpression.cast<optimizer::Constant>();
            timezoneExpressionConst) {
            auto [timezoneTag, timezoneVal] = timezoneExpressionConst->get();
            if (!sbe::value::isNullish(timezoneTag)) {
                // We don't want to error on null.
                uassert(4997805,
                        "$dateFromString parameter 'timezone' must be a string",
                        sbe::value::isString(timezoneTag));
                auto [timezoneDBTag, timezoneDBVal] =
                    _context->state.data->env->getAccessor(timeZoneDBSlot)->getViewOfValue();
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
                generateABTNonStringCheck(makeVariable(timezoneName)),
                makeABTFail(ErrorCodes::Error{4997807},
                            "$dateFromString parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction(
                    "isTimezone", makeVariable(timeZoneDBName), makeVariable(timezoneName))),
                makeABTFail(ErrorCodes::Error{4997808},
                            "$dateFromString parameter 'timezone' must be a valid timezone"));
        }

        auto dateFromStringExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateFromStringFunctionCall));

        // If onError is specified, a Nothing return means that either dateString is not a string,
        // or the builtin dateFromStringNoThrow caught an error. We return onError in either case.
        if (expr->isOnErrorSpecified()) {
            dateFromStringExpr =
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::FillEmpty,
                                                     std::move(dateFromStringExpr),
                                                     std::move(onErrorExpression));
        }

        for (int i = bindings.size() - 1; i >= 0; --i) {
            dateFromStringExpr = optimizer::make<optimizer::Let>(
                std::move(bindingNames[i]), std::move(bindings[i]), std::move(dateFromStringExpr));
        }

        pushABT(std::move(dateFromStringExpr));
    }
    void visit(const ExpressionDateFromParts* expr) final {
        // This expression can carry null children depending on the set of fields provided,
        // to compute a date from parts so we only need to pop if a child exists.
        auto children = expr->getChildren();
        invariant(children.size() == 11);

        boost::optional<optimizer::ABT> eTimezone;
        if (children[10]) {
            eTimezone = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eIsoDayOfWeek;
        if (children[9]) {
            eIsoDayOfWeek = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eIsoWeek;
        if (children[8]) {
            eIsoWeek = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eIsoWeekYear;
        if (children[7]) {
            eIsoWeekYear = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eMillisecond;
        if (children[6]) {
            eMillisecond = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eSecond;
        if (children[5]) {
            eSecond = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eMinute;
        if (children[4]) {
            eMinute = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eHour;
        if (children[3]) {
            eHour = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eDay;
        if (children[2]) {
            eDay = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eMonth;
        if (children[1]) {
            eMonth = _context->popABTExpr();
        }
        boost::optional<optimizer::ABT> eYear;
        if (children[0]) {
            eYear = _context->popABTExpr();
        }

        auto yearName = makeLocalVariableName(_context->state.frameId(), 0);
        auto monthName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dayName = makeLocalVariableName(_context->state.frameId(), 0);
        auto hourName = makeLocalVariableName(_context->state.frameId(), 0);
        auto minName = makeLocalVariableName(_context->state.frameId(), 0);
        auto secName = makeLocalVariableName(_context->state.frameId(), 0);
        auto millisecName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timeZoneName = makeLocalVariableName(_context->state.frameId(), 0);

        auto yearVar = makeVariable(yearName);
        auto monthVar = makeVariable(monthName);
        auto dayVar = makeVariable(dayName);
        auto hourVar = makeVariable(hourName);
        auto minVar = makeVariable(minName);
        auto secVar = makeVariable(secName);
        auto millisecVar = makeVariable(millisecName);
        auto timeZoneVar = makeVariable(timeZoneName);

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
            [](optimizer::ABT& var, int16_t lower, int16_t upper, const std::string& varName) {
                str::stream errMsg;
                if (varName == "year" || varName == "isoWeekYear") {
                    errMsg << "'" << varName << "'"
                           << " must evaluate to an integer in the range " << lower << " to "
                           << upper;
                } else {
                    errMsg << "'" << varName << "'"
                           << " must evaluate to a value in the range [" << lower << ", " << upper
                           << "]";
                }
                return std::make_pair(
                    optimizer::make<optimizer::BinaryOp>(
                        optimizer::Operations::And,
                        optimizer::make<optimizer::BinaryOp>(
                            optimizer::Operations::Gte, var, optimizer::Constant::int32(lower)),
                        optimizer::make<optimizer::BinaryOp>(
                            optimizer::Operations::Lte, var, optimizer::Constant::int32(upper))),
                    makeABTFail(ErrorCodes::Error{7157916}, errMsg));
            };

        // Here we want to validate each field that is provided as input to the agg expression. To
        // do this we implement the following checks:
        // 1) Check if the value in a given slot null or missing.
        // 2) Check if the value in a given slot is an integral int64.
        auto fieldConversionBinding = [](optimizer::ABT& expr,
                                         sbe::value::FrameIdGenerator* frameIdGenerator,
                                         const std::string& varName) {
            auto outerName = makeLocalVariableName(frameIdGenerator->generate(), 0);
            auto outerVar = makeVariable(outerName);
            auto convertedFieldName = makeLocalVariableName(frameIdGenerator->generate(), 0);
            auto convertedFieldVar = makeVariable(convertedFieldName);

            return optimizer::make<optimizer::Let>(
                outerName,
                expr,
                optimizer::make<optimizer::If>(
                    optimizer::make<optimizer::BinaryOp>(
                        optimizer::Operations::Or,
                        makeNot(makeABTFunction("exists", outerVar)),
                        makeABTFunction("isNull", outerVar)),
                    optimizer::Constant::null(),
                    optimizer::make<optimizer::Let>(
                        convertedFieldName,
                        makeABTFunction("convert",
                                        outerVar,
                                        optimizer::Constant::int32(static_cast<int32_t>(
                                            sbe::value::TypeTags::NumberInt64))),
                        optimizer::make<optimizer::If>(
                            makeABTFunction("exists", convertedFieldVar),
                            convertedFieldVar,
                            makeABTFail(ErrorCodes::Error{7157917},
                                        str::stream() << "'" << varName << "'"
                                                      << " must evaluate to an integer")))));
        };

        // Build two vectors on the fly to elide bound and conversion for defaulted values.
        std::vector<std::pair<optimizer::ABT, optimizer::ABT>>
            boundChecks;  // checks for lower and upper bounds of date fields.

        // Operands is for the outer let bindings.
        optimizer::ABTVector operands;
        if (eIsoWeekYear) {
            boundChecks.push_back(boundedCheck(yearVar, 1, 9999, "isoWeekYear"));
            operands.push_back(fieldConversionBinding(
                *eIsoWeekYear, _context->state.frameIdGenerator, "isoWeekYear"));
            if (!eIsoWeek) {
                operands.push_back(optimizer::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(monthVar, minInt16, maxInt16, "isoWeek"));
                operands.push_back(
                    fieldConversionBinding(*eIsoWeek, _context->state.frameIdGenerator, "isoWeek"));
            }
            if (!eIsoDayOfWeek) {
                operands.push_back(optimizer::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(dayVar, minInt16, maxInt16, "isoDayOfWeek"));
                operands.push_back(fieldConversionBinding(
                    *eIsoDayOfWeek, _context->state.frameIdGenerator, "isoDayOfWeek"));
            }
        } else {
            // The regular year/month/day case.
            if (!eYear) {
                operands.push_back(optimizer::Constant::int32(1970));
            } else {
                boundChecks.push_back(boundedCheck(yearVar, 1, 9999, "year"));
                operands.push_back(
                    fieldConversionBinding(*eYear, _context->state.frameIdGenerator, "year"));
            }
            if (!eMonth) {
                operands.push_back(optimizer::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(monthVar, minInt16, maxInt16, "month"));
                operands.push_back(
                    fieldConversionBinding(*eMonth, _context->state.frameIdGenerator, "month"));
            }
            if (!eDay) {
                operands.push_back(optimizer::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(dayVar, minInt16, maxInt16, "day"));
                operands.push_back(
                    fieldConversionBinding(*eDay, _context->state.frameIdGenerator, "day"));
            }
        }
        if (!eHour) {
            operands.push_back(optimizer::Constant::int32(0));
        } else {
            boundChecks.push_back(boundedCheck(hourVar, minInt16, maxInt16, "hour"));
            operands.push_back(
                fieldConversionBinding(*eHour, _context->state.frameIdGenerator, "hour"));
        }
        if (!eMinute) {
            operands.push_back(optimizer::Constant::int32(0));
        } else {
            boundChecks.push_back(boundedCheck(minVar, minInt16, maxInt16, "minute"));
            operands.push_back(
                fieldConversionBinding(*eMinute, _context->state.frameIdGenerator, "minute"));
        }
        if (!eSecond) {
            operands.push_back(optimizer::Constant::int32(0));
        } else {
            // MQL doesn't place bound restrictions on the second field, because seconds carry over
            // to minutes and can be large ints such as 71,841,012 or even unix epochs.
            operands.push_back(
                fieldConversionBinding(*eSecond, _context->state.frameIdGenerator, "second"));
        }
        if (!eMillisecond) {
            operands.push_back(optimizer::Constant::int32(0));
        } else {
            // MQL doesn't enforce bound restrictions on millisecond fields because milliseconds
            // carry over to seconds.
            operands.push_back(fieldConversionBinding(
                *eMillisecond, _context->state.frameIdGenerator, "millisecond"));
        }
        if (!eTimezone) {
            operands.push_back(optimizer::Constant::str("UTC"));
        } else {
            // Validate that eTimezone is a string.
            auto timeZoneName = makeLocalVariableName(_context->state.frameId(), 0);
            auto timeZoneVar = makeVariable(timeZoneName);
            operands.push_back(optimizer::make<optimizer::Let>(
                timeZoneName,
                *eTimezone,
                optimizer::make<optimizer::If>(
                    makeABTFunction("isString", timeZoneVar),
                    timeZoneVar,
                    makeABTFail(ErrorCodes::Error{7157918},
                                str::stream() << "'timezone' must evaluate to a string"))));
        }

        // Make a disjunction of null checks for each date part by over this vector. These checks
        // are necessary after the initial conversion computation because we need have the outer let
        // binding evaluate to null if any field is null.
        auto nullExprs = optimizer::ABTVector{generateABTNullOrMissing(timeZoneName),
                                              generateABTNullOrMissing(millisecName),
                                              generateABTNullOrMissing(secName),
                                              generateABTNullOrMissing(minName),
                                              generateABTNullOrMissing(hourName),
                                              generateABTNullOrMissing(dayName),
                                              generateABTNullOrMissing(monthName),
                                              generateABTNullOrMissing(yearName)};

        auto checkPartsForNull =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(nullExprs));

        // Invocation of the datePartsWeekYear and dateParts functions depend on a TimeZoneDatabase
        // for datetime computation. This global object is registered as an unowned value in the
        // runtime environment so we pass the corresponding slot to the datePartsWeekYear and
        // dateParts functions as a variable.
        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);
        auto computeDate = makeABTFunction(eIsoWeekYear ? "datePartsWeekYear" : "dateParts",
                                           makeVariable(timeZoneDBName),
                                           yearVar,
                                           monthVar,
                                           dayVar,
                                           hourVar,
                                           minVar,
                                           secVar,
                                           millisecVar,
                                           timeZoneVar);

        using iterPair_t = std::vector<std::pair<optimizer::ABT, optimizer::ABT>>::iterator;
        auto computeBoundChecks =
            std::accumulate(std::move_iterator<iterPair_t>(boundChecks.begin()),
                            std::move_iterator<iterPair_t>(boundChecks.end()),
                            std::move(computeDate),
                            [](auto&& acc, auto&& b) {
                                return optimizer::make<optimizer::If>(
                                    std::move(b.first), std::move(acc), std::move(b.second));
                            });

        // This final ite expression allows short-circuting of the null field case. If the nullish,
        // checks pass, then we check the bounds of each field and invoke the builtins if all checks
        // pass.
        auto computeDateOrNull = optimizer::make<optimizer::If>(std::move(checkPartsForNull),
                                                                optimizer::Constant::null(),
                                                                std::move(computeBoundChecks));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(yearName),
            std::move(operands[0]),
            optimizer::make<optimizer::Let>(
                std::move(monthName),
                std::move(operands[1]),
                optimizer::make<optimizer::Let>(
                    std::move(dayName),
                    std::move(operands[2]),
                    optimizer::make<optimizer::Let>(
                        std::move(hourName),
                        std::move(operands[3]),
                        optimizer::make<optimizer::Let>(
                            std::move(minName),
                            std::move(operands[4]),
                            optimizer::make<optimizer::Let>(
                                std::move(secName),
                                std::move(operands[5]),
                                optimizer::make<optimizer::Let>(
                                    std::move(millisecName),
                                    std::move(operands[6]),
                                    optimizer::make<optimizer::Let>(
                                        std::move(timeZoneName),
                                        std::move(operands[7]),
                                        std::move(computeDateOrNull))))))))));
    }

    void visit(const ExpressionDateToParts* expr) final {
        auto children = expr->getChildren();
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        auto isoflagName = makeLocalVariableName(_context->state.frameId(), 0);

        auto dateVar = makeVariable(dateName);
        auto timezoneVar = makeVariable(timezoneName);
        auto isoflagVar = makeVariable(isoflagName);

        // Initialize arguments with values from stack or default values.
        auto isoflag = optimizer::Constant::boolean(false);
        if (children[2]) {
            isoflag = _context->popABTExpr();
        }
        auto timezone = optimizer::Constant::str("UTC");
        if (children[1]) {
            timezone = _context->popABTExpr();
        }
        if (!children[0]) {
            pushABT(makeABTFail(ErrorCodes::Error{7157911}, "$dateToParts must include a date"));
            return;
        }
        auto date = _context->popABTExpr();

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);
        auto timeZoneDBVar = makeVariable(timeZoneDBName);

        auto isoTypeMask = getBSONTypeMask(sbe::value::TypeTags::Boolean);

        // Determine whether to call dateToParts or isoDateToParts.
        auto checkIsoflagValue = buildABTMultiBranchConditional(
            ABTCaseValuePair{
                optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Eq, isoflagVar, optimizer::Constant::boolean(false)),
                makeABTFunction("dateToParts", timeZoneDBVar, dateVar, timezoneVar, isoflagVar)},
            makeABTFunction("isoDateToParts", timeZoneDBVar, dateVar, timezoneVar, isoflagVar));

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDateToPartsFunc = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(timezoneName), optimizer::Constant::null()},
            ABTCaseValuePair{
                makeNot(makeABTFunction("isString", timezoneVar)),
                makeABTFail(ErrorCodes::Error{7157912}, "$dateToParts timezone must be a string")},
            ABTCaseValuePair{makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                             makeABTFail(ErrorCodes::Error{7157913},
                                         "$dateToParts timezone must be a valid timezone")},
            ABTCaseValuePair{generateABTNullOrMissing(isoflagName), optimizer::Constant::null()},
            ABTCaseValuePair{
                makeNot(makeABTFunction(
                    "typeMatch", isoflagVar, optimizer::Constant::int32(isoTypeMask))),
                makeABTFail(ErrorCodes::Error{7157914}, "$dateToParts iso8601 must be a boolean")},
            ABTCaseValuePair{generateABTNullOrMissing(dateName), optimizer::Constant::null()},
            ABTCaseValuePair{makeNot(makeABTFunction(
                                 "typeMatch", dateVar, optimizer::Constant::int32(dateTypeMask()))),
                             makeABTFail(ErrorCodes::Error{7157915},
                                         "$dateToParts date must have the format of a date")},
            std::move(checkIsoflagValue));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(dateName),
            std::move(date),
            optimizer::make<optimizer::Let>(
                std::move(timezoneName),
                std::move(timezone),
                optimizer::make<optimizer::Let>(
                    std::move(isoflagName), std::move(isoflag), std::move(totalDateToPartsFunc)))));
    }

    void visit(const ExpressionDateToString* expr) final {
        auto children = expr->getChildren();
        invariant(children.size() == 4);
        _context->ensureArity(1 + (expr->isFormatSpecified() ? 1 : 0) +
                              (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isOnNullSpecified() ? 1 : 0));

        // Get child expressions.
        auto onNullExpression =
            expr->isOnNullSpecified() ? _context->popABTExpr() : optimizer::Constant::null();

        auto timezoneExpression = expr->isTimezoneSpecified() ? _context->popABTExpr()
                                                              : optimizer::Constant::str("UTC"_sd);
        auto dateExpression = _context->popABTExpr();

        auto formatExpression = expr->isFormatSpecified()
            ? _context->popABTExpr()
            : optimizer::Constant::str("%Y-%m-%dT%H:%M:%S.%LZ"_sd);

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);
        auto timeZoneDBVar = makeVariable(timeZoneDBName);
        auto [timezoneDBTag, timezoneDBVal] =
            _context->state.data->env->getAccessor(timeZoneDBSlot)->getViewOfValue();
        uassert(4997900,
                "$dateToString first argument must be a timezoneDB object",
                timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
        auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);

        // Local bind to hold the date expression result
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateVar = makeVariable(dateName);

        // Set parameters for an invocation of built-in "dateToString" function.
        optimizer::ABTVector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(dateExpression);
        arguments.push_back(formatExpression);
        arguments.push_back(timezoneExpression);

        // Create an expression to invoke built-in "dateToString" function.
        auto dateToStringFunctionCall =
            optimizer::make<optimizer::FunctionCall>("dateToString", std::move(arguments));
        auto dateToStringName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateToStringVar = makeVariable(dateToStringName);

        // Create expressions to check that each argument to "dateToString" function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;
        // Return onNull if date is null or missing.
        inputValidationCases.push_back(
            {generateABTNullOrMissing(dateExpression), onNullExpression});
        // Return null if format or timezone is null or missing.
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(formatExpression));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(timezoneExpression));

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            dateVar, ErrorCodes::Error{4997901}, "$dateToString"_sd, "date"_sd));

        // "format" parameter validation.
        if (auto* formatExpressionConst = formatExpression.cast<optimizer::Constant>();
            formatExpressionConst) {
            auto [formatTag, formatVal] = formatExpressionConst->get();
            if (!sbe::value::isNullish(formatTag)) {
                // We don't want to return an error on null.
                uassert(4997902,
                        "$dateToString parameter 'format' must be a string",
                        sbe::value::isString(formatTag));
                TimeZone::validateToStringFormat(getStringView(formatTag, formatVal));
            }
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(formatExpression),
                makeABTFail(ErrorCodes::Error{4997903},
                            "$dateToString parameter 'format' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isValidToStringFormat", formatExpression)),
                makeABTFail(ErrorCodes::Error{4997904},
                            "$dateToString parameter 'format' must be a valid format"));
        }

        // "timezone" parameter validation.
        if (auto* timezoneExpressionConst = timezoneExpression.cast<optimizer::Constant>();
            timezoneExpressionConst) {
            auto [timezoneTag, timezoneVal] = timezoneExpressionConst->get();
            if (!sbe::value::isNullish(timezoneTag)) {
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
                generateABTNonStringCheck(timezoneExpression),
                makeABTFail(ErrorCodes::Error{4997907},
                            "$dateToString parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneExpression)),
                makeABTFail(ErrorCodes::Error{4997908},
                            "$dateToString parameter 'timezone' must be a valid timezone"));
        }

        pushABT(optimizer::make<optimizer::Let>(
            std::move(dateName),
            std::move(dateExpression),
            optimizer::make<optimizer::Let>(
                std::move(dateToStringName),
                std::move(dateToStringFunctionCall),
                optimizer::make<optimizer::If>(
                    makeABTFunction("exists", dateToStringVar),
                    dateToStringVar,
                    buildABTMultiBranchConditionalFromCaseValuePairs(
                        std::move(inputValidationCases), optimizer::Constant::nothing())))));
    }
    void visit(const ExpressionDateTrunc* expr) final {
        auto children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(2 + (expr->isBinSizeSpecified() ? 1 : 0) +
                              (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isStartOfWeekSpecified() ? 1 : 0));

        // Get child expressions.
        auto startOfWeekExpression = expr->isStartOfWeekSpecified()
            ? _context->popABTExpr()
            : optimizer::Constant::str("sun"_sd);
        auto timezoneExpression = expr->isTimezoneSpecified() ? _context->popABTExpr()
                                                              : optimizer::Constant::str("UTC"_sd);
        auto binSizeExpression =
            expr->isBinSizeSpecified() ? _context->popABTExpr() : optimizer::Constant::int64(1);
        auto unitExpression = _context->popABTExpr();
        auto dateExpression = _context->popABTExpr();

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);
        auto timeZoneDBVar = makeVariable(timeZoneDBName);
        auto [timezoneDBTag, timezoneDBVal] =
            _context->state.data->env->getAccessor(timeZoneDBSlot)->getViewOfValue();
        tassert(7157927,
                "$dateTrunc first argument must be a timezoneDB object",
                timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
        auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);

        // Local bind to hold the date expression result
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateVar = makeVariable(dateName);

        // Set parameters for an invocation of built-in "dateTrunc" function.
        optimizer::ABTVector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(dateVar);
        arguments.push_back(unitExpression);
        arguments.push_back(binSizeExpression);
        arguments.push_back(timezoneExpression);
        arguments.push_back(startOfWeekExpression);

        // Create an expression to invoke built-in "dateTrunc" function.
        auto dateTruncFunctionCall =
            optimizer::make<optimizer::FunctionCall>("dateTrunc", std::move(arguments));
        auto dateTruncName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateTruncVar = makeVariable(dateTruncName);

        // Local bind to hold the unitIsWeek common subexpression
        auto unitIsWeekName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitIsWeekVar = makeVariable(unitIsWeekName);
        auto unitIsWeek = generateABTIsEqualToStringCheck(unitExpression, "week"_sd);

        // Create expressions to check that each argument to "dateTrunc" function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(dateVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(unitExpression));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(binSizeExpression));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(timezoneExpression));
        inputValidationCases.emplace_back(
            optimizer::make<optimizer::BinaryOp>(optimizer::Operations::And,
                                                 unitIsWeekVar,
                                                 generateABTNullOrMissing(startOfWeekExpression)),
            optimizer::Constant::null());

        // "timezone" parameter validation.
        if (timezoneExpression.is<optimizer::Constant>()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.cast<optimizer::Constant>()->get();
            tassert(7157928,
                    "$dateTrunc parameter 'timezone' must be a string",
                    sbe::value::isString(timezoneTag));
            tassert(7157929,
                    "$dateTrunc parameter 'timezone' must be a valid timezone",
                    sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(timezoneExpression),
                makeABTFail(ErrorCodes::Error{7157930},
                            "$dateTrunc parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneExpression)),
                makeABTFail(ErrorCodes::Error{7157931},
                            "$dateTrunc parameter 'timezone' must be a valid timezone"));
        }

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            dateVar, ErrorCodes::Error{7157932}, "$dateTrunc"_sd, "date"_sd));

        // "unit" parameter validation.
        if (unitExpression.is<optimizer::Constant>()) {
            auto [unitTag, unitVal] = unitExpression.cast<optimizer::Constant>()->get();
            tassert(7157933,
                    "$dateTrunc parameter 'unit' must be a string",
                    sbe::value::isString(unitTag));
            auto unitString = sbe::value::getStringView(unitTag, unitVal);
            tassert(7157934,
                    "$dateTrunc parameter 'unit' must be a valid time unit",
                    isValidTimeUnit(unitString));
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(unitExpression),
                makeABTFail(ErrorCodes::Error{7157935},
                            "$dateTrunc parameter 'unit' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isTimeUnit", unitExpression)),
                makeABTFail(ErrorCodes::Error{7157936},
                            "$dateTrunc parameter 'unit' must be a valid time unit"));
        }

        // "binSize" parameter validation.
        if (expr->isBinSizeSpecified()) {
            if (binSizeExpression.is<optimizer::Constant>()) {
                auto [binSizeTag, binSizeValue] =
                    binSizeExpression.cast<optimizer::Constant>()->get();
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
                    makeNot(optimizer::make<optimizer::BinaryOp>(
                        optimizer::Operations::And,
                        optimizer::make<optimizer::BinaryOp>(
                            optimizer::Operations::And,
                            makeABTFunction("isNumber", binSizeExpression),
                            makeABTFunction(
                                "exists",
                                makeABTFunction("convert",
                                                binSizeExpression,
                                                optimizer::Constant::int32(static_cast<int32_t>(
                                                    sbe::value::TypeTags::NumberInt64))))),
                        generateABTPositiveCheck(binSizeExpression))),
                    makeABTFail(
                        ErrorCodes::Error{7157940},
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer"));
            }
        }

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            if (startOfWeekExpression.is<optimizer::Constant>()) {
                auto [startOfWeekTag, startOfWeekVal] =
                    startOfWeekExpression.cast<optimizer::Constant>()->get();
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
                    optimizer::make<optimizer::BinaryOp>(
                        optimizer::Operations::And,
                        unitIsWeekVar,
                        generateABTNonStringCheck(startOfWeekExpression)),
                    makeABTFail(ErrorCodes::Error{7157943},
                                "$dateTrunc parameter 'startOfWeek' must be a string"));
                inputValidationCases.emplace_back(
                    optimizer::make<optimizer::BinaryOp>(
                        optimizer::Operations::And,
                        unitIsWeekVar,
                        makeNot(makeABTFunction("isDayOfWeek", startOfWeekExpression))),
                    makeABTFail(
                        ErrorCodes::Error{7157944},
                        "$dateTrunc parameter 'startOfWeek' must be a valid day of the week"));
            }
        }

        pushABT(optimizer::make<optimizer::Let>(
            std::move(dateName),
            std::move(dateExpression),
            optimizer::make<optimizer::Let>(
                std::move(dateTruncName),
                std::move(dateTruncFunctionCall),
                optimizer::make<optimizer::If>(
                    makeABTFunction("exists", dateTruncVar),
                    dateTruncVar,
                    optimizer::make<optimizer::Let>(
                        std::move(unitIsWeekName),
                        std::move(unitIsWeek),
                        buildABTMultiBranchConditionalFromCaseValuePairs(
                            std::move(inputValidationCases), optimizer::Constant::nothing()))))));
    }
    void visit(const ExpressionDivide* expr) final {
        _context->ensureArity(2);
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();

        auto lhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto checkIsNumber = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::And,
            makeABTFunction("isNumber", makeVariable(lhsName)),
            makeABTFunction("isNumber", makeVariable(rhsName)));

        auto checkIsNullOrMissing =
            optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                 generateABTNullOrMissing(lhsName),
                                                 generateABTNullOrMissing(rhsName));

        auto divideExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkIsNullOrMissing), optimizer::Constant::null()},
            ABTCaseValuePair{std::move(checkIsNumber),
                             optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Div,
                                                                  makeVariable(lhsName),
                                                                  makeVariable(rhsName))},
            makeABTFail(ErrorCodes::Error{7157719}, "$divide only supports numeric types"));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(lhsName),
            std::move(lhs),
            optimizer::make<optimizer::Let>(
                std::move(rhsName), std::move(rhs), std::move(divideExpr))));
    }
    void visit(const ExpressionExp* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto expExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157704}, "$exp only supports numeric types")},
            makeABTFunction("exp", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(expExpr)));
    }
    void visit(const ExpressionFieldPath* expr) final {
        EvalExpr inputExpr;
        boost::optional<sbe::value::SlotId> topLevelFieldSlot;
        bool expectsDocumentInputOnly = false;
        auto fp = (expr->getFieldPath().getPathLength() > 1)
            ? boost::make_optional(expr->getFieldPathWithoutCurrentPrefix())
            : boost::none;

        if (!Variables::isUserDefinedVariable(expr->getVariableId())) {
            const auto* slots = _context->slots;
            if (expr->getVariableId() == Variables::kRootId) {
                // Set inputExpr to refer to the root document.
                inputExpr = _context->rootSlot ? EvalExpr{*_context->rootSlot} : EvalExpr{};
                expectsDocumentInputOnly = true;

                if (slots && fp) {
                    // Check if we already have a slot containing an expression corresponding
                    // to 'expr'.
                    auto fpe = std::make_pair(PlanStageSlots::kPathExpr, fp->fullPath());
                    if (slots->has(fpe)) {
                        _context->pushExpr(slots->get(fpe));
                        return;
                    }

                    // Obtain a slot for the top-level field referred to by 'expr', if one
                    // exists.
                    auto topLevelField = std::make_pair(PlanStageSlots::kField, fp->front());
                    topLevelFieldSlot = slots->getIfExists(topLevelField);
                }
            } else if (expr->getVariableId() == Variables::kRemoveId) {
                // For the field paths that begin with "$$REMOVE", we always produce Nothing,
                // so no traversal is necessary.
                pushABT(optimizer::Constant::nothing());
                return;
            } else {
                auto it = Variables::kIdToBuiltinVarName.find(expr->getVariableId());
                tassert(5611300,
                        "Encountered unexpected system variable ID",
                        it != Variables::kIdToBuiltinVarName.end());

                auto slot = _context->state.data->env->getSlotIfExists(it->second);
                uassert(5611301,
                        str::stream()
                            << "Builtin variable '$$" << it->second << "' is not available",
                        slot.has_value());

                inputExpr = *slot;
            }
        } else {
            auto it = _context->environment.find(expr->getVariableId());
            if (it != _context->environment.end()) {
                inputExpr = abt::wrap(makeVariable(makeLocalVariableName(it->second, 0)));
            } else {
                inputExpr = _context->state.getGlobalVariableSlot(expr->getVariableId());
            }
        }

        if (expr->getFieldPath().getPathLength() == 1) {
            tassert(6929400, "Expected a valid input expression", !inputExpr.isNull());

            // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
            // traversal.
            _context->pushExpr(inputExpr.extractABT(_context->state.slotVarMap));
            return;
        }

        tassert(6929401,
                "Expected a valid input expression or a valid field slot",
                !inputExpr.isNull() || topLevelFieldSlot.has_value());

        // Dereference a dotted path, which may contain arrays requiring implicit traversal.
        auto resultExpr = generateTraverse(
            _context,
            inputExpr.isNull() ? boost::optional<optimizer::ABT>{}
                               : abt::unwrap(inputExpr.extractABT(_context->state.slotVarMap)),
            expectsDocumentInputOnly,
            *fp,
            _context->state.frameIdGenerator,
            topLevelFieldSlot);
        pushABT(std::move(resultExpr));
    }
    void visit(const ExpressionFilter* expr) final {
        unsupportedExpression("$filter");
    }

    void visit(const ExpressionFloor* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto floorExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157703}, "$floor only supports numeric types")},
            makeABTFunction("floor", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(floorExpr)));
    }
    void visit(const ExpressionIfNull* expr) final {
        auto numChildren = expr->getChildren().size();
        invariant(numChildren >= 2);

        std::vector<optimizer::ABT> values;
        values.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            values.emplace_back(_context->popABTExpr());
        }
        std::reverse(values.begin(), values.end());

        auto resultExpr = makeIfNullExpr(std::move(values), _context->state.frameIdGenerator);

        pushABT(std::move(resultExpr));
    }
    void visit(const ExpressionIn* expr) final {
        unsupportedExpression(expr->getOpName());
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
        auto arg = _context->popABTExpr();
        auto varName = makeLocalVariableName(_context->state.frameId(), 0);
        auto exprIsNum =
            optimizer::make<optimizer::If>(makeABTFunction("exists", makeVariable(varName)),
                                           makeABTFunction("isNumber", makeVariable(varName)),
                                           optimizer::Constant::boolean(false));

        pushABT(optimizer::make<optimizer::Let>(varName, std::move(arg), std::move(exprIsNum)));
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

        auto resultExpr = _context->popABTExpr();
        for (auto& binding : currentFrame.bindings) {
            resultExpr = optimizer::make<optimizer::Let>(
                makeLocalVariableName(binding.frameId, 0),
                abt::unwrap(binding.expr.extractABT(_context->state.slotVarMap)),
                std::move(resultExpr));
        }

        pushABT(std::move(resultExpr));

        // Pop the lexical frame for this $let and remove all its bindings, which are now out of
        // scope.
        for (const auto& binding : currentFrame.bindings) {
            _context->environment.erase(binding.variableId);
        }
        _context->varsFrameStack.pop();
    }

    void visit(const ExpressionLn* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto lnExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157705}, "$ln only supports numeric types")},
            // Note: In MQL, $ln on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            ABTCaseValuePair{generateABTNaNCheck(inputName),
                             makeABTFunction("convert",
                                             makeVariable(inputName),
                                             optimizer::Constant::int32(static_cast<int32_t>(
                                                 sbe::value::TypeTags::NumberDouble)))},
            ABTCaseValuePair{generateABTNonPositiveCheck(inputName),
                             makeABTFail(ErrorCodes::Error{7157706},
                                         "$ln's argument must be a positive number")},
            makeABTFunction("ln", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(lnExpr)));
    }
    void visit(const ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionLog10* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto log10Expr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157707}, "$log10 only supports numeric types")},
            // Note: In MQL, $log10 on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            ABTCaseValuePair{generateABTNaNCheck(inputName),
                             makeABTFunction("convert",
                                             makeVariable(inputName),
                                             optimizer::Constant::int32(static_cast<int32_t>(
                                                 sbe::value::TypeTags::NumberDouble)))},
            ABTCaseValuePair{generateABTNonPositiveCheck(inputName),
                             makeABTFail(ErrorCodes::Error{7157708},
                                         "$log10's argument must be a positive number")},
            makeABTFunction("log10", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(log10Expr)));
    }
    void visit(const ExpressionInternalFLEBetween* expr) final {
        unsupportedExpression("$_internalFleBetween");
    }
    void visit(const ExpressionInternalFLEEqual* expr) final {
        unsupportedExpression("$_internalFleEq");
    }
    void visit(const ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }
    void visit(const ExpressionMeta* expr) final {
        unsupportedExpression("$meta");
    }
    void visit(const ExpressionMod* expr) final {
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();
        auto lhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        // If the rhs is a small integral double, convert it to int32 to match $mod MQL semantics.
        auto numericConvert32 = makeABTFunction(
            "convert",
            makeVariable(rhsName),
            optimizer::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt32)));
        auto rhsExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{
                optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::And,
                    makeABTFunction("typeMatch",
                                    makeVariable(rhsName),
                                    optimizer::Constant::int32(
                                        getBSONTypeMask(sbe::value::TypeTags::NumberDouble))),
                    makeNot(makeABTFunction("typeMatch",
                                            makeVariable(lhsName),
                                            optimizer::Constant::int32(getBSONTypeMask(
                                                sbe::value::TypeTags::NumberDouble))))),
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::FillEmpty,
                                                     std::move(numericConvert32),
                                                     makeVariable(rhsName))},
            makeVariable(rhsName));

        auto modExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                     generateABTNullOrMissing(lhsName),
                                                     generateABTNullOrMissing(rhsName)),
                optimizer::Constant::null()},
            ABTCaseValuePair{
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                     generateABTNonNumericCheck(lhsName),
                                                     generateABTNonNumericCheck(rhsName)),
                makeABTFail(ErrorCodes::Error{7157718}, "$mod only supports numeric types")},
            makeABTFunction("mod", makeVariable(lhsName), std::move(rhsExpr)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(lhsName),
            std::move(lhs),
            optimizer::make<optimizer::Let>(
                std::move(rhsName), std::move(rhs), std::move(modExpr))));
    }
    void visit(const ExpressionMultiply* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity < kArgumentCountForBinaryTree) {
            visitFast(expr);
            return;
        }

        auto checkLeaf = [&](optimizer::ABT arg) {
            auto name = makeLocalVariableName(_context->state.frameId(), 0);
            auto var = makeVariable(name);
            auto checkedLeaf = buildABTMultiBranchConditional(
                ABTCaseValuePair{makeABTFunction("isNumber", var), var},
                makeABTFail(ErrorCodes::Error{7315403},
                            "only numbers are allowed in an $multiply expression"));
            return optimizer::make<optimizer::Let>(
                std::move(name), std::move(arg), std::move(checkedLeaf));
        };

        auto combineTwoTree = [&](optimizer::ABT left, optimizer::ABT right) {
            auto nameLeft = makeLocalVariableName(_context->state.frameId(), 0);
            auto nameRight = makeLocalVariableName(_context->state.frameId(), 0);
            auto varLeft = makeVariable(nameLeft);
            auto varRight = makeVariable(nameRight);

            auto mulExpr = buildABTMultiBranchConditional(
                ABTCaseValuePair{
                    optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                         generateABTNullOrMissing(nameLeft),
                                                         generateABTNullOrMissing(nameRight)),
                    optimizer::Constant::null()},
                optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Mult, varLeft, varRight));
            return optimizer::make<optimizer::Let>(
                std::move(nameLeft),
                std::move(left),
                optimizer::make<optimizer::Let>(
                    std::move(nameRight), std::move(right), std::move(mulExpr)));
        };

        optimizer::ABTVector leaves;
        leaves.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            leaves.emplace_back(checkLeaf(_context->popABTExpr()));
        }
        std::reverse(std::begin(leaves), std::end(leaves));

        pushABT(makeBalancedTree(combineTwoTree, std::move(leaves)));
    }
    void visitFast(const ExpressionMultiply* expr) {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Return multiplicative identity if the $multiply expression has no operands.
        if (arity == 0) {
            pushABT(optimizer::Constant::int32(1));
            return;
        }

        optimizer::ABTVector binds;
        optimizer::ProjectionNameVector names;
        optimizer::ABTVector checkExprsNull;
        optimizer::ABTVector checkExprsNumber;
        optimizer::ABTVector variables;
        binds.reserve(arity);
        names.reserve(arity);
        variables.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNumber.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            binds.push_back(_context->popABTExpr());
            auto currentName =
                makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
            names.push_back(currentName);

            checkExprsNull.push_back(generateABTNullOrMissing(currentName));
            checkExprsNumber.push_back(makeABTFunction("isNumber", makeVariable(currentName)));
            variables.push_back(makeVariable(currentName));
        }

        // At this point 'binds' vector contains arguments of $multiply expression in the reversed
        // order. We need to reverse it back to perform multiplication in the right order below.
        // Multiplication in different order can lead to different result because of accumulated
        // precision errors from floating point types.
        std::reverse(std::begin(binds), std::end(binds));

        auto checkNullAnyArgument =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(checkExprsNull));
        auto checkNumberAllArguments =
            makeBalancedBooleanOpTree(optimizer::Operations::And, std::move(checkExprsNumber));
        auto multiplication = std::accumulate(
            names.begin() + 1, names.end(), makeVariable(names.front()), [](auto&& acc, auto&& ex) {
                return optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Mult, std::move(acc), makeVariable(ex));
            });

        auto multiplyExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkNullAnyArgument), optimizer::Constant::null()},
            ABTCaseValuePair{std::move(checkNumberAllArguments), std::move(multiplication)},
            makeABTFail(ErrorCodes::Error{7157721},
                        "only numbers are allowed in an $multiply expression"));

        for (size_t i = 0; i < arity; ++i) {
            multiplyExpr = optimizer::make<optimizer::Let>(
                std::move(names[i]), std::move(binds[i]), std::move(multiplyExpr));
        }

        pushABT(std::move(multiplyExpr));
    }
    void visit(const ExpressionNot* expr) final {
        pushABT(
            makeNot(makeFillEmptyFalse(makeABTFunction("coerceToBool", _context->popABTExpr()))));
    }
    void visit(const ExpressionObject* expr) final {
        const auto& childExprs = expr->getChildExpressions();
        size_t childSize = childExprs.size();
        _context->ensureArity(childSize);

        // The expression argument for 'newObj' must be a sequence of a field name constant
        // expression and an expression for the value. So, we need 2 * childExprs.size() elements in
        // the expressions vector.
        optimizer::ABTVector exprs;
        exprs.reserve(childSize * 2);

        // We iterate over child expressions in reverse, because they will be popped from stack in
        // reverse order.
        for (auto rit = childExprs.rbegin(); rit != childExprs.rend(); ++rit) {
            exprs.push_back(_context->popABTExpr());
            exprs.push_back(optimizer::Constant::str(rit->first));
        }

        // Lastly we need to reverse it to get the correct order of arguments.
        std::reverse(exprs.begin(), exprs.end());

        pushABT(optimizer::make<optimizer::FunctionCall>("newObj", std::move(exprs)));
    }

    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, optimizer::Operations::Or);
    }
    void visit(const ExpressionPow* expr) final {
        unsupportedExpression("$pow");
    }
    void visit(const ExpressionRange* expr) final {
        auto startName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto endName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto stepName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto convertedStartName =
            makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto convertedEndName =
            makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto convertedStepName =
            makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto step = expr->getChildren().size() == 3 ? _context->popABTExpr()
                                                    : optimizer::Constant::int32(1);
        auto end = _context->popABTExpr();
        auto start = _context->popABTExpr();

        auto rangeExpr = optimizer::make<optimizer::Let>(
            startName,
            std::move(start),
            optimizer::make<optimizer::Let>(
                endName,
                std::move(end),
                optimizer::make<optimizer::Let>(
                    stepName,
                    std::move(step),
                    buildABTMultiBranchConditional(
                        ABTCaseValuePair{
                            generateABTNonNumericCheck(startName),
                            makeABTFail(ErrorCodes::Error{7157711},
                                        "$range only supports numeric types for start")},
                        ABTCaseValuePair{generateABTNonNumericCheck(endName),
                                         makeABTFail(ErrorCodes::Error{7157712},
                                                     "$range only supports numeric types for end")},
                        ABTCaseValuePair{
                            generateABTNonNumericCheck(stepName),
                            makeABTFail(ErrorCodes::Error{7157713},
                                        "$range only supports numeric types for step")},
                        optimizer::make<optimizer::Let>(
                            convertedStartName,
                            makeABTFunction("convert",
                                            makeVariable(startName),
                                            optimizer::Constant::int32(static_cast<int32_t>(
                                                sbe::value::TypeTags::NumberInt32))),
                            optimizer::make<optimizer::Let>(
                                convertedEndName,
                                makeABTFunction("convert",
                                                makeVariable(endName),
                                                optimizer::Constant::int32(static_cast<int32_t>(
                                                    sbe::value::TypeTags::NumberInt32))),
                                optimizer::make<optimizer::Let>(
                                    convertedStepName,
                                    makeABTFunction("convert",
                                                    makeVariable(stepName),
                                                    optimizer::Constant::int32(static_cast<int32_t>(
                                                        sbe::value::TypeTags::NumberInt32))),
                                    buildABTMultiBranchConditional(
                                        ABTCaseValuePair{
                                            makeNot(makeABTFunction(
                                                "exists", makeVariable(convertedStartName))),
                                            makeABTFail(ErrorCodes::Error{7157714},
                                                        "$range start argument cannot be "
                                                        "represented as a 32-bit integer")},
                                        ABTCaseValuePair{
                                            makeNot(makeABTFunction(
                                                "exists", makeVariable(convertedEndName))),
                                            makeABTFail(ErrorCodes::Error{7157715},
                                                        "$range end argument cannot be represented "
                                                        "as a 32-bit integer")},
                                        ABTCaseValuePair{
                                            makeNot(makeABTFunction(
                                                "exists", makeVariable(convertedStepName))),
                                            makeABTFail(ErrorCodes::Error{7157716},
                                                        "$range step argument cannot be "
                                                        "represented as a 32-bit integer")},
                                        ABTCaseValuePair{
                                            optimizer::make<optimizer::BinaryOp>(
                                                optimizer::Operations::Eq,
                                                makeVariable(convertedStepName),
                                                optimizer::Constant::int32(0)),
                                            makeABTFail(ErrorCodes::Error{7157717},
                                                        "$range requires a non-zero step value")},
                                        makeABTFunction("newArrayFromRange",
                                                        makeVariable(convertedStartName),
                                                        makeVariable(convertedEndName),
                                                        makeVariable(convertedStepName))))))))));

        pushABT(std::move(rangeExpr));
    }

    void visit(const ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(const ExpressionReplaceOne* expr) final {
        _context->ensureArity(3);

        auto replacementArg = _context->popABTExpr();
        auto findArg = _context->popABTExpr();
        auto inputArg = _context->popABTExpr();

        auto inputArgName = makeLocalVariableName(_context->state.frameId(), 0);
        auto findArgName = makeLocalVariableName(_context->state.frameId(), 0);
        auto replacementArgName = makeLocalVariableName(_context->state.frameId(), 0);

        auto inputArgNullName = makeLocalVariableName(_context->state.frameId(), 0);
        auto findArgNullName = makeLocalVariableName(_context->state.frameId(), 0);
        auto replacementArgNullName = makeLocalVariableName(_context->state.frameId(), 0);

        auto checkNull = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Or,
            optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                 makeVariable(inputArgNullName),
                                                 makeVariable(findArgNullName)),
            makeVariable(replacementArgNullName));

        // Check if find string is empty, and if so return the the concatenation of the replacement
        // string and the input string, otherwise replace the first occurrence of the find string.
        auto isEmptyFindStr = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Eq, makeVariable(findArgName), optimizer::Constant::str(""_sd));

        auto replaceOneExpr = optimizer::make<optimizer::If>(
            std::move(isEmptyFindStr),
            makeABTFunction("concat", makeVariable(replacementArgName), makeVariable(inputArgName)),
            makeABTFunction("replaceOne",
                            makeVariable(inputArgName),
                            makeVariable(findArgName),
                            makeVariable(replacementArgName)));

        auto generateTypeCheckCaseValuePair = [](optimizer::ProjectionName paramName,
                                                 optimizer::ProjectionName paramIsNullName,
                                                 StringData param) {
            return ABTCaseValuePair{
                makeNot(optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Or,
                    makeVariable(std::move(paramIsNullName)),
                    makeABTFunction("isString", makeVariable(std::move(paramName))))),
                makeABTFail(ErrorCodes::Error{7158302},
                            str::stream()
                                << "$replaceOne requires that '" << param << "' be a string")};
        };

        // Order here is important because we want to preserve the precedence of failures in MQL.
        replaceOneExpr = buildABTMultiBranchConditional(
            generateTypeCheckCaseValuePair(inputArgName, inputArgNullName, "input"),
            generateTypeCheckCaseValuePair(findArgName, findArgNullName, "find"),
            generateTypeCheckCaseValuePair(
                replacementArgName, replacementArgNullName, "replacement"),
            ABTCaseValuePair{checkNull, optimizer::Constant::null()},
            std::move(replaceOneExpr));

        replaceOneExpr =
            optimizer::make<optimizer::Let>(std::move(replacementArgNullName),
                                            generateABTNullOrMissing(replacementArgName),
                                            std::move(replaceOneExpr));
        replaceOneExpr = optimizer::make<optimizer::Let>(std::move(findArgNullName),
                                                         generateABTNullOrMissing(findArgName),
                                                         std::move(replaceOneExpr));
        replaceOneExpr = optimizer::make<optimizer::Let>(std::move(inputArgNullName),
                                                         generateABTNullOrMissing(inputArgName),
                                                         std::move(replaceOneExpr));

        replaceOneExpr = optimizer::make<optimizer::Let>(
            std::move(replacementArgName), std::move(replacementArg), std::move(replaceOneExpr));
        replaceOneExpr = optimizer::make<optimizer::Let>(
            std::move(findArgName), std::move(findArg), std::move(replaceOneExpr));
        replaceOneExpr = optimizer::make<optimizer::Let>(
            std::move(inputArgName), std::move(inputArg), std::move(replaceOneExpr));

        pushABT(std::move(replaceOneExpr));
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
            pushABT(makeABTConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Intersection);
    }

    void visit(const ExpressionSetIsSubset* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSetUnion* expr) final {
        if (expr->getChildren().size() == 0) {
            auto [emptySetTag, emptySetValue] = sbe::value::makeNewArraySet();
            pushABT(makeABTConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Union);
    }

    void visit(const ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionReverseArray* expr) final {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto argumentIsNotArray = makeNot(makeABTFunction("isArray", var));

        auto exprReverseArr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(name), optimizer::Constant::null()},
            ABTCaseValuePair{
                std::move(argumentIsNotArray),
                makeABTFail(ErrorCodes::Error{7158002}, "$reverseArray argument must be an array")},
            makeABTFunction("reverseArray", std::move(var)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(name), std::move(arg), std::move(exprReverseArr)));
    }

    void visit(const ExpressionSortArray* expr) final {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto [specTag, specVal] = makeValue(expr->getSortPattern());
        auto specConstant = makeABTConstant(specTag, specVal);

        auto collatorSlot = _context->state.data->env->getSlotIfExists("collator"_sd);
        auto collatorVar = collatorSlot.map(
            [&](auto slotId) { return _context->registerVariable(*collatorSlot); });

        auto argumentIsNotArray = makeNot(makeABTFunction("isArray", var));

        optimizer::ABTVector functionArgs{std::move(var), std::move(specConstant)};
        if (collatorVar) {
            functionArgs.emplace_back(makeVariable(std::move(*collatorVar)));
        }

        auto exprSortArr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(name), optimizer::Constant::null()},
            ABTCaseValuePair{std::move(argumentIsNotArray),
                             makeABTFail(ErrorCodes::Error{7158001},
                                         "$sortArray input argument must be an array")},
            optimizer::make<optimizer::FunctionCall>("sortArray", std::move(functionArgs)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(name), std::move(arg), std::move(exprSortArr)));
    }

    void visit(const ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionIsArray* expr) final {
        pushABT(makeFillEmptyFalse(makeABTFunction("isArray", _context->popABTExpr())));
    }
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionRound* expr) final {
        invariant(expr->getChildren().size() == 1 || expr->getChildren().size() == 2);
        const bool hasPlaceArg = expr->getChildren().size() == 2;
        _context->ensureArity(expr->getChildren().size());

        auto inputNumName = makeLocalVariableName(_context->state.frameId(), 0);
        auto inputPlaceName = makeLocalVariableName(_context->state.frameId(), 0);

        // We always need to validate the number parameter, since it will always exist.
        std::vector<ABTCaseValuePair> inputValidationCases{
            generateABTReturnNullIfNullOrMissing(makeVariable(inputNumName)),
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputNumName),
                makeABTFail(ErrorCodes::Error{5155300}, "$round only supports numeric types")}};
        // Only add these cases if we have a "place" argument.
        if (hasPlaceArg) {
            inputValidationCases.emplace_back(
                generateABTReturnNullIfNullOrMissing(makeVariable(inputPlaceName)));
            inputValidationCases.emplace_back(
                generateInvalidRoundPlaceArgCheck(inputPlaceName),
                makeABTFail(ErrorCodes::Error{5155301},
                            "$round requires \"place\" argument to be "
                            "an integer between -20 and 100"));
        }

        auto roundExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases),
            makeABTFunction("round"_sd, makeVariable(inputNumName), makeVariable(inputPlaceName)));

        // "place" argument defaults to 0.
        auto placeABT = hasPlaceArg ? _context->popABTExpr() : optimizer::Constant::int32(0);
        auto inputABT = _context->popABTExpr();
        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputNumName),
            std::move(inputABT),
            optimizer::make<optimizer::Let>(
                std::move(inputPlaceName), std::move(placeABT), std::move(roundExpr))));
    }
    void visit(const ExpressionSplit* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto [arrayWithEmptyStringTag, arrayWithEmptyStringVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrayWithEmptyStringGuard{arrayWithEmptyStringTag,
                                                         arrayWithEmptyStringVal};
        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        sbe::value::getArrayView(arrayWithEmptyStringVal)->push_back(emptyStrTag, emptyStrVal);

        auto delimiter = _context->popABTExpr();
        auto stringExpression = _context->popABTExpr();

        auto varString = makeLocalVariableName(_context->state.frameId(), 0);
        auto varDelimiter = makeLocalVariableName(_context->state.frameId(), 0);
        auto emptyResult = makeABTConstant(arrayWithEmptyStringTag, arrayWithEmptyStringVal);
        arrayWithEmptyStringGuard.reset();

        // In order to maintain MQL semantics, first check both the string expression
        // (first agument), and delimiter string (second argument) for null, undefined, or
        // missing, and if either is nullish make the entire expression return null. Only
        // then make further validity checks against the input. Fail if the delimiter is an
        // empty string. Return [""] if the string expression is an empty string.
        auto totalSplitFunc = buildABTMultiBranchConditional(
            ABTCaseValuePair{
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                     generateABTNullOrMissing(varString),
                                                     generateABTNullOrMissing(varDelimiter)),
                optimizer::Constant::null()},
            ABTCaseValuePair{makeNot(makeABTFunction("isString"_sd, makeVariable(varString))),
                             makeABTFail(ErrorCodes::Error{7158202},
                                         "$split string expression must be a string")},
            ABTCaseValuePair{
                makeNot(makeABTFunction("isString"_sd, makeVariable(varDelimiter))),
                makeABTFail(ErrorCodes::Error{7158203}, "$split delimiter must be a string")},
            ABTCaseValuePair{optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Eq,
                                                                  makeVariable(varDelimiter),
                                                                  makeABTConstant(""_sd)),
                             makeABTFail(ErrorCodes::Error{7158204},
                                         "$split delimiter must not be an empty string")},
            ABTCaseValuePair{optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Eq,
                                                                  makeVariable(varString),
                                                                  makeABTConstant(""_sd)),
                             std::move(emptyResult)},
            makeABTFunction("split"_sd, makeVariable(varString), makeVariable(varDelimiter)));

        pushABT(optimizer::make<optimizer::Let>(
            varString,
            std::move(stringExpression),
            optimizer::make<optimizer::Let>(
                varDelimiter, std::move(delimiter), std::move(totalSplitFunc))));
    }
    void visit(const ExpressionSqrt* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto sqrtExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(inputName), optimizer::Constant::null()},
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputName),
                makeABTFail(ErrorCodes::Error{7157709}, "$sqrt only supports numeric types")},
            ABTCaseValuePair{generateABTNegativeCheck(inputName),
                             makeABTFail(ErrorCodes::Error{7157710},
                                         "$sqrt's argument must be greater than or equal to 0")},
            makeABTFunction("sqrt", makeVariable(inputName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(sqrtExpr)));
    }
    void visit(const ExpressionStrcasecmp* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSubstrBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSubstrCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionStrLenBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionStrLenCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSubtract* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();

        auto lhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto checkNullArguments =
            optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                 generateABTNullOrMissing(lhsName),
                                                 generateABTNullOrMissing(rhsName));

        auto checkArgumentTypes = makeNot(optimizer::make<optimizer::If>(
            makeABTFunction("isNumber", makeVariable(lhsName)),
            makeABTFunction("isNumber", makeVariable(rhsName)),
            optimizer::make<optimizer::BinaryOp>(
                optimizer::Operations::And,
                makeABTFunction("isDate", makeVariable(lhsName)),
                optimizer::make<optimizer::BinaryOp>(
                    optimizer::Operations::Or,
                    makeABTFunction("isNumber", makeVariable(rhsName)),
                    makeABTFunction("isDate", makeVariable(rhsName))))));

        auto subtractOp = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Sub, makeVariable(lhsName), makeVariable(rhsName));
        auto subtractExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkNullArguments), optimizer::Constant::null()},
            ABTCaseValuePair{
                std::move(checkArgumentTypes),
                makeABTFail(ErrorCodes::Error{7157720},
                            "Only numbers and dates are allowed in an $subtract expression. To "
                            "subtract a number from a date, the date must be the first argument.")},
            std::move(subtractOp));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(lhsName),
            std::move(lhs),
            optimizer::make<optimizer::Let>(
                std::move(rhsName), std::move(rhs), std::move(subtractExpr))));
    }
    void visit(const ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionTestApiVersion* expr) final {
        pushABT(optimizer::Constant::int32(1));
    }
    void visit(const ExpressionToLower* expr) final {
        generateStringCaseConversionExpression(_context, "toLower");
    }
    void visit(const ExpressionToUpper* expr) final {
        generateStringCaseConversionExpression(_context, "toUpper");
    }
    void visit(const ExpressionTrim* expr) final {
        unsupportedExpression("$trim");
    }
    void visit(const ExpressionTrunc* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionType* expr) final {
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
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        unsupportedExpression(expr->getOpName());
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
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        unsupportedExpression(expr->getOpName());
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
        unsupportedExpression(expr->getOpName());
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

    void visit(const ExpressionTsSecond* expr) final {
        _context->ensureArity(1);

        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto tsSecondExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(name), optimizer::Constant::null()},
            ABTCaseValuePair{generateABTNonTimestampCheck(name),
                             makeABTFail(ErrorCodes::Error{7157900},
                                         str::stream() << expr->getOpName()
                                                       << " expects argument of type timestamp")},
            makeABTFunction("tsSecond", makeVariable(name)));
        pushABT(optimizer::make<optimizer::Let>(
            std::move(name), std::move(arg), std::move(tsSecondExpr)));
    }

    void visit(const ExpressionTsIncrement* expr) final {
        _context->ensureArity(1);

        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto tsIncrementExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(name), optimizer::Constant::null()},
            ABTCaseValuePair{generateABTNonTimestampCheck(name),
                             makeABTFail(ErrorCodes::Error{7157901},
                                         str::stream() << expr->getOpName()
                                                       << " expects argument of type timestamp")},
            makeABTFunction("tsIncrement", makeVariable(name)));
        pushABT(optimizer::make<optimizer::Let>(
            std::move(name), std::move(arg), std::move(tsIncrementExpr)));
    }

    void visit(const ExpressionInternalOwningShard* expr) final {
        unsupportedExpression("$_internalOwningShard");
    }

    void visit(const ExpressionInternalIndexKey* expr) final {
        unsupportedExpression("$_internalIndexKey");
    }

private:
    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(const Expression* expr, optimizer::Operations logicOp) {
        invariant(logicOp == optimizer::Operations::And || logicOp == optimizer::Operations::Or);

        size_t numChildren = expr->getChildren().size();
        if (numChildren == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value:
            // true and false, respectively.
            auto logicIdentityVal = (logicOp == optimizer::Operations::And);
            pushABT(optimizer::Constant::boolean(logicIdentityVal));
            return;
        }

        std::vector<optimizer::ABT> exprs;
        exprs.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            exprs.emplace_back(
                makeFillEmptyFalse(makeABTFunction("coerceToBool", _context->popABTExpr())));
        }
        std::reverse(exprs.begin(), exprs.end());

        pushABT(makeBalancedBooleanOpTree(logicOp, std::move(exprs)));
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
            ? _context->popABTExpr()
            : makeABTFail(ErrorCodes::Error{7158303},
                          "$switch could not find a matching branch for an "
                          "input, and no default was specified.");

        size_t numCases = expr->getChildren().size() / 2;
        std::vector<ABTCaseValuePair> cases;
        cases.reserve(numCases);

        for (size_t i = 0; i < numCases; ++i) {
            auto valueExpr = _context->popABTExpr();
            auto conditionExpr =
                makeFillEmptyFalse(makeABTFunction("coerceToBool", _context->popABTExpr()));
            cases.emplace_back(std::move(conditionExpr), std::move(valueExpr));
        }

        std::reverse(cases.begin(), cases.end());

        pushABT(buildABTMultiBranchConditionalFromCaseValuePairs(std::move(cases),
                                                                 std::move(defaultExpr)));
    }

    void generateDateExpressionAcceptingTimeZone(StringData exprName, const Expression* expr) {
        auto children = expr->getChildren();
        invariant(children.size() == 2);

        auto timezoneExpression =
            children[1] ? _context->popABTExpr() : optimizer::Constant::str("UTC"_sd);
        auto dateExpression = _context->popABTExpr();

        // Local bind to hold the date expression result
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateVar = makeVariable(dateName);

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);

        // Set parameters for an invocation of the built-in function.
        optimizer::ABTVector arguments;
        arguments.push_back(dateVar);

        // Create expressions to check that each argument to the function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(dateVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullOrMissing(timezoneExpression));

        // "timezone" parameter validation.
        if (timezoneExpression.is<optimizer::Constant>()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.cast<optimizer::Constant>()->get();
            auto [timezoneDBTag, timezoneDBVal] =
                _context->state.data->env->getAccessor(timeZoneDBSlot)->getViewOfValue();
            auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);
            uassert(5157900,
                    str::stream() << "$" << exprName.toString()
                                  << " parameter 'timezone' must be a string",
                    sbe::value::isString(timezoneTag));
            uassert(5157901,
                    str::stream() << "$" << exprName.toString()
                                  << " parameter 'timezone' must be a valid timezone",
                    sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
            auto [timezoneObjTag, timezoneObjVal] = sbe::value::makeCopyTimeZone(
                sbe::vm::getTimezone(timezoneTag, timezoneVal, timezoneDB));
            auto timezoneConst =
                optimizer::make<optimizer::Constant>(timezoneObjTag, timezoneObjVal);
            arguments.push_back(std::move(timezoneConst));
        } else {
            auto timeZoneDBName = _context->registerVariable(timeZoneDBSlot);
            auto timeZoneDBVar = makeVariable(timeZoneDBName);
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(timezoneExpression),
                makeABTFail(ErrorCodes::Error{5157902},
                            str::stream() << "$" << exprName.toString()
                                          << " parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneExpression)),
                makeABTFail(ErrorCodes::Error{5157903},
                            str::stream() << "$" << exprName.toString()
                                          << " parameter 'timezone' must be a valid timezone"));
            arguments.push_back(std::move(timeZoneDBVar));
            arguments.push_back(std::move(timezoneExpression));
        }

        // Create an expression to invoke the built-in function.
        auto funcCall =
            optimizer::make<optimizer::FunctionCall>(exprName.toString(), std::move(arguments));
        auto funcName = makeLocalVariableName(_context->state.frameId(), 0);
        auto funcVar = makeVariable(funcName);


        // "date" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            std::move(dateVar), ErrorCodes::Error{5157904}, std::move(exprName), "date"_sd));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(dateName),
            std::move(dateExpression),
            optimizer::make<optimizer::Let>(
                std::move(funcName),
                std::move(funcCall),
                optimizer::make<optimizer::If>(
                    makeABTFunction("exists", funcVar),
                    std::move(funcVar),
                    buildABTMultiBranchConditionalFromCaseValuePairs(
                        std::move(inputValidationCases), optimizer::Constant::nothing())))));
    }

    /**
     * Creates a CaseValuePair such that an exception is thrown if a value of the parameter denoted
     * by variable 'dateRef' is of a type that is not coercible to a date.
     *
     * dateRef - a variable corresponding to the parameter.
     * errorCode - error code of the type mismatch error.
     * expressionName - a name of an expression the parameter belongs to.
     * parameterName - a name of the parameter corresponding to variable 'dateRef'.
     */
    static CaseValuePair generateFailIfNotCoercibleToDate(const sbe::EVariable& dateRef,
                                                          ErrorCodes::Error errorCode,
                                                          StringData expressionName,
                                                          StringData parameterName) {
        return {
            makeNot(makeFunction("typeMatch",
                                 dateRef.clone(),
                                 makeConstant(sbe::value::TypeTags::NumberInt64,
                                              sbe::value::bitcastFrom<int64_t>(dateTypeMask())))),
            sbe::makeE<sbe::EFail>(errorCode,
                                   str::stream()
                                       << expressionName << " parameter '" << parameterName
                                       << "' must be coercible to date")};
    }

    static ABTCaseValuePair generateABTFailIfNotCoercibleToDate(const optimizer::ABT& dateVar,
                                                                ErrorCodes::Error errorCode,
                                                                StringData expressionName,
                                                                StringData parameterName) {
        return {makeNot(makeABTFunction(
                    "typeMatch", dateVar, optimizer::Constant::int32(dateTypeMask()))),
                makeABTFail(errorCode,
                            str::stream() << expressionName << " parameter '" << parameterName
                                          << "' must be coercible to date")};
    }

    /**
     * Creates a CaseValuePair such that Null value is returned if a value of variable denoted by
     * 'variable' is null or missing.
     */
    static CaseValuePair generateReturnNullIfNullOrMissing(const sbe::EVariable& variable) {
        return {generateNullOrMissing(variable), makeConstant(sbe::value::TypeTags::Null, 0)};
    }

    static ABTCaseValuePair generateABTReturnNullIfNullOrMissing(const optimizer::ABT& name) {
        return {generateABTNullOrMissing(name), optimizer::Constant::null()};
    }

    static CaseValuePair generateReturnNullIfNullOrMissing(std::unique_ptr<sbe::EExpression> expr) {
        return {generateNullOrMissing(std::move(expr)),
                makeConstant(sbe::value::TypeTags::Null, 0)};
    }

    /**
     * Creates a boolean expression to check if 'variable' is equal to string 'string'.
     */
    static std::unique_ptr<sbe::EExpression> generateIsEqualToStringCheck(
        const sbe::EExpression& expr, StringData string) {
        return sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicAnd,
            makeFunction("isString", expr.clone()),
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::eq, expr.clone(), makeConstant(string)));
    }

    static optimizer::ABT generateABTIsEqualToStringCheck(const optimizer::ABT& expr,
                                                          StringData string) {
        return optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::And,
            makeABTFunction("isString", expr),
            optimizer::make<optimizer::BinaryOp>(
                optimizer::Operations::Eq, expr, optimizer::Constant::str(string)));
    }

    /**
     * Shared expression building logic for trignometric expressions to make sure the operand
     * is numeric and is not null.
     */
    void generateTrigonometricExpression(StringData exprName) {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(frameId, 0);

        auto genericTrigonometricExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(argName), optimizer::Constant::null()},
            ABTCaseValuePair{makeABTFunction("isNumber", makeVariable(argName)),
                             makeABTFunction(exprName, makeVariable(argName))},
            makeABTFail(ErrorCodes::Error{7157800},
                        str::stream()
                            << "$" << exprName.toString() << " supports only numeric types"));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(argName), std::move(arg), std::move(genericTrigonometricExpr)));
    }

    /**
     * Shared expression building logic for binary trigonometric expressions to make sure the
     * operands are numeric and are not null.
     */
    void generateTrigonometricExpressionBinary(StringData exprName) {
        _context->ensureArity(2);
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();
        auto lhsName = makeLocalVariableName(_context->state.frameId(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameId(), 0);
        auto lhsVariable = makeVariable(lhsName);
        auto rhsVariable = makeVariable(rhsName);

        auto checkNullOrMissing =
            optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Or,
                                                 generateABTNullOrMissing(lhsName),
                                                 generateABTNullOrMissing(rhsName));

        auto checkIsNumber =
            optimizer::make<optimizer::BinaryOp>(optimizer::Operations::And,
                                                 makeABTFunction("isNumber", lhsVariable),
                                                 makeABTFunction("isNumber", rhsVariable));

        auto genericTrigonometricExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkNullOrMissing), optimizer::Constant::null()},
            ABTCaseValuePair{
                std::move(checkIsNumber),
                makeABTFunction(exprName, std::move(lhsVariable), std::move(rhsVariable))},
            makeABTFail(ErrorCodes::Error{7157801},
                        str::stream() << "$" << exprName << " supports only numeric types"));


        pushABT(optimizer::make<optimizer::Let>(
            std::move(lhsName),
            std::move(lhs),
            optimizer::make<optimizer::Let>(
                std::move(rhsName), std::move(rhs), std::move(genericTrigonometricExpr))));
    }

    /**
     * Shared expression building logic for trignometric expressions with bounds for the valid
     * values of the argument.
     */
    void generateTrigonometricExpressionWithBounds(StringData exprName,
                                                   const DoubleBound& lowerBound,
                                                   const DoubleBound& upperBound) {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(frameId, 0);
        auto variable = makeVariable(argName);
        optimizer::Operations lowerCmp =
            lowerBound.inclusive ? optimizer::Operations::Gte : optimizer::Operations::Gt;
        optimizer::Operations upperCmp =
            upperBound.inclusive ? optimizer::Operations::Lte : optimizer::Operations::Lt;
        auto checkBounds = optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::And,
            optimizer::make<optimizer::BinaryOp>(
                lowerCmp, variable, optimizer::Constant::fromDouble(lowerBound.bound)),
            optimizer::make<optimizer::BinaryOp>(
                upperCmp, variable, optimizer::Constant::fromDouble(upperBound.bound)));

        auto checkIsNumber = makeABTFunction("isNumber", variable);
        auto trigonometricExpr = makeABTFunction(exprName, variable);

        auto genericTrigonometricExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(argName), optimizer::Constant::null()},
            ABTCaseValuePair{makeNot(std::move(checkIsNumber)),
                             makeABTFail(ErrorCodes::Error{7157802},
                                         str::stream() << "$" << exprName.toString()
                                                       << " supports only numeric types")},
            ABTCaseValuePair{generateABTNaNCheck(argName), std::move(variable)},
            ABTCaseValuePair{std::move(checkBounds), std::move(trigonometricExpr)},
            makeABTFail(ErrorCodes::Error{7157803},
                        str::stream() << "Cannot apply $" << exprName.toString()
                                      << ", value must be in " << lowerBound.printLowerBound()
                                      << ", " << upperBound.printUpperBound()));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(argName), std::move(arg), std::move(genericTrigonometricExpr)));
    }

    /*
     * Generates an EExpression that returns an index for $indexOfBytes or $indexOfCP.
     */
    void visitIndexOfFunction(const Expression* expr,
                              ExpressionVisitorContext* _context,
                              const std::string& indexOfFunction) {
        auto children = expr->getChildren();
        auto operandSize = children.size() <= 3 ? 3 : 4;
        optimizer::ABTVector operands;
        operands.reserve(operandSize);

        auto strName = makeLocalVariableName(_context->state.frameId(), 0);
        auto substrName = makeLocalVariableName(_context->state.frameId(), 0);
        boost::optional<optimizer::ProjectionName> startIndexName;
        boost::optional<optimizer::ProjectionName> endIndexName;

        // Get arguments from stack.
        switch (children.size()) {
            case 2: {
                operands.emplace_back(optimizer::Constant::int64(0));
                operands.emplace_back(_context->popABTExpr());
                operands.emplace_back(_context->popABTExpr());
                startIndexName.emplace(makeLocalVariableName(_context->state.frameId(), 0));
                break;
            }
            case 3: {
                operands.emplace_back(_context->popABTExpr());
                operands.emplace_back(_context->popABTExpr());
                operands.emplace_back(_context->popABTExpr());
                startIndexName.emplace(makeLocalVariableName(_context->state.frameId(), 0));
                break;
            }
            case 4: {
                operands.emplace_back(_context->popABTExpr());
                operands.emplace_back(_context->popABTExpr());
                operands.emplace_back(_context->popABTExpr());
                operands.emplace_back(_context->popABTExpr());
                startIndexName.emplace(makeLocalVariableName(_context->state.frameId(), 0));
                endIndexName.emplace(makeLocalVariableName(_context->state.frameId(), 0));
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        // Add string and substring operands.
        optimizer::ABTVector functionArgs{makeVariable(strName), makeVariable(substrName)};

        // Add start index operand.
        if (startIndexName) {
            auto numericConvert64 = makeABTFunction("convert",
                                                    makeVariable(*startIndexName),
                                                    optimizer::Constant::int32(static_cast<int32_t>(
                                                        sbe::value::TypeTags::NumberInt64)));
            auto checkValidStartIndex = buildABTMultiBranchConditional(
                ABTCaseValuePair{generateABTNullishOrNotRepresentableInt32Check(*startIndexName),
                                 makeABTFail(ErrorCodes::Error{7158003},
                                             str::stream()
                                                 << "$" << indexOfFunction
                                                 << " start index must resolve to a number")},
                ABTCaseValuePair{generateABTNegativeCheck(*startIndexName),
                                 makeABTFail(ErrorCodes::Error{7158004},
                                             str::stream() << "$" << indexOfFunction
                                                           << " start index must be positive")},
                std::move(numericConvert64));
            functionArgs.push_back(std::move(checkValidStartIndex));
        }

        // Add end index operand.
        if (endIndexName) {
            auto numericConvert64 = makeABTFunction("convert",
                                                    makeVariable(*endIndexName),
                                                    optimizer::Constant::int32(static_cast<int32_t>(
                                                        sbe::value::TypeTags::NumberInt64)));
            auto checkValidEndIndex = buildABTMultiBranchConditional(
                ABTCaseValuePair{generateABTNullishOrNotRepresentableInt32Check(*endIndexName),
                                 makeABTFail(ErrorCodes::Error{7158005},
                                             str::stream()
                                                 << "$" << indexOfFunction
                                                 << " end index must resolve to a number")},
                ABTCaseValuePair{generateABTNegativeCheck(*endIndexName),
                                 makeABTFail(ErrorCodes::Error{7158006},
                                             str::stream() << "$" << indexOfFunction
                                                           << " end index must be positive")},
                std::move(numericConvert64));
            functionArgs.push_back(std::move(checkValidEndIndex));
        }

        // Check if string or substring are null or missing before calling indexOfFunction.
        auto resultExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{generateABTNullOrMissing(strName), optimizer::Constant::null()},
            ABTCaseValuePair{generateABTNonStringCheck(strName),
                             makeABTFail(ErrorCodes::Error{7158007},
                                         str::stream()
                                             << "$" << indexOfFunction
                                             << " string must resolve to a string or null")},
            ABTCaseValuePair{generateABTNullOrMissing(substrName),
                             makeABTFail(ErrorCodes::Error{7158008},
                                         str::stream() << "$" << indexOfFunction
                                                       << " substring must resolve to a string")},
            ABTCaseValuePair{generateABTNonStringCheck(substrName),
                             makeABTFail(ErrorCodes::Error{7158009},
                                         str::stream() << "$" << indexOfFunction
                                                       << " substring must resolve to a string")},
            optimizer::make<optimizer::FunctionCall>(indexOfFunction, std::move(functionArgs)));

        // Build local binding tree.
        int operandIdx = 0;
        if (endIndexName) {
            resultExpr = optimizer::make<optimizer::Let>(
                *endIndexName, std::move(operands[operandIdx++]), std::move(resultExpr));
        }
        if (startIndexName) {
            resultExpr = optimizer::make<optimizer::Let>(
                *startIndexName, std::move(operands[operandIdx++]), std::move(resultExpr));
        }
        resultExpr = optimizer::make<optimizer::Let>(
            std::move(substrName), std::move(operands[operandIdx++]), std::move(resultExpr));
        resultExpr = optimizer::make<optimizer::Let>(
            std::move(strName), std::move(operands[operandIdx++]), std::move(resultExpr));

        pushABT(std::move(resultExpr));
    }

    /**
     * Generic logic for building set expressions: setUnion, setIntersection, etc.
     */
    void generateSetExpression(const Expression* expr, SetOperation setOp) {
        using namespace std::literals;

        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        optimizer::ABTVector args;
        optimizer::ProjectionNameVector argNames;
        optimizer::ABTVector variables;

        optimizer::ABTVector checkNulls;
        optimizer::ABTVector checkNotArrays;

        auto collatorSlot = _context->state.data->env->getSlotIfExists("collator"_sd);

        args.reserve(arity);
        argNames.reserve(arity);
        variables.reserve(arity + (collatorSlot.has_value() ? 1 : 0));
        checkNulls.reserve(arity);
        checkNotArrays.reserve(arity);

        auto [operatorName, setFunctionName] =
            getSetOperatorAndFunctionNames(setOp, collatorSlot.has_value());
        if (collatorSlot) {
            variables.push_back(makeVariable(_context->registerVariable(*collatorSlot)));
        }

        for (size_t idx = 0; idx < arity; ++idx) {
            args.push_back(_context->popABTExpr());
            auto argName = makeLocalVariableName(_context->state.frameId(), 0);
            argNames.push_back(argName);
            variables.push_back(makeVariable(argName));

            checkNulls.push_back(generateABTNullOrMissing(argName));
            checkNotArrays.push_back(generateABTNonArrayCheck(std::move(argName)));
        }
        // Reverse the args array to preserve the original order of the arguments, since some set
        // operations, such as $setDifference, are not commutative.
        std::reverse(std::begin(args), std::end(args));

        auto checkNullAnyArgument =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(checkNulls));
        auto checkNotArrayAnyArgument =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(checkNotArrays));
        auto setExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkNullAnyArgument), optimizer::Constant::null()},
            ABTCaseValuePair{std::move(checkNotArrayAnyArgument),
                             makeABTFail(ErrorCodes::Error{7158100},
                                         str::stream() << "All operands of $" << operatorName
                                                       << " must be arrays.")},
            optimizer::make<optimizer::FunctionCall>(setFunctionName.toString(),
                                                     std::move(variables)));

        for (size_t i = 0; i < arity; ++i) {
            setExpr = optimizer::make<optimizer::Let>(
                std::move(argNames[i]), std::move(args[i]), setExpr);
        }

        pushABT(std::move(setExpr));
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
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Shared expression building logic for regex expressions.
     */
    void generateRegexExpression(const ExpressionRegex* expr, StringData exprName) {
        size_t arity = expr->hasOptions() ? 3 : 2;
        _context->ensureArity(arity);

        boost::optional<optimizer::ABT> options;
        if (expr->hasOptions()) {
            options = _context->popABTExpr();
        }
        auto pattern = _context->popABTExpr();
        auto input = _context->popABTExpr();

        auto inputVar = makeLocalVariableName(_context->state.frameId(), 0);
        auto patternVar = makeLocalVariableName(_context->state.frameId(), 0);

        auto generateRegexNullResponse = [exprName]() {
            if (exprName == "regexMatch"_sd) {
                return optimizer::Constant::boolean(false);
            } else if (exprName == "regexFindAll"_sd) {
                return optimizer::Constant::emptyArray();
            } else {
                return optimizer::Constant::null();
            }
        };

        auto makeError = [exprName](int errorCode, StringData message) {
            return makeABTFail(ErrorCodes::Error{errorCode},
                               str::stream() << "$" << exprName.toString() << ": " << message);
        };

        auto makeRegexFunctionCall = [&](optimizer::ABT compiledRegex) {
            auto resultVar = makeLocalVariableName(_context->state.frameId(), 0);
            return optimizer::make<optimizer::Let>(
                resultVar,
                makeABTFunction(exprName, std::move(compiledRegex), makeVariable(inputVar)),
                optimizer::make<optimizer::If>(
                    makeABTFunction("exists"_sd, makeVariable(resultVar)),
                    makeVariable(resultVar),
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
                auto [regexTag, regexVal] = sbe::value::makeNewPcreRegex(*pattern, options);
                auto compiledRegex = makeABTConstant(regexTag, regexVal);
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
            auto patternArgument = optimizer::make<optimizer::If>(
                makeABTFunction("isString"_sd, makeVariable(patternVar)),
                optimizer::make<optimizer::If>(
                    makeABTFunction("hasNullBytes"_sd, makeVariable(patternVar)),
                    makeError(5126602, "regex pattern must not have embedded null bytes"),
                    makeVariable(patternVar)),
                optimizer::make<optimizer::If>(
                    makeABTFunction("typeMatch"_sd,
                                    makeVariable(patternVar),
                                    optimizer::Constant::int32(getBSONTypeMask(BSONType::RegEx))),
                    makeABTFunction("getRegexPattern"_sd, makeVariable(patternVar)),
                    makeError(5126601,
                              "regex pattern must have either string or BSON RegEx type")));

            if (!options) {
                // If no options are passed to the expression, try to extract them from the
                // pattern.
                auto optionsArgument = optimizer::make<optimizer::If>(
                    makeABTFunction("typeMatch"_sd,
                                    makeVariable(patternVar),
                                    optimizer::Constant::int32(getBSONTypeMask(BSONType::RegEx))),
                    makeABTFunction("getRegexFlags"_sd, makeVariable(patternVar)),
                    makeABTConstant(""_sd));
                auto compiledRegex = makeABTFunction(
                    "regexCompile"_sd, std::move(patternArgument), std::move(optionsArgument));
                return optimizer::make<optimizer::If>(
                    makeABTFunction("isNull"_sd, makeVariable(patternVar)),
                    generateRegexNullResponse(),
                    makeRegexFunctionCall(std::move(compiledRegex)));
            }

            // If there are options passed to the expression, we construct local bind with
            // options argument because it needs to be validated even when pattern is null.
            auto userOptionsVar = makeLocalVariableName(_context->state.frameId(), 0);
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
                auto stringOptions = optimizer::make<optimizer::If>(
                    makeABTFunction("isString"_sd, makeVariable(userOptionsVar)),
                    optimizer::make<optimizer::If>(
                        makeABTFunction("hasNullBytes"_sd, makeVariable(userOptionsVar)),
                        makeError(5126604, "regex flags must not have embedded null bytes"),
                        makeVariable(userOptionsVar)),
                    optimizer::make<optimizer::If>(
                        makeABTFunction("isNull"_sd, makeVariable(userOptionsVar)),
                        makeABTConstant(""_sd),
                        makeError(5126603, "regex flags must have either string or null type")));

                auto generateIsEmptyString = [](const optimizer::ProjectionName& var) {
                    return optimizer::make<optimizer::BinaryOp>(
                        optimizer::Operations::Eq, makeVariable(var), makeABTConstant(""_sd));
                };

                auto stringVar = makeLocalVariableName(_context->state.frameId(), 0);
                auto bsonPatternVar = makeLocalVariableName(_context->state.frameId(), 0);
                return optimizer::make<optimizer::Let>(
                    stringVar,
                    std::move(stringOptions),
                    optimizer::make<optimizer::If>(
                        makeABTFunction(
                            "typeMatch"_sd,
                            makeVariable(patternVar),
                            optimizer::Constant::int32(getBSONTypeMask(BSONType::RegEx))),
                        optimizer::make<optimizer::Let>(
                            bsonPatternVar,
                            makeABTFunction("getRegexFlags", makeVariable(patternVar)),
                            optimizer::make<optimizer::If>(
                                generateIsEmptyString(stringVar),
                                makeVariable(bsonPatternVar),
                                optimizer::make<optimizer::If>(
                                    generateIsEmptyString(bsonPatternVar),
                                    makeVariable(stringVar),
                                    makeError(5126605,
                                              "regex options cannot be specified in both BSON "
                                              "RegEx and 'options' field")))),
                        makeVariable(stringVar)));
            }();

            auto optionsVar = makeLocalVariableName(_context->state.frameId(), 0);
            return optimizer::make<optimizer::Let>(
                userOptionsVar,
                std::move(*options),
                optimizer::make<optimizer::Let>(
                    optionsVar,
                    std::move(optionsArgument),
                    optimizer::make<optimizer::If>(
                        makeABTFunction("isNull"_sd, makeVariable(patternVar)),
                        generateRegexNullResponse(),
                        makeRegexFunctionCall(makeABTFunction("regexCompile"_sd,
                                                              makeVariable(patternVar),
                                                              makeVariable(optionsVar))))));
        }();

        auto regexCall = optimizer::make<optimizer::If>(
            generateABTNullOrMissing(inputVar),
            generateRegexNullResponse(),
            optimizer::make<optimizer::If>(
                makeNot(makeABTFunction("isString"_sd, makeVariable(inputVar))),
                makeError(5073401, "input must be of type string"),
                regexFunctionResult));

        pushABT(optimizer::make<optimizer::Let>(
            inputVar,
            std::move(input),
            optimizer::make<optimizer::Let>(patternVar, std::move(pattern), std::move(regexCall))));
    }

    /**
     * Generic logic for building $dateAdd and $dateSubtract expressions.
     */
    void generateDateArithmeticsExpression(const ExpressionDateArithmetics* expr,
                                           const std::string& dateExprName) {
        auto children = expr->getChildren();
        auto arity = children.size();
        invariant(arity == 4);
        auto timezoneExpr =
            children[3] ? _context->popABTExpr() : optimizer::Constant::str("UTC"_sd);
        auto amountExpr = _context->popABTExpr();
        auto unitExpr = _context->popABTExpr();
        auto startDateExpr = _context->popABTExpr();

        auto startDateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitName = makeLocalVariableName(_context->state.frameId(), 0);
        auto origAmountName = makeLocalVariableName(_context->state.frameId(), 0);
        auto tzName = makeLocalVariableName(_context->state.frameId(), 0);
        auto amountName = makeLocalVariableName(_context->state.frameId(), 0);

        auto convertedAmountInt64 = [&]() {
            if (dateExprName == "dateAdd") {
                return makeABTFunction("convert",
                                       makeVariable(origAmountName),
                                       optimizer::Constant::int32(static_cast<int32_t>(
                                           sbe::value::TypeTags::NumberInt64)));
            } else if (dateExprName == "dateSubtract") {
                return makeABTFunction(
                    "convert",
                    optimizer::make<optimizer::UnaryOp>(optimizer::Operations::Neg,
                                                        makeVariable(origAmountName)),
                    optimizer::Constant::int32(
                        static_cast<int32_t>(sbe::value::TypeTags::NumberInt64)));
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto timeZoneDBVar = makeVariable(_context->registerVariable(timeZoneDBSlot));

        optimizer::ABTVector checkNullArg;
        checkNullArg.push_back(generateABTNullOrMissing(startDateName));
        checkNullArg.push_back(generateABTNullOrMissing(unitName));
        checkNullArg.push_back(generateABTNullOrMissing(origAmountName));
        checkNullArg.push_back(generateABTNullOrMissing(tzName));

        auto checkNullAnyArgument =
            makeBalancedBooleanOpTree(optimizer::Operations::Or, std::move(checkNullArg));

        auto dateAddExpr = buildABTMultiBranchConditional(
            ABTCaseValuePair{std::move(checkNullAnyArgument), optimizer::Constant::null()},
            ABTCaseValuePair{generateABTNonStringCheck(tzName),
                             makeABTFail(ErrorCodes::Error{7157902},
                                         str::stream()
                                             << "$" << dateExprName
                                             << " expects timezone argument of type string")},
            ABTCaseValuePair{
                makeNot(makeABTFunction("isTimezone", timeZoneDBVar, makeVariable(tzName))),
                makeABTFail(ErrorCodes::Error{7157903},
                            str::stream() << "$" << dateExprName << " expects a valid timezone")},
            ABTCaseValuePair{
                makeNot(makeABTFunction("typeMatch",
                                        makeVariable(startDateName),
                                        optimizer::Constant::int32(dateTypeMask()))),
                makeABTFail(ErrorCodes::Error{7157904},
                            str::stream() << "$" << dateExprName
                                          << " must have startDate argument convertable to date")},
            ABTCaseValuePair{generateABTNonStringCheck(unitName),
                             makeABTFail(ErrorCodes::Error{7157905},
                                         str::stream() << "$" << dateExprName
                                                       << " expects unit argument of type string")},
            ABTCaseValuePair{
                makeNot(makeABTFunction("isTimeUnit", makeVariable(unitName))),
                makeABTFail(ErrorCodes::Error{7157906},
                            str::stream() << "$" << dateExprName << " expects a valid time unit")},
            ABTCaseValuePair{makeNot(makeABTFunction("exists", makeVariable(amountName))),
                             makeABTFail(ErrorCodes::Error{7157907},
                                         str::stream() << "invalid $" << dateExprName
                                                       << " 'amount' argument value")},
            makeABTFunction("dateAdd",
                            timeZoneDBVar,
                            makeVariable(startDateName),
                            makeVariable(unitName),
                            makeVariable(amountName),
                            makeVariable(tzName)));

        pushABT(optimizer::make<optimizer::Let>(
            std::move(startDateName),
            std::move(startDateExpr),
            optimizer::make<optimizer::Let>(
                std::move(unitName),
                std::move(unitExpr),
                optimizer::make<optimizer::Let>(
                    std::move(origAmountName),
                    std::move(amountExpr),
                    optimizer::make<optimizer::Let>(
                        std::move(tzName),
                        std::move(timezoneExpr),
                        optimizer::make<optimizer::Let>(std::move(amountName),
                                                        std::move(convertedAmountInt64),
                                                        std::move(dateAddExpr)))))));
    }


    void unsupportedExpression(const char* op) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(5182300, str::stream() << "Unsupported expression in SBE stage builder: " << op);
    }

    ExpressionVisitorContext* _context;

private:
    void pushABT(optimizer::ABT abt) {
        _context->pushExpr(abt::wrap(std::move(abt)));
    }
};
}  // namespace

EvalExpr generateExpression(StageBuilderState& state,
                            const Expression* expr,
                            boost::optional<sbe::value::SlotId> rootSlot,
                            const PlanStageSlots* slots) {
    ExpressionVisitorContext context(state, std::move(rootSlot), slots);

    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk<const Expression>(expr, &walker);

    return context.done();
}
}  // namespace mongo::stage_builder
