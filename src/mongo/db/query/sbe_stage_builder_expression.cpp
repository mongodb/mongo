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

#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/util/make_data_structure.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/util/str.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace mongo::stage_builder {
namespace {
struct ExpressionVisitorContext {
    struct VarsFrame {
        std::deque<Variables::Id> variablesToBind;

        // Slots that have been used to bind $let variables. This list is necessary to know which
        // slots to remove from the environment when the $let goes out of scope.
        std::set<sbe::value::SlotId> slotsForLetVariables;

        template <class... Args>
        VarsFrame(Args&&... args)
            : variablesToBind{std::forward<Args>(args)...}, slotsForLetVariables{} {}
    };

    ExpressionVisitorContext(StageBuilderState& state,
                             EvalStage inputStage,
                             boost::optional<sbe::value::SlotId> optionalRootSlot,
                             PlanNodeId planNodeId)
        : state(state), optionalRootSlot(optionalRootSlot), planNodeId(planNodeId) {
        evalStack.emplaceFrame(std::move(inputStage));
    }

    void ensureArity(size_t arity) {
        invariant(evalStack.topFrame().exprsCount() >= arity);
    }

    EvalStage extractCurrentEvalStage() {
        return evalStack.topFrame().extractStage();
    }

    void setCurrentStage(EvalStage stage) {
        evalStack.topFrame().setStage(std::move(stage));
    }

    std::unique_ptr<sbe::EExpression> popExpr() {
        return evalStack.topFrame().popExpr().extractExpr();
    }

    EvalExpr popEvalExpr() {
        return evalStack.topFrame().popExpr();
    }

    void pushExpr(EvalExpr expr) {
        evalStack.topFrame().pushExpr(std::move(expr));
    }

    void pushExpr(EvalExpr expr, EvalStage stage) {
        pushExpr(std::move(expr));
        evalStack.topFrame().setStage(std::move(stage));
    }

    EvalExprStagePair popFrame() {
        return evalStack.popFrame();
    }

    sbe::value::SlotVector getLexicalEnvironment() {
        sbe::value::SlotVector lexicalEnvironment;
        for (const auto& [_, slot] : environment) {
            lexicalEnvironment.push_back(slot);
        }
        return lexicalEnvironment;
    }

    EvalExprStagePair done() {
        invariant(evalStack.framesCount() == 1);
        auto [expr, stage] = popFrame();
        return {std::move(expr), stageOrLimitCoScan(std::move(stage), planNodeId)};
    }

    StageBuilderState& state;

    EvalStack<> evalStack;

    boost::optional<sbe::value::SlotId> optionalRootSlot;

    // The lexical environment for the expression being traversed. A variable reference takes the
    // form "$$variable_name" in MQL's concrete syntax and gets transformed into a numeric
    // identifier (Variables::Id) in the AST. During this translation, we directly translate any
    // such variable to an SBE slot using this mapping.
    std::map<Variables::Id, sbe::value::SlotId> environment;
    std::stack<VarsFrame> varsFrameStack;
    // The id of the QuerySolutionNode to which the expression we are converting to SBE is attached.
    const PlanNodeId planNodeId;
    // This stack contains slot id for the current element variable of $filter expression.
    std::stack<sbe::value::SlotId> filterExprSlotIdStack;
    // We use this counter to track which children of $filter we've already processed.
    std::stack<int> filterExprChildrenCounter;
};

std::unique_ptr<sbe::EExpression> generateTraverseHelper(
    const sbe::EVariable& inputVar,
    const FieldPath& fp,
    size_t level,
    sbe::value::FrameIdGenerator* frameIdGenerator) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldName = sbe::makeE<sbe::EConstant>(fp.getFieldName(level));
    auto fieldExpr = makeFunction("getField"_sd, inputVar.clone(), std::move(fieldName));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse stage.
        return fieldExpr;
    }

    // Generate nested traversal.
    auto lambdaFrameId = frameIdGenerator->generate();
    auto lambdaParam = sbe::EVariable{lambdaFrameId, 0};

    auto resultExpr = generateTraverseHelper(lambdaParam, fp, level + 1, frameIdGenerator);

    auto lambdaExpr = sbe::makeE<sbe::ELocalLambda>(lambdaFrameId, std::move(resultExpr));

    // Generate the traverse stage for the current nested level.
    return makeFunction("traverseP",
                        std::move(fieldExpr),
                        std::move(lambdaExpr),
                        makeConstant(sbe::value::TypeTags::NumberInt32, 1));
}

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
std::unique_ptr<sbe::EExpression> generateTraverse(const sbe::EVariable& inputVar,
                                                   bool expectsDocumentInputOnly,
                                                   const FieldPath& fp,
                                                   sbe::value::FrameIdGenerator* frameIdGenerator) {
    size_t level = 0;

    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputVar' will be a document and _not_ an array (such as
        // when traversing the root document), we can generate a simpler expression.
        return generateTraverseHelper(inputVar, fp, level, frameIdGenerator);
    } else {
        // The general case: the value in the 'inputVar' may be an array that will require
        // traversal.
        auto lambdaFrameId = frameIdGenerator->generate();
        auto lambdaParam = sbe::EVariable{lambdaFrameId, 0};

        auto resultExpr = generateTraverseHelper(lambdaParam, fp, level, frameIdGenerator);

        auto lambdaExpr = sbe::makeE<sbe::ELocalLambda>(lambdaFrameId, std::move(resultExpr));

        return makeFunction("traverseP",
                            inputVar.clone(),
                            std::move(lambdaExpr),
                            makeConstant(sbe::value::TypeTags::NumberInt32, 1));
    }
}

/**
 * Generates an EExpression that converts the input to upper or lower case.
 */
void generateStringCaseConversionExpression(ExpressionVisitorContext* _context,
                                            const std::string& caseConversionFunction) {
    auto frameId = _context->state.frameId();
    auto str = sbe::makeEs(_context->popExpr());
    sbe::EVariable inputRef(frameId, 0);
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
    auto checkValidTypeExpr =
        makeFunction("typeMatch",
                     inputRef.clone(),
                     makeConstant(sbe::value::TypeTags::NumberInt64,
                                  sbe::value::bitcastFrom<int64_t>(typeMask)));
    auto checkNullorMissing = generateNullOrMissing(inputRef);
    auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");

    auto caseConversionExpr = sbe::makeE<sbe::EIf>(
        std::move(checkValidTypeExpr),
        makeFunction(caseConversionFunction, makeFunction("coerceToString", inputRef.clone())),
        sbe::makeE<sbe::EFail>(ErrorCodes::Error{5066300},
                               str::stream() << "$" << caseConversionFunction
                                             << " input type is not supported"));

    auto totalCaseConversionExpr =
        sbe::makeE<sbe::EIf>(std::move(checkNullorMissing),
                             sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                             std::move(caseConversionExpr));
    _context->pushExpr(
        sbe::makeE<sbe::ELocalBind>(frameId, std::move(str), std::move(totalCaseConversionExpr)));
}

void buildArrayAccessByConstantIndex(ExpressionVisitorContext* context,
                                     const std::string& exprName,
                                     int32_t index) {
    context->ensureArity(1);

    // It's important that we project the array to a slot here. If we didn't do this, then the
    // view of the array element could potentially outlive the array itself (which could result
    // in use-after-free bugs).
    auto [arraySlot, stage] = projectEvalExpr(context->popEvalExpr(),
                                              context->extractCurrentEvalStage(),
                                              context->planNodeId,
                                              context->state.slotIdGenerator);
    auto array = makeVariable(arraySlot);
    auto frameId = context->state.frameId();
    auto binds = sbe::makeEs(std::move(array));
    sbe::EVariable arrayRef{frameId, 0};

    auto indexExpr = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                sbe::value::bitcastFrom<int32_t>(index));
    auto argumentIsNotArray = makeNot(makeFunction("isArray", arrayRef.clone()));
    auto resultExpr = buildMultiBranchConditional(
        CaseValuePair{generateNullOrMissing(arrayRef),
                      sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
        CaseValuePair{std::move(argumentIsNotArray),
                      sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126704},
                                             exprName + " argument must be an array")},
        makeFunction("getElement", arrayRef.clone(), std::move(indexExpr)));

    context->pushExpr(sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(resultExpr)),
                      std::move(stage));
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
    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(const ExpressionAnyElementTrue* expr) final {}
    void visit(const ExpressionArray* expr) final {}
    void visit(const ExpressionArrayElemAt* expr) final {}
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
    void visit(const ExpressionCond* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(const ExpressionDateDiff* expr) final {}
    void visit(const ExpressionDateFromString* expr) final {}
    void visit(const ExpressionDateFromParts* expr) final {}
    void visit(const ExpressionDateToParts* expr) final {}
    void visit(const ExpressionDateToString* expr) final {}
    void visit(const ExpressionDateTrunc* expr) final {}
    void visit(const ExpressionDivide* expr) final {}
    void visit(const ExpressionEncryptedBetween* expr) final {}
    void visit(const ExpressionExp* expr) final {}
    void visit(const ExpressionFieldPath* expr) final {}
    void visit(const ExpressionFilter* expr) final {
        _context->filterExprChildrenCounter.push(1);
    }
    void visit(const ExpressionFloor* expr) final {}
    void visit(const ExpressionIfNull* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(const ExpressionIn* expr) final {}
    void visit(const ExpressionIndexOfArray* expr) final {}
    void visit(const ExpressionIndexOfBytes* expr) final {}
    void visit(const ExpressionIndexOfCP* expr) final {}
    void visit(const ExpressionIsNumber* expr) final {}
    void visit(const ExpressionLet* expr) final {
        _context->varsFrameStack.push(ExpressionVisitorContext::VarsFrame{
            std::begin(expr->getOrderedVariableIds()), std::end(expr->getOrderedVariableIds())});
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
    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
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
    void visit(const ExpressionSwitch* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
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

private:
    void visitMultiBranchLogicExpression(const Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);

        if (expr->getChildren().size() < 2) {
            // All this bookkeeping is only necessary for short circuiting, so we can skip it if we
            // don't have two or more branches.
            return;
        }

        _context->evalStack.emplaceFrame(EvalStage{});
    }

    ExpressionVisitorContext* _context;
};

class ExpressionInVisitor final : public ExpressionConstVisitor {
public:
    ExpressionInVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(const ExpressionConstant* expr) final {}
    void visit(const ExpressionAbs* expr) final {}
    void visit(const ExpressionAdd* expr) final {}
    void visit(const ExpressionAllElementsTrue* expr) final {}
    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(const ExpressionAnyElementTrue* expr) final {}
    void visit(const ExpressionArray* expr) final {}
    void visit(const ExpressionArrayElemAt* expr) final {}
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
    void visit(const ExpressionCond* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(const ExpressionDateDiff* expr) final {}
    void visit(const ExpressionDateFromString* expr) final {}
    void visit(const ExpressionDateFromParts* expr) final {}
    void visit(const ExpressionDateToParts* expr) final {}
    void visit(const ExpressionDateToString* expr) final {}
    void visit(const ExpressionDateTrunc*) final {}
    void visit(const ExpressionDivide* expr) final {}
    void visit(const ExpressionEncryptedBetween* expr) final {}
    void visit(const ExpressionExp* expr) final {}
    void visit(const ExpressionFieldPath* expr) final {}
    void visit(const ExpressionFilter* expr) final {
        // $filter has up to three children: cond, as, and limit (optional).
        // Only the filter predicate (cond) needs access to the value of the "as" arg, here referred
        // to as current element var. The filter predicate will be the second element in
        // the _children vector the expression_walker walks and limit will be the last if it exists.

        const auto limitPredIndex = 3;
        const auto filterPredIndex = 2;
        auto variableId = expr->getVariableId();

        // We use this counter in the visit methods of ExpressionFilter to track which child we are
        // processing and which children we've already processed.

        auto& currentIndex = _context->filterExprChildrenCounter.top();
        if (++currentIndex == filterPredIndex) {
            tassert(3273901,
                    "Current element variable already exists in _context",
                    _context->environment.find(variableId) == _context->environment.end());
            auto currentElementSlot = _context->state.slotId();
            _context->environment.insert({variableId, currentElementSlot});
            // This stack maintains the current element variable for $filter so that we can erase it
            // from our context in inVisitor when processing the optional limit arg, but then still
            // have access to this var again in postVisitor when constructing the filter
            // predicate/'cond' subtree.
            _context->filterExprSlotIdStack.push(currentElementSlot);
        }

        if (currentIndex == limitPredIndex) {
            tassert(3273902,
                    "$Filter expression has unknown third child (not 'limit' arg)",
                    expr->hasLimit());
            _context->environment.erase(variableId);
        }
        // Push new frame to provide clean context for sub-tree generated by filter predicate or
        // limit arg.
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(const ExpressionFloor* expr) final {}
    void visit(const ExpressionIfNull* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
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

        invariant(!currentFrame.variablesToBind.empty());

        auto varToBind = currentFrame.variablesToBind.front();
        currentFrame.variablesToBind.pop_front();

        // We create two bindings. First, the initializer result is bound to a slot (if it's not
        // already in a slot).
        auto [slotToBind, projectStage] = projectEvalExpr(_context->popEvalExpr(),
                                                          _context->extractCurrentEvalStage(),
                                                          _context->planNodeId,
                                                          _context->state.slotIdGenerator);
        _context->setCurrentStage(std::move(projectStage));
        currentFrame.slotsForLetVariables.insert(slotToBind);

        // Second, we bind this variables AST-level name (with type Variable::Id) to the SlotId that
        // will be used for compilation and execution. Once this "stage builder" finishes, these
        // Variable::Id bindings will no longer be relevant.
        invariant(_context->environment.find(varToBind) == _context->environment.end());
        _context->environment.insert({varToBind, slotToBind});
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
    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
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
    void visit(const ExpressionSwitch* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
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

private:
    void visitMultiBranchLogicExpression(const Expression* expr, sbe::EPrimBinary::Op logicOp) {
        // The infix visitor should only visit expressions with more than one child.
        invariant(expr->getChildren().size() >= 2);
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);
        _context->evalStack.emplaceFrame(EvalStage{});
    }

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
        auto [tag, val] = makeValue(expr->getValue());
        _context->pushExpr(sbe::makeE<sbe::EConstant>(tag, val));
    }

    void visit(const ExpressionAbs* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto absExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903700},
                                                 "$abs only supports numeric types")},
            CaseValuePair{generateLongLongMinCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903701},
                                                 "can't take $abs of long long min")},
            makeFunction("abs", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(absExpr)));
    }

    void visit(const ExpressionAdd* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->state.frameId();

        auto generateNotNumberOrDate = [frameId](const sbe::value::SlotId slotId) {
            sbe::EVariable var{frameId, slotId};
            return makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                makeNot(makeFunction("isNumber", var.clone())),
                                makeNot(makeFunction("isDate", var.clone())));
        };

        if (arity == 0) {
            // Return a zero constant if the expression has no operand children.
            _context->pushExpr(makeConstant(sbe::value::TypeTags::NumberInt32, 0));
        } else {
            sbe::EExpression::Vector binds;
            sbe::EExpression::Vector argVars;
            sbe::EExpression::Vector checkExprsNull;
            sbe::EExpression::Vector checkExprsNotNumberOrDate;
            binds.reserve(arity);
            argVars.reserve(arity);
            checkExprsNull.reserve(arity);
            checkExprsNotNumberOrDate.reserve(arity);
            for (size_t idx = 0; idx < arity; ++idx) {
                binds.push_back(_context->popExpr());
                argVars.push_back(sbe::makeE<sbe::EVariable>(frameId, idx));

                checkExprsNull.push_back(generateNullOrMissing(frameId, idx));
                checkExprsNotNumberOrDate.push_back(generateNotNumberOrDate(idx));
            }

            // At this point 'binds' vector contains arguments of $add expression in the reversed
            // order. We need to reverse it back to perform summation in the right order below.
            // Summation in different order can lead to different result because of accumulated
            // precision errors from floating point types.
            std::reverse(std::begin(binds), std::end(binds));

            using iter_t = sbe::EExpression::Vector::iterator;
            auto checkNullAllArguments = std::accumulate(
                std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
                std::move_iterator<iter_t>(checkExprsNull.end()),
                std::move(checkExprsNull.front()),
                [](auto&& acc, auto&& ex) {
                    return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                });
            auto checkNotNumberOrDateAllArguments = std::accumulate(
                std::move_iterator<iter_t>(checkExprsNotNumberOrDate.begin() + 1),
                std::move_iterator<iter_t>(checkExprsNotNumberOrDate.end()),
                std::move(checkExprsNotNumberOrDate.front()),
                [](auto&& acc, auto&& ex) {
                    return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                });
            auto addOp = std::move(argVars[0]);
            for (size_t idx = 1; idx < arity; ++idx) {
                addOp = makeLocalBind(
                    _context->state.frameIdGenerator,
                    [&](sbe::EVariable var1, sbe::EVariable var2) {
                        return sbe::makeE<sbe::EIf>(
                            makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                         makeFunction("isDate", var1.clone()),
                                         makeFunction("isDate", var2.clone())),
                            makeFail(4974202, "only one date allowed in an $add expression"),
                            makeBinaryOp(sbe::EPrimBinary::add, var1.clone(), var2.clone()));
                    },
                    std::move(addOp),
                    std::move(argVars[idx]));
            }
            auto addExpr = buildMultiBranchConditional(
                CaseValuePair{std::move(checkNullAllArguments),
                              makeConstant(sbe::value::TypeTags::Null, 0)},
                CaseValuePair{
                    std::move(checkNotNumberOrDateAllArguments),
                    makeFail(4974201, "only numbers and dates are allowed in an $add expression")},
                std::move(addOp));
            _context->pushExpr(
                sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(addExpr)));
        }
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(const ExpressionAnyElementTrue* expr) final {
        auto [inputSlot, stage] = projectEvalExpr(_context->popEvalExpr(),
                                                  _context->extractCurrentEvalStage(),
                                                  _context->planNodeId,
                                                  _context->state.slotIdGenerator);

        auto fromBranch = makeFilter<false>(
            std::move(stage),
            makeBinaryOp(sbe::EPrimBinary::logicOr,
                         makeFillEmptyFalse(makeFunction("isArray", makeVariable(inputSlot))),
                         makeFail(5159200, "$anyElementTrue's argument must be an array")),
            _context->planNodeId);

        auto innerOutputSlot = _context->state.slotId();
        auto innerBranch = makeProject(makeLimitCoScanStage(_context->planNodeId),
                                       _context->planNodeId,
                                       innerOutputSlot,
                                       generateCoerceToBoolExpression(inputSlot));

        auto traverseSlot = _context->state.slotId();
        auto traverseStage = makeTraverse(std::move(fromBranch),
                                          std::move(innerBranch),
                                          inputSlot,
                                          traverseSlot,
                                          innerOutputSlot,
                                          makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                       makeVariable(traverseSlot),
                                                       makeVariable(innerOutputSlot)),
                                          makeVariable(traverseSlot),
                                          _context->planNodeId,
                                          1,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(makeFillEmptyFalse(makeVariable(traverseSlot)),
                           std::move(traverseStage));
    }
    void visit(const ExpressionArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionArrayElemAt* expr) final {
        _context->ensureArity(2);

        auto index = _context->popExpr();

        // It's important that we project the array to a slot here. If we didn't do this, then the
        // view of the array element could potentially outlive the array itself (which could result
        // in use-after-free bugs).
        auto [arraySlot, stage] = projectEvalExpr(_context->popEvalExpr(),
                                                  _context->extractCurrentEvalStage(),
                                                  _context->planNodeId,
                                                  _context->state.slotIdGenerator);
        auto array = makeVariable(arraySlot);

        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(array), std::move(index));
        sbe::EVariable arrayRef{frameId, 0};
        sbe::EVariable indexRef{frameId, 1};

        auto int32Index = [&]() {
            auto convertedIndex = sbe::makeE<sbe::ENumericConvert>(
                indexRef.clone(), sbe::value::TypeTags::NumberInt32);
            auto frameId = _context->state.frameId();
            auto binds = sbe::makeEs(std::move(convertedIndex));
            sbe::EVariable convertedIndexRef{frameId, 0};

            auto inExpression = sbe::makeE<sbe::EIf>(
                makeFunction("exists", convertedIndexRef.clone()),
                convertedIndexRef.clone(),
                sbe::makeE<sbe::EFail>(
                    ErrorCodes::Error{5126703},
                    "$arrayElemAt second argument cannot be represented as a 32-bit integer"));

            return sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(inExpression));
        }();

        auto anyOfArgumentsIsNullish = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                    generateNullOrMissing(arrayRef),
                                                    generateNullOrMissing(indexRef));
        auto firstArgumentIsNotArray = makeNot(makeFunction("isArray", arrayRef.clone()));
        auto secondArgumentIsNotNumeric = generateNonNumericCheck(indexRef);
        auto arrayElemAtExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(anyOfArgumentsIsNullish),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(firstArgumentIsNotArray),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126701},
                                                 "$arrayElemAt first argument must be an array")},
            CaseValuePair{std::move(secondArgumentIsNotNumeric),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126702},
                                                 "$arrayElemAt second argument must be a number")},
            makeFunction("getElement", arrayRef.clone(), std::move(int32Index)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(arrayElemAtExpr)),
            std::move(stage));
    }
    void visit(const ExpressionFirst* expr) final {
        buildArrayAccessByConstantIndex(_context, expr->getOpName(), 0);
    }
    void visit(const ExpressionLast* expr) final {
        buildArrayAccessByConstantIndex(_context, expr->getOpName(), -1);
    }
    void visit(const ExpressionObjectToArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionArrayToObject* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBsonSize* expr) final {
        // Build an expression which evaluates the size of a BSON document and validates the input
        // argument.
        // 1. If the argument is null or empty, return null.
        // 2. Else, if the argument is a BSON document, return its size.
        // 3. Else, raise an error.

        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto bsonSizeExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonObjectCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5043001},
                                                 "$bsonSize requires a document input")},
            makeFunction("bsonSize", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(bsonSizeExpr)));
    }
    void visit(const ExpressionCeil* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto ceilExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903702},
                                                 "$ceil only supports numeric types")},
            makeFunction("ceil", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(ceilExpr)));
    }
    void visit(const ExpressionCoerceToBool* expr) final {
        // Since $coerceToBool is internal-only and there are not yet any input expressions that
        // generate an ExpressionCoerceToBool expression, we will leave it as unreachable for now.
        MONGO_UNREACHABLE;
    }
    void visit(const ExpressionCompare* expr) final {
        _context->ensureArity(2);
        sbe::EExpression::Vector operands(2);
        for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
            *it = _context->popExpr();
        }

        auto frameId = _context->state.frameId();
        sbe::EVariable lhsRef(frameId, 0);
        sbe::EVariable rhsRef(frameId, 1);

        auto comparisonOperator = [expr]() {
            switch (expr->getOp()) {
                case ExpressionCompare::CmpOp::EQ:
                    return sbe::EPrimBinary::eq;
                case ExpressionCompare::CmpOp::NE:
                    return sbe::EPrimBinary::neq;
                case ExpressionCompare::CmpOp::GT:
                    return sbe::EPrimBinary::greater;
                case ExpressionCompare::CmpOp::GTE:
                    return sbe::EPrimBinary::greaterEq;
                case ExpressionCompare::CmpOp::LT:
                    return sbe::EPrimBinary::less;
                case ExpressionCompare::CmpOp::LTE:
                    return sbe::EPrimBinary::lessEq;
                case ExpressionCompare::CmpOp::CMP:
                    return sbe::EPrimBinary::cmp3w;
            }
            MONGO_UNREACHABLE;
        }();

        // We use the "cmp3w" primitive for every comparison, because it "type brackets" its
        // comparisons (for example, a number will always compare as less than a string). The other
        // comparison primitives are designed for comparing values of the same type.
        auto cmp3w = makeBinaryOp(
            sbe::EPrimBinary::cmp3w, lhsRef.clone(), rhsRef.clone(), _context->state.data->env);
        auto cmp = (comparisonOperator == sbe::EPrimBinary::cmp3w)
            ? std::move(cmp3w)
            : makeBinaryOp(comparisonOperator,
                           std::move(cmp3w),
                           sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(0)));

        // If either operand evaluates to "Nothing", then the entire operation expressed by 'cmp'
        // will also evaluate to "Nothing". MQL comparisons, however, treat "Nothing" as if it is a
        // value that is less than everything other than MinKey. (Notably, two expressions that
        // evaluate to "Nothing" are considered equal to each other.)
        // We also need to explicitly check for 'bsonUndefined' type because it is considered equal
        // to "Nothing" according to MQL semantics.
        auto generateExists = [&](const sbe::EVariable& var) {
            auto undefinedTypeMask = static_cast<int64_t>(getBSONTypeMask(BSONType::Undefined));
            return makeBinaryOp(
                sbe::EPrimBinary::logicAnd,
                makeFunction("exists", var.clone()),
                makeFunction("typeMatch",
                             var.clone(),
                             makeConstant(sbe::value::TypeTags::NumberInt64,
                                          sbe::value::bitcastFrom<int64_t>(~undefinedTypeMask))));
        };

        auto nothingFallbackCmp =
            makeBinaryOp(comparisonOperator, generateExists(lhsRef), generateExists(rhsRef));

        auto cmpWithFallback =
            makeFunction("fillEmpty", std::move(cmp), std::move(nothingFallbackCmp));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(operands), std::move(cmpWithFallback)));
    }

    void visit(const ExpressionConcat* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Concatination of no strings is an empty string.
        if (arity == 0) {
            _context->pushExpr(makeConstant(""_sd));
            return;
        }

        auto frameId = _context->state.frameId();
        sbe::EExpression::Vector binds;
        sbe::EExpression::Vector checkNullArg;
        sbe::EExpression::Vector checkStringArg;
        sbe::EExpression::Vector argVars;
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            sbe::EVariable var(frameId, slot);
            binds.push_back(_context->popExpr());
            checkNullArg.push_back(generateNullOrMissing(frameId, slot));
            checkStringArg.push_back(makeFunction("isString", var.clone()));
            argVars.push_back(var.clone());
        }
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = sbe::EExpression::Vector::iterator;

        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkNullArg.begin() + 1),
            std::move_iterator<iter_t>(checkNullArg.end()),
            std::move(checkNullArg.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });

        auto checkStringAllArguments = std::accumulate(
            std::move_iterator<iter_t>(checkStringArg.begin() + 1),
            std::move_iterator<iter_t>(checkStringArg.end()),
            std::move(checkStringArg.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicAnd, std::move(acc), std::move(ex));
            });

        auto concatExpr = sbe::makeE<sbe::EIf>(
            std::move(checkNullAnyArgument),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(std::move(checkStringAllArguments),
                                 sbe::makeE<sbe::EFunction>("concat", std::move(argVars)),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073001},
                                                        "$concat supports only strings")));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(concatExpr)));
    }

    void visit(const ExpressionConcatArrays* expr) final {
        auto numChildren = expr->getChildren().size();
        _context->ensureArity(numChildren);

        // If there are no children, return an empty array.
        if (numChildren == 0) {
            auto [emptyArrTag, emptyArrValue] = sbe::value::makeNewArray();
            _context->pushExpr(makeConstant(emptyArrTag, emptyArrValue));
            return;
        }

        sbe::EExpression::Vector nullChecks;
        std::vector<EvalStage> unionBranches;
        std::vector<sbe::value::SlotVector> unionInputSlots;
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;

        nullChecks.reserve(numChildren);
        unionBranches.reserve(numChildren);
        unionInputSlots.reserve(numChildren);
        for (size_t idx = 0; idx < numChildren; ++idx) {
            auto outputSlot = _context->state.slotId();
            projections.emplace(outputSlot, _context->popExpr());
            unionBranches.emplace_back(
                EvalStage{makeLimitCoScanTree(_context->planNodeId), sbe::makeSV()});
            unionInputSlots.emplace_back(sbe::makeSV(outputSlot));
            nullChecks.emplace_back(generateNullOrMissing(outputSlot));
        }

        // Build a project to capture our child expressions.
        std::reverse(std::begin(unionInputSlots), std::end(unionInputSlots));
        auto project = makeProject(
            _context->extractCurrentEvalStage(), std::move(projections), _context->planNodeId);

        // Build a union stage to consolidate array input branches into a stream.
        auto unionOutputSlot = _context->state.slotId();
        auto unionStage = makeUnion(std::move(unionBranches),
                                    std::move(unionInputSlots),
                                    sbe::makeSV(unionOutputSlot),
                                    _context->planNodeId);

        auto collatorSlot = _context->state.data->env->getSlotIfExists("collator"_sd);

        // Build a filter that will throw an 'EFail' if any element coming from the union is NOT
        // an array.
        auto filter = makeFilter<false, false>(
            std::move(unionStage),
            makeBinaryOp(sbe::EPrimBinary::logicOr,
                         makeFunction("isArray", makeVariable(unionOutputSlot)),
                         sbe::makeE<sbe::EFail>(ErrorCodes::Error{5153400},
                                                "$concatArrays only supports arrays")),
            _context->planNodeId);

        // Build subtree to handle nulls. If an input is null, return null. Otherwise, unwind the
        // input and concatenate it into an array using addToArray.
        auto unwindEvalStage =
            makeUnwind(std::move(filter), _context->state.slotIdGenerator, _context->planNodeId);
        auto unwindSlot = unwindEvalStage.outSlots.front();

        // Create a group stage to append all streamed elements into one array. This is the final
        // output when the input consists entirely of arrays.
        auto finalAddToArrayExpr = makeFunction("addToArray", makeVariable(unwindSlot));
        auto finalGroupSlot = _context->state.slotId();
        auto finalGroupStage =
            makeHashAgg(std::move(unwindEvalStage),
                        sbe::makeSV(),
                        sbe::makeEM(finalGroupSlot, std::move(finalAddToArrayExpr)),
                        collatorSlot,
                        _context->state.allowDiskUse,
                        _context->planNodeId);

        // Returns true if any of our input expressions return null.
        using iter_t = sbe::EExpression::Vector::iterator;
        auto checkPartsForNull = std::accumulate(
            std::move_iterator<iter_t>(nullChecks.begin() + 1),
            std::move_iterator<iter_t>(nullChecks.end()),
            std::move(nullChecks.front()),
            [](auto&& acc, auto&& b) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(b));
            });

        // Create a branch stage to select between the branch that produces one null if any elements
        // in the original input were null or missing, or otherwise select the branch that unwinds
        // and concatenates elements into the output array.
        auto [nullSlot, nullStage] = [&] {
            auto outputSlot = _context->state.slotId();
            auto nullEvalStage =
                makeProject({makeLimitCoScanTree(_context->planNodeId), sbe::makeSV()},
                            _context->planNodeId,
                            outputSlot,
                            makeConstant(sbe::value::TypeTags::Null, 0));
            return std::make_pair(outputSlot, std::move(nullEvalStage));
        }();

        auto branchSlot = _context->state.slotId();
        auto branchNullEvalStage = makeBranch(std::move(nullStage),
                                              std::move(finalGroupStage),
                                              std::move(checkPartsForNull),
                                              sbe::makeSV(nullSlot),
                                              sbe::makeSV(finalGroupSlot),
                                              sbe::makeSV(branchSlot),
                                              _context->planNodeId);

        // Create nlj to connect outer project with inner branch that handles null input.
        _context->pushExpr(branchSlot,
                           makeLoopJoin(std::move(project),
                                        std::move(branchNullEvalStage),
                                        _context->planNodeId,
                                        _context->getLexicalEnvironment()));
    }
    void visit(const ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionDateDiff* expr) final {
        using namespace std::literals;
        auto frameId = _context->state.frameId();
        sbe::EExpression::Vector arguments;
        sbe::EExpression::Vector bindings;
        sbe::EVariable startDateRef(frameId, 0);
        sbe::EVariable endDateRef(frameId, 1);
        sbe::EVariable unitRef(frameId, 2);
        sbe::EVariable timezoneRef(frameId, 3);
        sbe::EVariable startOfWeekRef(frameId, 4);

        // An auxiliary boolean variable to hold a value of a common subexpression 'unit'=="week"
        // (string).
        sbe::EVariable unitIsWeekRef(frameId, 5);

        auto children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(3 + (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isStartOfWeekSpecified() ? 1 : 0));

        // Get child expressions.
        auto startOfWeekExpression = expr->isStartOfWeekSpecified() ? _context->popExpr() : nullptr;
        auto timezoneExpression =
            expr->isTimezoneSpecified() ? _context->popExpr() : makeConstant("UTC"_sd);
        auto unitExpression = _context->popExpr();
        auto endDateExpression = _context->popExpr();
        auto startDateExpression = _context->popExpr();

        auto timezoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);

        //  Set parameters for an invocation of built-in "dateDiff" function.
        arguments.push_back(sbe::makeE<sbe::EVariable>(timezoneDBSlot));
        arguments.push_back(startDateRef.clone());
        arguments.push_back(endDateRef.clone());
        arguments.push_back(unitRef.clone());
        arguments.push_back(timezoneRef.clone());
        if (expr->isStartOfWeekSpecified()) {
            // Parameter "startOfWeek" - if the time unit is the week, then pass value of parameter
            // "startOfWeek" of "$dateDiff" expression, otherwise pass a valid default value, since
            // "dateDiff" built-in function does not accept non-string type values for this
            // parameter.
            arguments.push_back(sbe::makeE<sbe::EIf>(
                unitIsWeekRef.clone(), startOfWeekRef.clone(), makeConstant("sun"_sd)));
        }

        // Set bindings for the frame.
        bindings.push_back(std::move(startDateExpression));
        bindings.push_back(std::move(endDateExpression));
        bindings.push_back(std::move(unitExpression));
        bindings.push_back(std::move(timezoneExpression));
        if (expr->isStartOfWeekSpecified()) {
            bindings.push_back(std::move(startOfWeekExpression));
            bindings.push_back(generateIsEqualToStringCheck(unitRef, "week"_sd));
        }

        // Create an expression to invoke built-in "dateDiff" function.
        auto dateDiffFunctionCall = sbe::makeE<sbe::EFunction>("dateDiff"_sd, std::move(arguments));

        // Create expressions to check that each argument to "dateDiff" function exists, is not
        // null, and is of the correct type.
        std::vector<CaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(startDateRef));
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(endDateRef));
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(unitRef));
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(timezoneRef));
        if (expr->isStartOfWeekSpecified()) {
            inputValidationCases.emplace_back(
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                             unitIsWeekRef.clone(),
                                             generateNullOrMissing(startOfWeekRef)),
                makeConstant(sbe::value::TypeTags::Null, 0));
        }

        // "timezone" parameter validation.
        inputValidationCases.emplace_back(
            generateNonStringCheck(timezoneRef),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166504},
                                   "$dateDiff parameter 'timezone' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeFunction(
                "isTimezone", sbe::makeE<sbe::EVariable>(timezoneDBSlot), timezoneRef.clone())),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166505},
                                   "$dateDiff parameter 'timezone' must be a valid timezone"));

        // "startDate" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            startDateRef, ErrorCodes::Error{5166500}, "$dateDiff"_sd, "startDate"_sd));

        // "endDate" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            endDateRef, ErrorCodes::Error{5166501}, "$dateDiff"_sd, "endDate"_sd));

        // "unit" parameter validation.
        inputValidationCases.emplace_back(
            generateNonStringCheck(unitRef),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166502},
                                   "$dateDiff parameter 'unit' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeFunction("isTimeUnit", unitRef.clone())),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166503},
                                   "$dateDiff parameter 'unit' must be a valid time unit"));

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
            inputValidationCases.emplace_back(
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                             unitIsWeekRef.clone(),
                                             generateNonStringCheck(startOfWeekRef)),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5338801},
                                       "$dateDiff parameter 'startOfWeek' must be a string"));
            inputValidationCases.emplace_back(
                sbe::makeE<sbe::EPrimBinary>(
                    sbe::EPrimBinary::logicAnd,
                    unitIsWeekRef.clone(),
                    makeNot(makeFunction("isDayOfWeek", startOfWeekRef.clone()))),
                sbe::makeE<sbe::EFail>(
                    ErrorCodes::Error{5338802},
                    "$dateDiff parameter 'startOfWeek' must be a valid day of the week"));
        }

        auto dateDiffExpression = buildMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateDiffFunctionCall));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(bindings), std::move(dateDiffExpression)));
    }
    void visit(const ExpressionDateFromString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(const ExpressionDateFromParts* expr) final {
        // This expression can carry null children depending on the set of fields provided,
        // to compute a date from parts so we only need to pop if a child exists.
        auto children = expr->getChildren();
        invariant(children.size() == 11);

        auto eTimezone = children[10] ? _context->popExpr() : nullptr;
        auto eIsoDayOfWeek = children[9] ? _context->popExpr() : nullptr;
        auto eIsoWeek = children[8] ? _context->popExpr() : nullptr;
        auto eIsoWeekYear = children[7] ? _context->popExpr() : nullptr;
        auto eMillisecond = children[6] ? _context->popExpr() : nullptr;
        auto eSecond = children[5] ? _context->popExpr() : nullptr;
        auto eMinute = children[4] ? _context->popExpr() : nullptr;
        auto eHour = children[3] ? _context->popExpr() : nullptr;
        auto eDay = children[2] ? _context->popExpr() : nullptr;
        auto eMonth = children[1] ? _context->popExpr() : nullptr;
        auto eYear = children[0] ? _context->popExpr() : nullptr;

        // Save a flag to determine if we are in the case of an iso
        // week year. Note that the agg expression parser ensures that one of date or
        // isoWeekYear inputs are provided so we don't need to enforce that at this depth.
        auto isIsoWeekYear = eIsoWeekYear ? true : false;

        auto frameId = _context->state.frameId();
        sbe::EVariable yearRef(frameId, 0);
        sbe::EVariable monthRef(frameId, 1);
        sbe::EVariable dayRef(frameId, 2);
        sbe::EVariable hourRef(frameId, 3);
        sbe::EVariable minRef(frameId, 4);
        sbe::EVariable secRef(frameId, 5);
        sbe::EVariable millisecRef(frameId, 6);
        sbe::EVariable timeZoneRef(frameId, 7);

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
            [](sbe::EExpression& var, int16_t lower, int16_t upper, const std::string& varName) {
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
                    makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                 makeBinaryOp(sbe::EPrimBinary::greaterEq,
                                              var.clone(),
                                              sbe::makeE<sbe::EConstant>(
                                                  sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(lower))),
                                 makeBinaryOp(sbe::EPrimBinary::lessEq,
                                              var.clone(),
                                              sbe::makeE<sbe::EConstant>(
                                                  sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(upper)))),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848972}, errMsg));
            };

        // Here we want to validate each field that is provided as input to the agg expression. To
        // do this we implement the following checks:
        //
        // 1) Check if the value in a given slot null or missing. If so bind null to l1.0, and
        // continue to the next binding. Otherwise, do check 2 below.
        //
        // 2) Check if the value in a given slot is an integral int64. This test is done by
        // computing a lossless conversion of the value in s1 to an int64. The exposed
        // conversion function by the vm returns a value if there is no loss of precision,
        // otherwise it returns Nothing. In both the valid or Nothing case, we can store the result
        // of the conversion in l2.0 of the inner let binding and test for existence. If the
        // existence check fails we know the conversion is lossy and we can fail the query.
        // Otherwise, the inner let evaluates to the converted value which is then bound to the
        // outer let.
        //
        // Each invocation of fieldConversionBinding will produce a nested let of the form.
        //
        // let [l1.0 = s1] in
        //   if (isNull(l1.0) || !exists(l1.0), null,
        //     let [l2.0 = convert(l1.0, int)] in
        //       if (exists(l2.0), l2.0, fail("... must evaluate to an integer")]), ...]
        //  in ...
        auto fieldConversionBinding = [](std::unique_ptr<sbe::EExpression> expr,
                                         sbe::value::FrameIdGenerator* frameIdGenerator,
                                         const std::string& varName) {
            auto outerFrameId = frameIdGenerator->generate();
            auto innerFrameId = frameIdGenerator->generate();
            sbe::EVariable outerSlotRef(outerFrameId, 0);
            sbe::EVariable convertedFieldRef(innerFrameId, 0);

            return sbe::makeE<sbe::ELocalBind>(
                outerFrameId,
                sbe::makeEs(expr->clone()),
                sbe::makeE<sbe::EIf>(
                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                 makeNot(makeFunction("exists", outerSlotRef.clone())),
                                 makeFunction("isNull", outerSlotRef.clone())),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                    sbe::makeE<sbe::ELocalBind>(
                        innerFrameId,
                        sbe::makeEs(sbe::makeE<sbe::ENumericConvert>(
                            outerSlotRef.clone(), sbe::value::TypeTags::NumberInt64)),
                        sbe::makeE<sbe::EIf>(
                            makeFunction("exists", convertedFieldRef.clone()),
                            convertedFieldRef.clone(),
                            sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848979},
                                                   str::stream()
                                                       << "'" << varName << "'"
                                                       << " must evaluate to an integer")))));
        };

        // Build two vectors on the fly to elide bound and conversion for defaulted values.
        std::vector<std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>>
            boundChecks;  // checks for lower and upper bounds of date fields.

        // Operands is for the outer let bindings.
        sbe::EExpression::Vector operands;
        if (isIsoWeekYear) {
            if (!eIsoWeekYear) {
                eIsoWeekYear = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                          sbe::value::bitcastFrom<int32_t>(1970));
                operands.push_back(std::move(eIsoWeekYear));
            } else {
                boundChecks.push_back(boundedCheck(yearRef, 1, 9999, "isoWeekYear"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoWeekYear), _context->state.frameIdGenerator, "isoWeekYear"));
            }
            if (!eIsoWeek) {
                eIsoWeek = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eIsoWeek));
            } else {
                boundChecks.push_back(boundedCheck(monthRef, minInt16, maxInt16, "isoWeek"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoWeek), _context->state.frameIdGenerator, "isoWeek"));
            }
            if (!eIsoDayOfWeek) {
                eIsoDayOfWeek = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                           sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eIsoDayOfWeek));
            } else {
                boundChecks.push_back(boundedCheck(dayRef, minInt16, maxInt16, "isoDayOfWeek"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoDayOfWeek), _context->state.frameIdGenerator, "isoDayOfWeek"));
            }
        } else {
            // The regular year/month/day case.
            if (!eYear) {
                eYear = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   sbe::value::bitcastFrom<int32_t>(1970));
                operands.push_back(std::move(eYear));
            } else {
                boundChecks.push_back(boundedCheck(yearRef, 1, 9999, "year"));
                operands.push_back(fieldConversionBinding(
                    std::move(eYear), _context->state.frameIdGenerator, "year"));
            }
            if (!eMonth) {
                eMonth = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                    sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eMonth));
            } else {
                boundChecks.push_back(boundedCheck(monthRef, minInt16, maxInt16, "month"));
                operands.push_back(fieldConversionBinding(
                    std::move(eMonth), _context->state.frameIdGenerator, "month"));
            }
            if (!eDay) {
                eDay = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eDay));
            } else {
                boundChecks.push_back(boundedCheck(dayRef, minInt16, maxInt16, "day"));
                operands.push_back(fieldConversionBinding(
                    std::move(eDay), _context->state.frameIdGenerator, "day"));
            }
        }
        if (!eHour) {
            eHour = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                               sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eHour));
        } else {
            boundChecks.push_back(boundedCheck(hourRef, minInt16, maxInt16, "hour"));
            operands.push_back(
                fieldConversionBinding(std::move(eHour), _context->state.frameIdGenerator, "hour"));
        }
        if (!eMinute) {
            eMinute = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                 sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eMinute));
        } else {
            boundChecks.push_back(boundedCheck(minRef, minInt16, maxInt16, "minute"));
            operands.push_back(fieldConversionBinding(
                std::move(eMinute), _context->state.frameIdGenerator, "minute"));
        }
        if (!eSecond) {
            eSecond = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                 sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eSecond));
        } else {
            // MQL doesn't place bound restrictions on the second field, because seconds carry over
            // to minutes and can be large ints such as 71,841,012 or even unix epochs.
            operands.push_back(fieldConversionBinding(
                std::move(eSecond), _context->state.frameIdGenerator, "second"));
        }
        if (!eMillisecond) {
            eMillisecond = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eMillisecond));
        } else {
            // MQL doesn't enforce bound restrictions on millisecond fields because milliseconds
            // carry over to seconds.
            operands.push_back(fieldConversionBinding(
                std::move(eMillisecond), _context->state.frameIdGenerator, "millisecond"));
        }
        if (!eTimezone) {
            eTimezone = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::StringSmall, 0);
            operands.push_back(std::move(eTimezone));
        } else {
            // Validate that eTimezone is a string.
            auto tzFrameId = _context->state.frameId();
            sbe::EVariable timezoneRef(tzFrameId, 0);
            operands.push_back(sbe::makeE<sbe::ELocalBind>(
                tzFrameId,
                sbe::makeEs(std::move(eTimezone)),
                sbe::makeE<sbe::EIf>(
                    makeFunction("isString", timeZoneRef.clone()),
                    timezoneRef.clone(),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848980},
                                           str::stream()
                                               << "'timezone' must evaluate to a string"))));
        }

        // Make a disjunction of null checks for each date part by over this vector. These checks
        // are necessary after the initial conversion computation because we need have the outer let
        // binding evaluate to null if any field is null.
        auto nullExprs = sbe::makeEs(generateNullOrMissing(frameId, 7),
                                     generateNullOrMissing(frameId, 6),
                                     generateNullOrMissing(frameId, 5),
                                     generateNullOrMissing(frameId, 4),
                                     generateNullOrMissing(frameId, 3),
                                     generateNullOrMissing(frameId, 2),
                                     generateNullOrMissing(frameId, 1),
                                     generateNullOrMissing(frameId, 0));

        using iter_t = sbe::EExpression::Vector::iterator;
        auto checkPartsForNull = std::accumulate(
            std::move_iterator<iter_t>(nullExprs.begin() + 1),
            std::move_iterator<iter_t>(nullExprs.end()),
            std::move(nullExprs.front()),
            [](auto&& acc, auto&& b) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(b));
            });

        // Invocation of the datePartsWeekYear and dateParts functions depend on a TimeZoneDatabase
        // for datetime computation. This global object is registered as an unowned value in the
        // runtime environment so we pass the corresponding slot to the datePartsWeekYear and
        // dateParts functions as a variable.
        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        auto computeDate = makeFunction(isIsoWeekYear ? "datePartsWeekYear" : "dateParts",
                                        sbe::makeE<sbe::EVariable>(timeZoneDBSlot),
                                        yearRef.clone(),
                                        monthRef.clone(),
                                        dayRef.clone(),
                                        hourRef.clone(),
                                        minRef.clone(),
                                        secRef.clone(),
                                        millisecRef.clone(),
                                        timeZoneRef.clone());

        using iterPair_t = std::vector<std::pair<std::unique_ptr<sbe::EExpression>,
                                                 std::unique_ptr<sbe::EExpression>>>::iterator;
        auto computeBoundChecks =
            std::accumulate(std::move_iterator<iterPair_t>(boundChecks.begin()),
                            std::move_iterator<iterPair_t>(boundChecks.end()),
                            std::move(computeDate),
                            [](auto&& acc, auto&& b) {
                                return sbe::makeE<sbe::EIf>(
                                    std::move(b.first), std::move(acc), std::move(b.second));
                            });

        // This final ite expression allows short-circuting of the null field case. If the nullish,
        // checks pass, then we check the bounds of each field and invoke the builtins if all checks
        // pass.
        auto computeDateOrNull =
            sbe::makeE<sbe::EIf>(std::move(checkPartsForNull),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                                 std::move(computeBoundChecks));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(computeDateOrNull)));
    }

    void visit(const ExpressionDateToParts* expr) final {
        auto frameId = _context->state.frameId();
        auto children = expr->getChildren();
        std::unique_ptr<sbe::EExpression> date, timezone, isoflag;
        std::unique_ptr<sbe::EExpression> totalExprDateToParts;
        sbe::EExpression::Vector args;
        sbe::EExpression::Vector isoargs;
        sbe::EExpression::Vector operands;
        sbe::EVariable dateRef(frameId, 0);
        sbe::EVariable timezoneRef(frameId, 1);
        sbe::EVariable isoflagRef(frameId, 2);

        // Initialize arguments with values from stack or default values.
        if (children[2]) {
            isoflag = _context->popExpr();
        } else {
            isoflag = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false);
        }
        if (children[1]) {
            timezone = _context->popExpr();
        } else {
            auto [utcTag, utcVal] = sbe::value::makeNewString("UTC");
            timezone = sbe::makeE<sbe::EConstant>(utcTag, utcVal);
        }
        if (children[0]) {
            date = _context->popExpr();
        } else {
            _context->pushExpr(sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997700},
                                                      "$dateToParts must include a date"));
            return;
        }

        // Add timezoneDB to arguments.
        args.push_back(
            sbe::makeE<sbe::EVariable>(_context->state.data->env->getSlot("timeZoneDB"_sd)));
        isoargs.push_back(
            sbe::makeE<sbe::EVariable>(_context->state.data->env->getSlot("timeZoneDB"_sd)));

        // Add date to arguments.
        operands.push_back(std::move(date));
        args.push_back(dateRef.clone());
        isoargs.push_back(dateRef.clone());

        // Add timezone to arguments.
        operands.push_back(std::move(timezone));
        args.push_back(timezoneRef.clone());
        isoargs.push_back(timezoneRef.clone());

        // Add iso8601 to arguments.
        uint32_t isoTypeMask = getBSONTypeMask(sbe::value::TypeTags::Boolean);
        operands.push_back(std::move(isoflag));
        args.push_back(isoflagRef.clone());
        isoargs.push_back(isoflagRef.clone());

        // Determine whether to call dateToParts or isoDateToParts.
        auto checkIsoflagValue = buildMultiBranchConditional(
            CaseValuePair{
                makeBinaryOp(sbe::EPrimBinary::eq,
                             isoflagRef.clone(),
                             sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false)),
                sbe::makeE<sbe::EFunction>("dateToParts", std::move(args))},
            sbe::makeE<sbe::EFunction>("isoDateToParts", std::move(isoargs)));

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDateToPartsFunc = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(frameId, 1),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{makeNot(makeFunction("isString", timezoneRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997701},
                                                 "$dateToParts timezone must be a string")},
            CaseValuePair{
                makeNot(makeFunction(
                    "isTimezone",
                    sbe::makeE<sbe::EVariable>(_context->state.data->env->getSlot("timeZoneDB"_sd)),
                    timezoneRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997704},
                                       "$dateToParts timezone must be a valid timezone")},
            CaseValuePair{generateNullOrMissing(frameId, 2),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{
                makeNot(makeFunction("typeMatch",
                                     isoflagRef.clone(),
                                     makeConstant(sbe::value::TypeTags::NumberInt64,
                                                  sbe::value::bitcastFrom<int64_t>(isoTypeMask)))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997702},
                                       "$dateToParts iso8601 must be a boolean")},
            CaseValuePair{generateNullOrMissing(frameId, 0),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{
                makeNot(makeFunction(
                    "typeMatch",
                    dateRef.clone(),
                    makeConstant(sbe::value::TypeTags::NumberInt64,
                                 sbe::value::bitcastFrom<int64_t>(dateTypeMask())))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997703},
                                       "$dateToParts date must have the format of a date")},
            std::move(checkIsoflagValue));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(totalDateToPartsFunc)));
    }
    void visit(const ExpressionDateToString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(const ExpressionDateTrunc*) final {
        unsupportedExpression("$dateTrunc");
    }
    void visit(const ExpressionDivide* expr) final {
        _context->ensureArity(2);

        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();

        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
        sbe::EVariable lhsRef{frameId, 0};
        sbe::EVariable rhsRef{frameId, 1};

        auto checkIsNumber = makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                          makeFunction("isNumber", lhsRef.clone()),
                                          makeFunction("isNumber", rhsRef.clone()));

        auto checkIsNullOrMissing = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                 generateNullOrMissing(lhsRef),
                                                 generateNullOrMissing(rhsRef));

        auto divideExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkIsNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkIsNumber),
                          makeBinaryOp(sbe::EPrimBinary::div, lhsRef.clone(), rhsRef.clone())},
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073101},
                                   "$divide only supports numeric types"));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(divideExpr)));
    }
    void visit(const ExpressionExp* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto expExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903703},
                                                 "$exp only supports numeric types")},
            makeFunction("exp", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(expExpr)));
    }
    void visit(const ExpressionEncryptedBetween* expr) final {
        unsupportedExpression("$_encryptedBetween");
    }
    void visit(const ExpressionFieldPath* expr) final {
        // There's a chance that we've already generated a SBE plan stage tree for this field path,
        // in which case we avoid regeneration of the same plan stage tree.
        if (auto it = _context->state.preGeneratedExprs.find(expr->getFieldPath().fullPath());
            it != _context->state.preGeneratedExprs.end()) {
            tassert(6089301,
                    "Expressions for top-level document or a variable must not be pre-generated",
                    expr->getFieldPath().getPathLength() != 1 && !expr->isVariableReference());
            if (auto optionalSlot = it->second.getSlot(); optionalSlot) {
                _context->pushExpr(*optionalSlot);
            } else {
                auto preGeneratedExpr = it->second.extractExpr();
                _context->pushExpr(preGeneratedExpr->clone());
                it->second = std::move(preGeneratedExpr);
            }
            return;
        }

        tassert(6075901, "Must have a valid root slot", _context->optionalRootSlot.has_value());

        sbe::value::SlotId slotId;

        if (!Variables::isUserDefinedVariable(expr->getVariableId())) {
            if (expr->getVariableId() == Variables::kRootId) {
                slotId = *(_context->optionalRootSlot);
            } else if (expr->getVariableId() == Variables::kRemoveId) {
                // For the field paths that begin with "$$REMOVE", we always produce Nothing,
                // so no traversal is necessary.
                _context->pushExpr(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0));
                return;
            } else {
                auto it = Variables::kIdToBuiltinVarName.find(expr->getVariableId());
                tassert(5611300,
                        "Encountered unexpected system variable ID",
                        it != Variables::kIdToBuiltinVarName.end());

                auto variableSlot = _context->state.data->env->getSlotIfExists(it->second);
                uassert(5611301,
                        str::stream()
                            << "Builtin variable '$$" << it->second << "' is not available",
                        variableSlot.has_value());

                slotId = *variableSlot;
            }
        } else {
            auto it = _context->environment.find(expr->getVariableId());
            if (it != _context->environment.end()) {
                slotId = it->second;
            } else {
                slotId = _context->state.getGlobalVariableSlot(expr->getVariableId());
            }
        }

        if (expr->getFieldPath().getPathLength() == 1) {
            // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
            // traversal.
            _context->pushExpr(slotId);
            return;
        }

        // Dereference a dotted path, which may contain arrays requiring implicit traversal.
        const bool expectsDocumentInputOnly = slotId == *(_context->optionalRootSlot);

        auto resultExpr = generateTraverse(sbe::EVariable{slotId},
                                           expectsDocumentInputOnly,
                                           expr->getFieldPathWithoutCurrentPrefix(),
                                           _context->state.frameIdGenerator);

        _context->pushExpr(std::move(resultExpr));
    }
    void visit(const ExpressionFilter* expr) final {
        // Remove index tracking current child of $filter expression, since it is not used anymore.
        _context->filterExprChildrenCounter.pop();

        // Extract limit expression and sub-tree.
        // Note that, auto&& [limitExpr, limitStage] = ...., desugars to a copy on windows.
        auto LimitEvalPair = expr->hasLimit() ? _context->popFrame() : EvalExprStagePair{};
        auto&& limitExpr = LimitEvalPair.first;
        auto&& limitStage = LimitEvalPair.second;

        // Extract filter predicate expression and sub-tree.
        auto [filterPredicate, filterStage] = _context->popFrame();
        _context->ensureArity(1);
        auto input = _context->popExpr();

        // Filter predicate of $filter expression expects current array element to be stored in the
        // specific variable. We already allocated slot for it in the "in" visitor, now we just need
        // to retrieve it from the environment.
        // This slot will be used in the traverse stage twice - to store the input array and to
        // store current element in this array.
        auto currentElementVariable = expr->getVariableId();

        // We no longer need this mapping because filter predicate which expects it was already
        // compiled.
        _context->environment.erase(currentElementVariable);

        tassert(3273900,
                "Expected slot id for the current element variable of $filter expression",
                !_context->filterExprSlotIdStack.empty());
        auto inputArraySlot = _context->filterExprSlotIdStack.top();
        _context->filterExprSlotIdStack.pop();
        // Construct 'from' branch of traverse stage. SBE tree stored in 'fromBranch' variable looks
        // like this:
        //
        // project inputIsNotNullishSlot = !(isNull(inputArraySlot) || !exists(inputArraySlot))
        // project inputArraySlot = (
        //   let inputRef = input
        //   in
        //       if isArray(inputRef) || isNull(inputRef) || !exists(inputRef)
        //         inputRef
        //       else
        //         fail()
        // )
        // <current sub-tree stage>
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(input));
        sbe::EVariable inputRef(frameId, 0);

        auto inputIsArrayOrNullish = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                  generateNullOrMissing(inputRef),
                                                  makeFunction("isArray", inputRef.clone()));
        auto checkInputArrayType =
            sbe::makeE<sbe::EIf>(std::move(inputIsArrayOrNullish),
                                 inputRef.clone(),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073201},
                                                        "input to $filter must be an array"));
        auto inputArray =
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(checkInputArrayType));

        sbe::EVariable inputArrayVariable{inputArraySlot};
        // When our $filter expression does not have a limit arg, we insert the main tree into the
        // 'from' branch of traverse stage:
        //   traverse
        //     from
        //       project inputArraySlot = {let inputRef = inputSlot in …}
        //       <main-tree>
        //   in
        //     ...
        //
        // However, when we do have a limit arg, we need to evaluate the limit expression and expose
        // it to the traverse stage by inserting a NLJ on top of it:
        //   nlj [] [inputSlot]
        //     outer
        //        <main-tree>
        //     inner
        //          nlj [] [limitSlot]
        //             outer
        //                  <limit-tree>
        //             inner
        //                  traverse
        //                   from
        //                        project inputArraySlot = {let inputRef = inputSlot in …}
        //                        limit 1
        //                        coscan
        //                    in
        //                      ...
        //
        // As the main evaluation tree is now the outer branch of a top NLJ, we use the limit 1 /
        // coscan subtree for the 'from' branch of the traverse stage.
        auto inputArrayEvalStage = expr->hasLimit() ? makeLimitCoScanStage(_context->planNodeId)
                                                    : _context->extractCurrentEvalStage();
        auto projectInputArray = makeProject(std::move(inputArrayEvalStage),
                                             _context->planNodeId,
                                             inputArraySlot,
                                             std::move(inputArray));

        auto inputIsNotNullish = makeNot(generateNullOrMissing(inputArrayVariable));
        auto inputIsNotNullishSlot = _context->state.slotId();
        auto fromBranch = makeProject(std::move(projectInputArray),
                                      _context->planNodeId,
                                      inputIsNotNullishSlot,
                                      std::move(inputIsNotNullish));

        // Construct 'in' branch of traverse stage. SBE tree stored in 'inBranch' variable looks
        // like this:
        //
        // cfilter Variable{inputIsNotNullishSlot}
        // filter filterPredicate
        // filterStage
        //
        // Filter predicate can return non-boolean values. To fix this, we generate expression to
        // coerce it to bool type.
        frameId = _context->state.frameId();
        auto boolFilterPredicate =
            sbe::makeE<sbe::ELocalBind>(frameId,
                                        sbe::makeEs(filterPredicate.extractExpr()),
                                        generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));
        auto filterWithPredicate = makeFilter<false>(
            std::move(filterStage), std::move(boolFilterPredicate), _context->planNodeId);

        // If input array is null or missing, we do not evaluate filter predicate and return EOF.
        auto innerBranch = makeFilter<true>(std::move(filterWithPredicate),
                                            makeVariable(inputIsNotNullishSlot),
                                            _context->planNodeId);

        auto filteredArraySlot = _context->state.slotId();
        // If input array is null or missing, 'in' stage of traverse will return EOF. In this case
        // traverse sets output slot (filteredArraySlot) to Nothing. We replace it with Null to
        // match $filter expression behaviour.
        auto result = makeFunction("fillEmpty",
                                   makeVariable(filteredArraySlot),
                                   makeConstant(sbe::value::TypeTags::Null, 0));

        if (expr->hasLimit()) {
            // To support $filter queries that have a limit, we create a finalExpr that we pass to
            // the traverse stage. The finalExpr is used as an early checkout condition. As each
            // element of the array is traversed, the finalExpr evaluates the size of the output
            // array and returns early if this size is equal to our limit arg. For $filter queries
            // without a limit arg, the finalExpr is null and therefore the entire array is
            // traversed.
            auto limitSlot = _context->state.slotId();
            auto checkLimitIsNullOrPositiveInt32 = makeLocalBind(
                _context->state.frameIdGenerator,
                [&](sbe::EVariable outerSlotRef) {
                    auto innerLocalBind = makeLocalBind(
                        _context->state.frameIdGenerator,
                        [&](sbe::EVariable convertedFieldRef) {
                            return sbe::makeE<sbe::EIf>(
                                makeFunction("exists", convertedFieldRef.clone()),
                                sbe::makeE<sbe::EIf>(
                                    generatePositiveCheck(convertedFieldRef),
                                    convertedFieldRef.clone(),
                                    makeFail(327392, "'$filter.limit' must be greater than 0")),
                                makeFail(327391, "'$filter.limit' must evaluate to an integer"));
                        },
                        sbe::makeE<sbe::ENumericConvert>(outerSlotRef.clone(),
                                                         sbe::value::TypeTags::NumberInt32));

                    return sbe::makeE<sbe::EIf>(generateNullOrMissing(outerSlotRef),
                                                makeConstant(sbe::value::TypeTags::Null, 0),
                                                std::move(innerLocalBind));
                },
                limitExpr.extractExpr()->clone());

            auto projectLimitStage = makeProject(std::move(limitStage),
                                                 _context->planNodeId,
                                                 limitSlot,
                                                 std::move(checkLimitIsNullOrPositiveInt32));
            auto getArraySizeExpr = makeFunction("getArraySize", makeVariable(filteredArraySlot));
            auto finalExpr = makeBinaryOp(
                sbe::EPrimBinary::greaterEq, std::move(getArraySizeExpr), makeVariable(limitSlot));

            auto traverseStage = makeTraverse(std::move(fromBranch),
                                              std::move(innerBranch),
                                              inputArraySlot /* inField */,
                                              filteredArraySlot /* outField */,
                                              inputArraySlot /* outFieldInner */,
                                              nullptr /* foldExpr */,
                                              std::move(finalExpr),
                                              _context->planNodeId,
                                              1 /* nestedArraysDepth */,
                                              _context->getLexicalEnvironment());

            // TODO: SERVER-60849 Remove NLJs from traverse stage for $filter.limit
            auto loopJoinStage = makeLoopJoin(std::move(projectLimitStage),
                                              std::move(traverseStage),
                                              _context->planNodeId,
                                              _context->getLexicalEnvironment());

            loopJoinStage =
                makeLoopJoin(_context->extractCurrentEvalStage(),
                             std::move(loopJoinStage),  // NOLINT(bugprone-use-after-move)
                             _context->planNodeId,
                             _context->getLexicalEnvironment());

            _context->pushExpr(std::move(result), std::move(loopJoinStage));
            return;
        }

        // Construct traverse stage with the following slots:
        // * inputArraySlot - slot containing input array of $filter expression
        // * filteredArraySlot - slot containing the array with items on which filter predicate has
        //   evaluated to true
        // * inputArraySlot - slot where 'in' branch of traverse stage stores current array
        //   element if it satisfies the filter predicate
        auto traverseStage = makeTraverse(std::move(fromBranch),
                                          std::move(innerBranch),
                                          inputArraySlot /* inField */,
                                          filteredArraySlot /* outField */,
                                          inputArraySlot /* outFieldInner */,
                                          nullptr /* foldExpr */,
                                          nullptr /* finalExpr */,
                                          _context->planNodeId,
                                          1 /* nestedArraysDepth */,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(std::move(result), std::move(traverseStage));
    }
    void visit(const ExpressionFloor* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto floorExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903704},
                                                 "$floor only supports numeric types")},
            makeFunction("floor", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(floorExpr)));
    }
    void visit(const ExpressionIfNull* expr) final {
        auto numChildren = expr->getChildren().size();
        invariant(numChildren >= 2);

        std::vector<EvalExprStagePair> branches;
        branches.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            auto [expr, stage] = _context->popFrame();
            branches.emplace_back(std::move(expr), std::move(stage));
        }
        std::reverse(branches.begin(), branches.end());

        // Prepare to create limit-1/union with N branches (where N is the number of operands). Each
        // branch will be evaluated from left to right until one of the branches produces a value.
        auto branchFn = [](EvalExpr evalExpr,
                           EvalStage stage,
                           PlanNodeId planNodeId,
                           sbe::value::SlotIdGenerator* slotIdGenerator) {
            auto [slot, projectStage] =
                projectEvalExpr(std::move(evalExpr), std::move(stage), planNodeId, slotIdGenerator);

            // Create a FilterStage for each branch (except the last one). If a branch's filter
            // condition is true, it will "short-circuit" the evaluation process. For ifNull,
            // short-circuiting should happen if the current variable is not null or missing.
            auto filterExpr = makeNot(generateNullOrMissing(slot));
            auto filterStage =
                makeFilter<false>(std::move(projectStage), std::move(filterExpr), planNodeId);

            // Set the current expression as the output to be returned if short-circuiting occurs.
            return std::make_pair(slot, std::move(filterStage));
        };

        auto [resultExpr, opStage] = generateSingleResultUnion(
            std::move(branches), branchFn, _context->planNodeId, _context->state.slotIdGenerator);

        auto loopJoinStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(opStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(std::move(resultExpr), std::move(loopJoinStage));
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
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto exprIsNum =
            sbe::makeE<sbe::EIf>(makeFunction("exists", inputRef.clone()),
                                 makeFunction("isNumber", inputRef.clone()),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                            sbe::value::bitcastFrom<bool>(false)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprIsNum)));
    }
    void visit(const ExpressionLet* expr) final {
        // The evaluated result of the $let is the evaluated result of its "in" field, which is
        // already on top of the stack. The "infix" visitor has already popped the variable
        // initializers off the expression stack.
        _context->ensureArity(1);

        // We should have bound all the variables from this $let expression.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();
        invariant(currentFrame.variablesToBind.empty());

        // Pop the lexical frame for this $let and remove all its bindings, which are now out of
        // scope.
        auto it = _context->environment.begin();
        while (it != _context->environment.end()) {
            if (currentFrame.slotsForLetVariables.count(it->second)) {
                it = _context->environment.erase(it);
            } else {
                ++it;
            }
        }
        _context->varsFrameStack.pop();

        // Note that there is no need to remove SlotId bindings from the the _context's environment.
        // The AST parser already enforces scope rules.
    }
    void visit(const ExpressionLn* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto lnExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903705},
                                                 "$ln only supports numeric types")},
            // Note: In MQL, $ln on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            CaseValuePair{generateNaNCheck(inputRef),
                          sbe::makeE<sbe::ENumericConvert>(inputRef.clone(),
                                                           sbe::value::TypeTags::NumberDouble)},
            CaseValuePair{generateNonPositiveCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903706},
                                                 "$ln's argument must be a positive number")},
            makeFunction("ln", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(lnExpr)));
    }
    void visit(const ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionLog10* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto log10Expr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903707},
                                                 "$log10 only supports numeric types")},
            // Note: In MQL, $log10 on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            CaseValuePair{generateNaNCheck(inputRef),
                          sbe::makeE<sbe::ENumericConvert>(inputRef.clone(),
                                                           sbe::value::TypeTags::NumberDouble)},
            CaseValuePair{generateNonPositiveCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903708},
                                                 "$log10's argument must be a positive number")},
            makeFunction("log10", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(log10Expr)));
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
        auto frameId = _context->state.frameId();
        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();
        auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
        sbe::EVariable lhsVar{frameId, 0};
        sbe::EVariable rhsVar{frameId, 1};

        // If the rhs is a small integral double, convert it to int32 to match $mod MQL semantics.
        auto numericConvert32 =
            sbe::makeE<sbe::ENumericConvert>(rhsVar.clone(), sbe::value::TypeTags::NumberInt32);
        auto rhsExpr = buildMultiBranchConditional(
            CaseValuePair{
                makeBinaryOp(
                    sbe::EPrimBinary::logicAnd,
                    makeFunction("typeMatch",
                                 rhsVar.clone(),
                                 makeConstant(sbe::value::TypeTags::NumberInt64,
                                              sbe::value::bitcastFrom<int64_t>(getBSONTypeMask(
                                                  sbe::value::TypeTags::NumberDouble)))),
                    makeNot(
                        makeFunction("typeMatch",
                                     lhsVar.clone(),
                                     makeConstant(sbe::value::TypeTags::NumberInt64,
                                                  sbe::value::bitcastFrom<int64_t>(getBSONTypeMask(
                                                      sbe::value::TypeTags::NumberDouble)))))),
                makeFunction("fillEmpty", std::move(numericConvert32), rhsVar.clone())},
            rhsVar.clone());

        auto modExpr = buildMultiBranchConditional(
            CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                       generateNullOrMissing(lhsVar),
                                       generateNullOrMissing(rhsVar)),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                       generateNonNumericCheck(lhsVar),
                                       generateNonNumericCheck(rhsVar)),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154000},
                                                 "$mod only supports numeric types")},
            makeFunction("mod", lhsVar.clone(), std::move(rhsExpr)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(modExpr)));
    }
    void visit(const ExpressionMultiply* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Return multiplicative identity if the $multiply expression has no operands.
        if (arity == 0) {
            _context->pushExpr(makeConstant(sbe::value::TypeTags::NumberInt32, 1));
            return;
        }

        auto frameId = _context->state.frameId();
        sbe::EExpression::Vector binds;
        sbe::EExpression::Vector variables;
        sbe::EExpression::Vector checkExprsNull;
        sbe::EExpression::Vector checkExprsNumber;
        binds.reserve(arity);
        variables.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNumber.reserve(arity);
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            binds.push_back(_context->popExpr());
            sbe::EVariable currentVariable{frameId, slot};
            variables.push_back(currentVariable.clone());

            checkExprsNull.push_back(generateNullOrMissing(currentVariable));
            checkExprsNumber.push_back(makeFunction("isNumber", currentVariable.clone()));
        }

        // At this point 'binds' vector contains arguments of $multiply expression in the reversed
        // order. We need to reverse it back to perform multiplication in the right order below.
        // Multiplication in different order can lead to different result because of accumulated
        // precision errors from floating point types.
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = sbe::EExpression::Vector::iterator;
        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNull.end()),
            std::move(checkExprsNull.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });

        auto checkNumberAllArguments = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNumber.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNumber.end()),
            std::move(checkExprsNumber.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicAnd, std::move(acc), std::move(ex));
            });

        auto multiplication = std::accumulate(
            std::move_iterator<iter_t>(variables.begin() + 1),
            std::move_iterator<iter_t>(variables.end()),
            std::move(variables.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::mul, std::move(acc), std::move(ex));
            });

        auto multiplyExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkNumberAllArguments), std::move(multiplication)},
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073102},
                                   "only numbers are allowed in an $multiply expression"));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(multiplyExpr)));
    }
    void visit(const ExpressionNot* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());

        auto notExpr = makeNot(generateCoerceToBoolExpression({frameId, 0}));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(notExpr)));
    }
    void visit(const ExpressionObject* expr) final {
        auto&& childExprs = expr->getChildExpressions();
        size_t childSize = childExprs.size();
        _context->ensureArity(childSize);

        // The expression argument for 'newObj' must be a sequence of a field name constant
        // expression and an expression for the value. So, we need 2 * childExprs.size() elements in
        // the expression vector.
        sbe::EExpression::Vector exprs(childSize * 2);
        size_t i = exprs.size();
        for (auto rit = childExprs.rbegin(); rit != childExprs.rend(); ++rit) {
            exprs[--i] = _context->popExpr();
            exprs[--i] = makeConstant(rit->first);
        }

        auto fieldSlot{_context->state.slotIdGenerator->generate()};
        auto stage = makeProject(_context->extractCurrentEvalStage(),
                                 _context->planNodeId,
                                 fieldSlot,
                                 sbe::makeE<sbe::EFunction>("newObj"_sd, std::move(exprs)));

        _context->pushExpr(fieldSlot, std::move(stage));
    }
    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(const ExpressionPow* expr) final {
        unsupportedExpression("$pow");
    }
    void visit(const ExpressionRange* expr) final {
        auto outerFrameId = _context->state.frameId();
        auto innerFrameId = _context->state.frameId();

        sbe::EVariable startRef(outerFrameId, 0);
        sbe::EVariable endRef(outerFrameId, 1);
        sbe::EVariable stepRef(outerFrameId, 2);

        sbe::EVariable convertedStartRef(innerFrameId, 0);
        sbe::EVariable convertedEndRef(innerFrameId, 1);
        sbe::EVariable convertedStepRef(innerFrameId, 2);

        auto step = expr->getChildren().size() == 3
            ? _context->popExpr()
            : sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 1);
        auto end = _context->popExpr();
        auto start = _context->popExpr();

        auto rangeExpr = sbe::makeE<sbe::ELocalBind>(
            outerFrameId,
            sbe::makeEs(std::move(start), std::move(end), std::move(step)),
            buildMultiBranchConditional(
                CaseValuePair{
                    generateNonNumericCheck(startRef),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154300},
                                           "$range only supports numeric types for start")},
                CaseValuePair{generateNonNumericCheck(endRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154301},
                                                     "$range only supports numeric types for end")},
                CaseValuePair{
                    generateNonNumericCheck(stepRef),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154302},
                                           "$range only supports numeric types for step")},
                sbe::makeE<sbe::ELocalBind>(
                    innerFrameId,
                    sbe::makeEs(sbe::makeE<sbe::ENumericConvert>(startRef.clone(),
                                                                 sbe::value::TypeTags::NumberInt32),
                                sbe::makeE<sbe::ENumericConvert>(endRef.clone(),
                                                                 sbe::value::TypeTags::NumberInt32),
                                sbe::makeE<sbe::ENumericConvert>(
                                    stepRef.clone(), sbe::value::TypeTags::NumberInt32)),
                    buildMultiBranchConditional(
                        CaseValuePair{
                            makeNot(makeFunction("exists", convertedStartRef.clone())),
                            sbe::makeE<sbe::EFail>(
                                ErrorCodes::Error{5154303},
                                "$range start argument cannot be represented as a 32-bit integer")},
                        CaseValuePair{
                            makeNot(makeFunction("exists", convertedEndRef.clone())),
                            sbe::makeE<sbe::EFail>(
                                ErrorCodes::Error{5154304},
                                "$range end argument cannot be represented as a 32-bit integer")},
                        CaseValuePair{
                            makeNot(makeFunction("exists", convertedStepRef.clone())),
                            sbe::makeE<sbe::EFail>(
                                ErrorCodes::Error{5154305},
                                "$range step argument cannot be represented as a 32-bit integer")},
                        CaseValuePair{
                            makeBinaryOp(
                                sbe::EPrimBinary::eq,
                                convertedStepRef.clone(),
                                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 0)),
                            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154306},
                                                   "$range requires a non-zero step value")},
                        makeFunction("newArrayFromRange",
                                     convertedStartRef.clone(),
                                     convertedEndRef.clone(),
                                     convertedStepRef.clone())))));

        _context->pushExpr(std::move(rangeExpr));
    }
    void visit(const ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(const ExpressionReplaceOne* expr) final {
        auto frameId = _context->state.frameId();

        auto replacement = _context->popExpr();
        auto find = _context->popExpr();
        auto input = _context->popExpr();

        sbe::EVariable inputRef(frameId, 0);
        sbe::EVariable findRef(frameId, 1);
        sbe::EVariable replacementRef(frameId, 2);
        sbe::EVariable inputNullOrMissingRef(frameId, 3);
        sbe::EVariable findNullOrMissingRef(frameId, 4);
        sbe::EVariable replacementNullOrMissingRef(frameId, 5);

        auto binds = sbe::makeEs(std::move(input),
                                 std::move(find),
                                 std::move(replacement),
                                 generateNullOrMissing(inputRef),
                                 generateNullOrMissing(findRef),
                                 generateNullOrMissing(replacementRef));

        auto generateValidateParameter = [](const sbe::EVariable& paramRef,
                                            const sbe::EVariable& paramMissingRef,
                                            const std::string& paramName) {
            return makeBinaryOp(sbe::EPrimBinary::logicOr,
                                makeBinaryOp(sbe::EPrimBinary::logicOr,
                                             paramMissingRef.clone(),
                                             makeFunction("isString", paramRef.clone())),
                                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154400},
                                                       str::stream()
                                                           << "$replaceOne requires that '"
                                                           << paramName << "' be a string"));
        };

        auto inputIsStringOrFail =
            generateValidateParameter(inputRef, inputNullOrMissingRef, "input");
        auto findIsStringOrFail = generateValidateParameter(findRef, findNullOrMissingRef, "find");
        auto replacementIsStringOrFail =
            generateValidateParameter(replacementRef, replacementNullOrMissingRef, "replacement");

        auto checkNullExpr = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                          makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                       inputNullOrMissingRef.clone(),
                                                       findNullOrMissingRef.clone()),
                                          replacementNullOrMissingRef.clone());

        // Order here is important because we want to preserve the precedence of failures in MQL.
        auto isNullExpr = makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                       makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                                    makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                                                 std::move(inputIsStringOrFail),
                                                                 std::move(findIsStringOrFail)),
                                                    std::move(replacementIsStringOrFail)),
                                       std::move(checkNullExpr));

        // Check if find string is empty, and if so return the the concatenation of the replacement
        // string and the input string, otherwise replace the first occurrence of the find string.
        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        auto isEmptyFindStr = makeBinaryOp(sbe::EPrimBinary::eq,
                                           findRef.clone(),
                                           sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                                           _context->state.data->env);

        auto replaceOrReturnInputExpr = sbe::makeE<sbe::EIf>(
            std::move(isEmptyFindStr),
            makeFunction("concat", replacementRef.clone(), inputRef.clone()),
            makeFunction("replaceOne", inputRef.clone(), findRef.clone(), replacementRef.clone()));

        auto replaceOneExpr =
            sbe::makeE<sbe::EIf>(std::move(isNullExpr),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                                 std::move(replaceOrReturnInputExpr));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(replaceOneExpr)));
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
            _context->pushExpr(makeConstant(emptySetTag, emptySetValue));
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
            _context->pushExpr(makeConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Union);
    }

    void visit(const ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionReverseArray* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef{frameId, 0};

        auto argumentIsNotArray = makeNot(makeFunction("isArray", inputRef.clone()));
        auto exprRevArr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          makeConstant(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(argumentIsNotArray),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154901},
                                                 "$reverseArray argument must be an array")},
            makeFunction("reverseArray", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprRevArr)));
    }

    void visit(const ExpressionSortArray* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef{frameId, 0};

        auto [specTag, specVal] = makeValue(expr->getSortPattern());
        auto specConstant = makeConstant(specTag, specVal);

        auto collatorSlot = _context->state.data->env->getSlotIfExists("collator"_sd);

        auto argumentIsNotArray = makeNot(makeFunction("isArray", inputRef.clone()));
        auto exprSortArr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          makeConstant(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(argumentIsNotArray),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{6096700},
                                                 "$sortArray input argument must be an array")},
            collatorSlot ? makeFunction("sortArray",
                                        inputRef.clone(),
                                        std::move(specConstant),
                                        makeVariable(*collatorSlot))
                         : makeFunction("sortArray", inputRef.clone(), std::move(specConstant)));


        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprSortArr)));
    }

    void visit(const ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionIsArray* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto exprIsArr = makeFillEmptyFalse(makeFunction("isArray", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprIsArr)));
    }
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionRound* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSplit* expr) final {
        auto frameId = _context->state.frameId();
        sbe::EExpression::Vector args;
        sbe::EExpression::Vector binds;
        sbe::EVariable stringExpressionRef(frameId, 0);
        sbe::EVariable delimiterRef(frameId, 1);

        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto delimiter = _context->popExpr();
        auto stringExpression = _context->popExpr();

        // Add stringExpression to arguments.
        binds.push_back(std::move(stringExpression));
        args.push_back(stringExpressionRef.clone());

        // Add delimiter to arguments.
        binds.push_back(std::move(delimiter));
        args.push_back(delimiterRef.clone());

        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        auto [arrayWithEmptyStringTag, arrayWithEmptyStringVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrayWithEmptyStringGuard{arrayWithEmptyStringTag,
                                                         arrayWithEmptyStringVal};
        auto arrayWithEmptyStringView = sbe::value::getArrayView(arrayWithEmptyStringVal);
        arrayWithEmptyStringView->push_back(emptyStrTag, emptyStrVal);
        arrayWithEmptyStringGuard.reset();

        auto generateIsEmptyString = [this, emptyStrTag = emptyStrTag, emptyStrVal = emptyStrVal](
                                         const sbe::EVariable& var) {
            return makeBinaryOp(sbe::EPrimBinary::eq,
                                var.clone(),
                                sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                                _context->state.data->env);
        };

        auto checkIsNullOrMissing = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                 generateNullOrMissing(stringExpressionRef),
                                                 generateNullOrMissing(delimiterRef));

        // In order to maintain MQL semantics, first check both the string expression
        // (first agument), and delimiter string (second argument) for null, undefined, or
        // missing, and if either is nullish make the entire expression return null. Only
        // then make further validity checks against the input. Fail if the delimiter is an empty
        // string. Return [""] if the string expression is an empty string.
        auto totalSplitFunc = buildMultiBranchConditional(
            CaseValuePair{std::move(checkIsNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(stringExpressionRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5155402},
                              str::stream() << "$split string expression must be a string")},
            CaseValuePair{
                generateNonStringCheck(delimiterRef),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5155400},
                                       str::stream() << "$split delimiter must be a string")},
            CaseValuePair{generateIsEmptyString(delimiterRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5155401},
                              str::stream() << "$split delimiter must not be an empty string")},
            sbe::makeE<sbe::EIf>(
                generateIsEmptyString(stringExpressionRef),
                sbe::makeE<sbe::EConstant>(arrayWithEmptyStringTag, arrayWithEmptyStringVal),
                sbe::makeE<sbe::EFunction>("split", std::move(args))));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(totalSplitFunc)));
    }
    void visit(const ExpressionSqrt* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto lnExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903709},
                                                 "$sqrt only supports numeric types")},
            CaseValuePair{
                generateNegativeCheck(inputRef),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903710},
                                       "$sqrt's argument must be greater than or equal to 0")},
            makeFunction("sqrt", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(lnExpr)));
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
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionTestApiVersion* expr) final {
        _context->pushExpr(
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(1)));
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
            "acosh", DoubleBound(1.0, true), DoubleBound::plusInfinity());
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
        generateDayOfExpression("dayOfMonth", expr);
    }
    void visit(const ExpressionDayOfWeek* expr) final {
        generateDayOfExpression("dayOfWeek", expr);
    }
    void visit(const ExpressionDayOfYear* expr) final {
        generateDayOfExpression("dayOfYear", expr);
    }
    void visit(const ExpressionHour* expr) final {
        unsupportedExpression("$hour");
    }
    void visit(const ExpressionMillisecond* expr) final {
        unsupportedExpression("$millisecond");
    }
    void visit(const ExpressionMinute* expr) final {
        unsupportedExpression("$minute");
    }
    void visit(const ExpressionMonth* expr) final {
        unsupportedExpression("$month");
    }
    void visit(const ExpressionSecond* expr) final {
        unsupportedExpression("$second");
    }
    void visit(const ExpressionWeek* expr) final {
        unsupportedExpression("$week");
    }
    void visit(const ExpressionIsoWeekYear* expr) final {
        unsupportedExpression("$isoWeekYear");
    }
    void visit(const ExpressionIsoDayOfWeek* expr) final {
        unsupportedExpression("$isoDayOfWeek");
    }
    void visit(const ExpressionIsoWeek* expr) final {
        unsupportedExpression("$isoWeek");
    }
    void visit(const ExpressionYear* expr) final {
        unsupportedExpression("$year");
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

        auto tsSecondExpr = makeLocalBind(
            _context->state.frameIdGenerator,
            [&](sbe::EVariable operand) {
                // The branching is as follows,
                // *  if the input value is null or missing, then return null
                // *  if the input value is not timestamp, then throw an exception
                // *  else, create a builtin function with the name 'tsSecond'
                return buildMultiBranchConditional(
                    CaseValuePair{generateNullOrMissing(operand),
                                  makeConstant(sbe::value::TypeTags::Null, 0)},
                    CaseValuePair{generateNonTimestampCheck(operand),
                                  sbe::makeE<sbe::EFail>(
                                      ErrorCodes::Error{5687400},
                                      str::stream() << expr->getOpName()
                                                    << " expects argument of type timestamp")},
                    makeFunction("tsSecond", operand.clone()));
            },
            _context->popExpr());
        _context->pushExpr(std::move(tsSecondExpr));
    }

    void visit(const ExpressionTsIncrement* expr) final {
        _context->ensureArity(1);

        auto tsIncrementExpr = makeLocalBind(
            _context->state.frameIdGenerator,
            [&](sbe::EVariable operand) {
                // The branching is as follows,
                // *  if the input value is null or missing, then return null
                // *  if the input value is not timestamp, then throw an exception
                // *  else, create a builtin function with the name 'tsIncrement'
                return buildMultiBranchConditional(
                    CaseValuePair{generateNullOrMissing(operand),
                                  makeConstant(sbe::value::TypeTags::Null, 0)},
                    CaseValuePair{generateNonTimestampCheck(operand),
                                  sbe::makeE<sbe::EFail>(
                                      ErrorCodes::Error{5687401},
                                      str::stream() << expr->getOpName()
                                                    << " expects argument of type timestamp")},
                    makeFunction("tsIncrement", operand.clone()));
            },
            _context->popExpr());
        _context->pushExpr(std::move(tsIncrementExpr));
    }

private:
    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(const Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

        size_t numChildren = expr->getChildren().size();
        if (numChildren == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value: true
            // and false, respectively.
            auto logicIdentityVal = (logicOp == sbe::EPrimBinary::logicAnd);
            _context->pushExpr(sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(logicIdentityVal)));
            return;
        } else if (numChildren == 1) {
            // No need for short circuiting logic in a singleton $and/$or. Just execute the branch
            // and return its result as a bool.
            auto frameId = _context->state.frameId();
            _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(_context->popExpr()),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0})));

            return;
        }

        std::vector<EvalExprStagePair> branches;
        for (size_t i = 0; i < numChildren; ++i) {
            auto [expr, stage] = _context->popFrame();

            auto frameId = _context->state.frameId();
            auto coercedExpr = sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(expr.extractExpr()),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));

            branches.emplace_back(std::move(coercedExpr), std::move(stage));
        }
        std::reverse(branches.begin(), branches.end());

        auto [resultExpr, opStage] =
            generateShortCircuitingLogicalOp(logicOp,
                                             std::move(branches),
                                             _context->planNodeId,
                                             _context->state.slotIdGenerator,
                                             BooleanStateHelper{});

        auto loopJoinStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(opStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(std::move(resultExpr), std::move(loopJoinStage));
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(const Expression* expr) {
        // The default case is always the last child in the ExpressionSwitch. If it is unspecified
        // in the user's query, it is a nullptr. In ExpressionCond, the last child is the "else"
        // branch, and it is guaranteed not to be nullptr.
        if (expr->getChildren().back() == nullptr) {
            _context->pushExpr(
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4934200},
                                       "$switch could not find a matching branch for an "
                                       "input, and no default was specified."));
        }

        auto numChildren = expr->getChildren().size();
        std::vector<EvalExprStagePair> branches;
        branches.reserve(numChildren);
        for (size_t i = 0; i < numChildren / 2 + 1; ++i) {
            auto [expr, stage] = _context->popFrame();

            if (i == 0) {
                // The first branch is the default value.
                branches.emplace_back(std::move(expr), std::move(stage));
                continue;
            }

            auto [thenSlot, thenStage] = projectEvalExpr(std::move(expr),
                                                         std::move(stage),
                                                         _context->planNodeId,
                                                         _context->state.slotIdGenerator);

            // Construct a FilterStage tree that will EOF if "case" expression returns false. In
            // this case inner branch of loop join with "then" expression will never be executed.
            std::tie(expr, stage) = _context->popFrame();
            auto frameId = _context->state.frameId();
            auto coercedExpr = sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(expr.extractExpr()),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));
            auto conditionStage =
                makeFilter<false>(std::move(stage), std::move(coercedExpr), _context->planNodeId);

            // Create a LoopJoinStage that will evaluate its outer child exactly once. If outer
            // child produces non-EOF result (i.e. condition evaluated to true), inner child is
            // executed. Inner child simply bounds result of "then" expression to a slot.
            auto loopJoinStage = makeLoopJoin(std::move(conditionStage),
                                              std::move(thenStage),
                                              _context->planNodeId,
                                              _context->getLexicalEnvironment());

            branches.emplace_back(thenSlot, std::move(loopJoinStage));
        }

        std::reverse(branches.begin(), branches.end());

        auto [resultExpr, resultStage] = generateSingleResultUnion(
            std::move(branches), {}, _context->planNodeId, _context->state.slotIdGenerator);

        auto loopJoinStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(resultStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(std::move(resultExpr), std::move(loopJoinStage));
    }

    void generateDayOfExpression(StringData exprName, const Expression* expr) {
        auto frameId = _context->state.frameId();
        sbe::EExpression::Vector args;
        sbe::EExpression::Vector binds;
        sbe::EVariable dateRef(frameId, 0);
        sbe::EVariable timezoneRef(frameId, 1);

        auto children = expr->getChildren();
        invariant(children.size() == 2);
        _context->ensureArity(children[1] ? 2 : 1);

        auto timezone = [&]() {
            if (children[1]) {
                return _context->popExpr();
            }
            auto [utcTag, utcVal] = sbe::value::makeNewString("UTC");
            return sbe::makeE<sbe::EConstant>(utcTag, utcVal);
        }();
        auto date = _context->popExpr();

        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        args.push_back(sbe::makeE<sbe::EVariable>(timeZoneDBSlot));

        // Add date to arguments.
        binds.push_back(std::move(date));
        args.push_back(dateRef.clone());

        // Add timezone to arguments.
        binds.push_back(std::move(timezone));
        args.push_back(timezoneRef.clone());

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDayOfFunc = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(timezoneRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(timezoneRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4998200},
                                                 str::stream() << "$" << exprName.toString()
                                                               << " timezone must be a string")},
            CaseValuePair{
                makeNot(makeFunction(
                    "isTimezone", sbe::makeE<sbe::EVariable>(timeZoneDBSlot), timezoneRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4998201},
                                       str::stream() << "$" << exprName.toString()
                                                     << " timezone must be a valid timezone")},
            CaseValuePair{generateNullOrMissing(dateRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{
                makeNot(
                    makeFunction("typeMatch",
                                 dateRef.clone(),
                                 makeConstant(sbe::value::TypeTags::NumberInt64,
                                              sbe::value::bitcastFrom<int64_t>(dateTypeMask())))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4998202},
                                       str::stream() << "$" << exprName.toString()
                                                     << " date must have a format of a date")},
            sbe::makeE<sbe::EFunction>(exprName.toString(), std::move(args)));
        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(totalDayOfFunc)));
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

    /**
     * Creates a CaseValuePair such that Null value is returned if a value of variable denoted by
     * 'variable' is null or missing.
     */
    static CaseValuePair generateReturnNullIfNullOrMissing(const sbe::EVariable& variable) {
        return {generateNullOrMissing(variable), makeConstant(sbe::value::TypeTags::Null, 0)};
    }

    /**
     * Creates a boolean expression to check if 'variable' is equal to string 'string'.
     */
    static std::unique_ptr<sbe::EExpression> generateIsEqualToStringCheck(
        const sbe::EVariable& variable, StringData string) {
        return sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                            makeFunction("isString", variable.clone()),
                                            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::eq,
                                                                         variable.clone(),
                                                                         makeConstant(string)));
    }

    /**
     * Shared expression building logic for trignometric expressions to make sure the operand
     * is numeric and is not null.
     */
    void generateTrigonometricExpression(StringData exprName) {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto genericTrignomentricExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(makeFunction("isNumber", inputRef.clone()),
                                 makeFunction(exprName.toString(), inputRef.clone()),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995501},
                                                        str::stream()
                                                            << "$" << exprName.toString()
                                                            << " supports only numeric types")));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(binds), std::move(genericTrignomentricExpr)));
    }

    /**
     * Shared expression building logic for binary trigonometric expressions to make sure the
     * operands are numeric and are not null.
     */
    void generateTrigonometricExpressionBinary(StringData exprName) {
        _context->ensureArity(2);

        auto genericTrignomentricExpr = makeLocalBind(
            _context->state.frameIdGenerator,
            [&](sbe::EVariable lhs, sbe::EVariable rhs) {
                return buildMultiBranchConditional(
                    CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                               generateNullOrMissing(lhs),
                                               generateNullOrMissing(rhs)),
                                  makeConstant(sbe::value::TypeTags::Null, 0)},
                    CaseValuePair{
                        makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                     makeFunction("isNumber", lhs.clone()),
                                     makeFunction("isNumber", rhs.clone())),
                        makeFunction(exprName.toString(), lhs.clone(), rhs.clone()),
                    },
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{5688500},
                                           str::stream() << "$" << exprName
                                                         << " supports only numeric types"));
            },
            _context->popExpr(),
            _context->popExpr());
        _context->pushExpr(std::move(genericTrignomentricExpr));
    }

    /**
     * Shared expression building logic for trignometric expressions with bounds for the valid
     * values of the argument.
     */
    void generateTrigonometricExpressionWithBounds(StringData exprName,
                                                   const DoubleBound& lowerBound,
                                                   const DoubleBound& upperBound) {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        sbe::EPrimBinary::Op lowerCmp =
            lowerBound.inclusive ? sbe::EPrimBinary::greaterEq : sbe::EPrimBinary::greater;
        sbe::EPrimBinary::Op upperCmp =
            upperBound.inclusive ? sbe::EPrimBinary::lessEq : sbe::EPrimBinary::less;
        auto checkBounds = makeBinaryOp(
            sbe::EPrimBinary::logicAnd,
            makeBinaryOp(
                lowerCmp,
                inputRef.clone(),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble,
                                           sbe::value::bitcastFrom<double>(lowerBound.bound))),
            makeBinaryOp(
                upperCmp,
                inputRef.clone(),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble,
                                           sbe::value::bitcastFrom<double>(upperBound.bound))));

        auto genericTrignomentricExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(
                makeNot(makeFunction("isNumber", inputRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995502},
                                       str::stream() << "$" << exprName.toString()
                                                     << " supports only numeric types"),
                sbe::makeE<sbe::EIf>(
                    std::move(checkBounds),
                    makeFunction(exprName.toString(), inputRef.clone()),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995503},
                                           str::stream() << "Cannot apply $" << exprName.toString()
                                                         << ", value must be in "
                                                         << lowerBound.printLowerBound() << ", "
                                                         << upperBound.printUpperBound()))));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(binds), std::move(genericTrignomentricExpr)));
    }

    /*
     * Generates an EExpression that returns an index for $indexOfBytes or $indexOfCP.
     */
    void visitIndexOfFunction(const Expression* expr,
                              ExpressionVisitorContext* _context,
                              const std::string& indexOfFunction) {
        auto frameId = _context->state.frameId();
        auto children = expr->getChildren();
        auto operandSize = children.size() <= 3 ? 3 : 4;
        sbe::EExpression::Vector operands(operandSize);
        sbe::EExpression::Vector bindings;
        sbe::EVariable strRef(frameId, 0);
        sbe::EVariable substrRef(frameId, 1);
        boost::optional<sbe::EVariable> startIndexRef;
        boost::optional<sbe::EVariable> endIndexRef;

        // Get arguments from stack.
        switch (children.size()) {
            case 2: {
                operands[2] = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                         sbe::value::bitcastFrom<int64_t>(0));
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                break;
            }
            case 3: {
                operands[2] = _context->popExpr();
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                break;
            }
            case 4: {
                operands[3] = _context->popExpr();
                operands[2] = _context->popExpr();
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                endIndexRef.emplace(frameId, 3);
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        // Add string and substring operands.
        bindings.push_back(strRef.clone());
        bindings.push_back(substrRef.clone());

        // Add start index operand.
        if (startIndexRef) {
            auto numericConvert64 = sbe::makeE<sbe::ENumericConvert>(
                startIndexRef->clone(), sbe::value::TypeTags::NumberInt64);
            auto checkValidStartIndex = buildMultiBranchConditional(
                CaseValuePair{generateNullishOrNotRepresentableInt32Check(*startIndexRef),
                              sbe::makeE<sbe::EFail>(
                                  ErrorCodes::Error{5075303},
                                  str::stream() << "$" << indexOfFunction
                                                << " start index must resolve to a number")},
                CaseValuePair{generateNegativeCheck(*startIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075304},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " start index must be positive")},
                std::move(numericConvert64));
            bindings.push_back(std::move(checkValidStartIndex));
        }
        // Add end index operand.
        if (endIndexRef) {
            auto numericConvert64 = sbe::makeE<sbe::ENumericConvert>(
                endIndexRef->clone(), sbe::value::TypeTags::NumberInt64);
            auto checkValidEndIndex = buildMultiBranchConditional(
                CaseValuePair{generateNullishOrNotRepresentableInt32Check(*endIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075305},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " end index must resolve to a number")},
                CaseValuePair{generateNegativeCheck(*endIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075306},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " end index must be positive")},
                std::move(numericConvert64));
            bindings.push_back(std::move(checkValidEndIndex));
        }

        // Check if string or substring are null or missing before calling indexOfFunction.
        auto checkStringNullOrMissing = generateNullOrMissing(frameId, 0);
        auto checkSubstringNullOrMissing = generateNullOrMissing(frameId, 1);
        auto exprIndexOfFunction = sbe::makeE<sbe::EFunction>(indexOfFunction, std::move(bindings));

        auto totalExprIndexOfFunction = buildMultiBranchConditional(
            CaseValuePair{std::move(checkStringNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(strRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5075300},
                              str::stream() << "$" << indexOfFunction
                                            << " string must resolve to a string or null")},
            CaseValuePair{std::move(checkSubstringNullOrMissing),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075301},
                                                 str::stream()
                                                     << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
            CaseValuePair{generateNonStringCheck(substrRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075302},
                                                 str::stream()
                                                     << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
            std::move(exprIndexOfFunction));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(totalExprIndexOfFunction)));
    }

    /**
     * Generic logic for building set expressions: setUnion, setIntersection, etc.
     */
    void generateSetExpression(const Expression* expr, SetOperation setOp) {
        using namespace std::literals;

        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->state.frameId();

        auto generateNotArray = [frameId](const sbe::value::SlotId slotId) {
            sbe::EVariable var{frameId, slotId};
            return makeNot(makeFunction("isArray", var.clone()));
        };

        sbe::EExpression::Vector binds;
        sbe::EExpression::Vector argVars;
        sbe::EExpression::Vector checkExprsNull;
        sbe::EExpression::Vector checkExprsNotArray;
        binds.reserve(arity);
        argVars.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNotArray.reserve(arity);

        auto collatorSlot = _context->state.data->env->getSlotIfExists("collator"_sd);

        auto [operatorName, setFunctionName] = [setOp, collatorSlot]() {
            switch (setOp) {
                case SetOperation::Difference:
                    return std::make_pair("setDifference"_sd,
                                          collatorSlot ? "collSetDifference"_sd
                                                       : "setDifference"_sd);
                case SetOperation::Intersection:
                    return std::make_pair("setIntersection"_sd,
                                          collatorSlot ? "collSetIntersection"_sd
                                                       : "setIntersection"_sd);
                case SetOperation::Union:
                    return std::make_pair("setUnion"_sd,
                                          collatorSlot ? "collSetUnion"_sd : "setUnion"_sd);
                case SetOperation::Equals:
                    return std::make_pair("setEquals"_sd,
                                          collatorSlot ? "collSetEquals"_sd : "setEquals"_sd);
                default:
                    MONGO_UNREACHABLE;
            }
        }();

        if (collatorSlot) {
            argVars.push_back(sbe::makeE<sbe::EVariable>(*collatorSlot));
        }

        for (size_t idx = 0; idx < arity; ++idx) {
            binds.push_back(_context->popExpr());
            argVars.push_back(sbe::makeE<sbe::EVariable>(frameId, idx));

            checkExprsNull.push_back(generateNullOrMissing(frameId, idx));
            checkExprsNotArray.push_back(generateNotArray(idx));
        }
        // Reverse the binds array to preserve the original order of the arguments, since some set
        // operations, such as $setDifference, are not commutative.
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = sbe::EExpression::Vector::iterator;
        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNull.end()),
            std::move(checkExprsNull.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });
        auto checkNotArrayAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNotArray.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNotArray.end()),
            std::move(checkExprsNotArray.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });
        auto setExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkNotArrayAnyArgument),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126900},
                                                 str::stream()
                                                     << "All operands of $" << operatorName
                                                     << " must be arrays.")},
            sbe::makeE<sbe::EFunction>(setFunctionName, std::move(argVars)));
        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(setExpr)));
    }

    /**
     * Shared expression building logic for regex expressions.
     */
    void generateRegexExpression(const ExpressionRegex* expr, StringData exprName) {
        size_t arity = expr->hasOptions() ? 3 : 2;
        _context->ensureArity(arity);

        std::unique_ptr<sbe::EExpression> options =
            expr->hasOptions() ? _context->popExpr() : nullptr;
        auto pattern = _context->popExpr();
        auto input = _context->popExpr();

        // Create top level local bind.
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(input));
        sbe::EVariable inputVar{frameId, 0};

        auto makeError = [exprName](int errorCode, StringData message) {
            return sbe::makeE<sbe::EFail>(ErrorCodes::Error{errorCode},
                                          str::stream() << "$" << exprName.toString() << ": "
                                                        << message.toString());
        };

        auto makeRegexFunctionCall = [&](std::unique_ptr<sbe::EExpression> compiledRegex) {
            return makeLocalBind(
                _context->state.frameIdGenerator,
                [&](sbe::EVariable regexResult) {
                    return sbe::makeE<sbe::EIf>(
                        makeFunction("exists", regexResult.clone()),
                        regexResult.clone(),
                        makeError(5073403,
                                  "error occurred while executing the regular expression"));
                },
                makeFunction(exprName.toString(), std::move(compiledRegex), inputVar.clone()));
        };

        auto regexFunctionResult = [&]() {
            if (auto patternAndOptions = expr->getConstantPatternAndOptions(); patternAndOptions) {
                auto [pattern, options] = *patternAndOptions;
                if (!pattern) {
                    // Pattern is null, just generate null result.
                    return generateRegexNullResponse(exprName);
                }

                // Create the compiled Regex from constant pattern and options.
                auto [regexTag, regexVal] = sbe::value::makeNewPcreRegex(*pattern, options);
                auto compiledRegex = sbe::makeE<sbe::EConstant>(regexTag, regexVal);
                return makeRegexFunctionCall(std::move(compiledRegex));
            }

            // Include pattern and options in the outer local bind.
            sbe::EVariable patternVar{frameId, 1};
            binds.push_back(std::move(pattern));

            boost::optional<sbe::EVariable> optionsVar;
            if (options) {
                binds.push_back(std::move(options));
                optionsVar.emplace(frameId, 2);
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
            auto patternNullBytesCheck = sbe::makeE<sbe::EIf>(
                makeFunction("hasNullBytes", patternVar.clone()),
                makeError(5126602, "regex pattern must not have embedded null bytes"),
                patternVar.clone());
            auto patternArgument = buildMultiBranchConditional(
                CaseValuePair{makeFunction("isString", patternVar.clone()),
                              std::move(patternNullBytesCheck)},
                CaseValuePair{makeFunction("typeMatch",
                                           patternVar.clone(),
                                           makeConstant(sbe::value::TypeTags::NumberInt64,
                                                        sbe::value::bitcastFrom<int64_t>(
                                                            getBSONTypeMask(BSONType::RegEx)))),
                              makeFunction("getRegexPattern", patternVar.clone())},
                makeError(5126601, "regex pattern must have either string or BSON RegEx type"));

            if (!optionsVar) {
                // If no options are passed to the expression, try to extract them from the pattern.
                auto optionsArgument = sbe::makeE<sbe::EIf>(
                    makeFunction("typeMatch",
                                 patternVar.clone(),
                                 makeConstant(sbe::value::TypeTags::NumberInt64,
                                              sbe::value::bitcastFrom<int64_t>(
                                                  getBSONTypeMask(BSONType::RegEx)))),
                    makeFunction("getRegexFlags", patternVar.clone()),
                    makeConstant(""));
                auto compiledRegex = makeFunction(
                    "regexCompile", std::move(patternArgument), std::move(optionsArgument));
                return sbe::makeE<sbe::EIf>(makeFunction("isNull", patternVar.clone()),
                                            generateRegexNullResponse(exprName),
                                            makeRegexFunctionCall(std::move(compiledRegex)));
            }

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
                auto optionsNullBytesCheck = sbe::makeE<sbe::EIf>(
                    makeFunction("hasNullBytes", optionsVar->clone()),
                    makeError(5126604, "regex flags must not have embedded null bytes"),
                    optionsVar->clone());
                auto stringOptions = buildMultiBranchConditional(
                    CaseValuePair{makeFunction("isString", optionsVar->clone()),
                                  std::move(optionsNullBytesCheck)},
                    CaseValuePair{makeFunction("isNull", optionsVar->clone()), makeConstant("")},
                    makeError(5126603, "regex flags must have either string or null type"));

                auto generateIsEmptyString = [](const sbe::EVariable& var) {
                    return makeBinaryOp(sbe::EPrimBinary::eq, var.clone(), makeConstant(""));
                };

                return makeLocalBind(
                    _context->state.frameIdGenerator,
                    [&](sbe::EVariable stringOptions) {
                        auto checkBsonRegexOptions = makeLocalBind(
                            _context->state.frameIdGenerator,
                            [&](sbe::EVariable bsonOptions) {
                                return buildMultiBranchConditional(
                                    CaseValuePair{generateIsEmptyString(stringOptions),
                                                  bsonOptions.clone()},
                                    CaseValuePair{generateIsEmptyString(bsonOptions),
                                                  stringOptions.clone()},
                                    makeError(5126605,
                                              "regex options cannot be specified in both BSON "
                                              "RegEx and 'options' field"));
                            },
                            makeFunction("getRegexFlags", patternVar.clone()));

                        return sbe::makeE<sbe::EIf>(
                            makeFunction("typeMatch",
                                         patternVar.clone(),
                                         makeConstant(sbe::value::TypeTags::NumberInt64,
                                                      sbe::value::bitcastFrom<int64_t>(
                                                          getBSONTypeMask(BSONType::RegEx)))),
                            std::move(checkBsonRegexOptions),
                            stringOptions.clone());
                    },
                    std::move(stringOptions));
            }();

            // If there are options passed to the expression, we construct local bind with options
            // argument because it needs to be validated even when pattern is null.
            return makeLocalBind(
                _context->state.frameIdGenerator,
                [&](sbe::EVariable options) {
                    auto compiledRegex =
                        makeFunction("regexCompile", std::move(patternArgument), options.clone());
                    return sbe::makeE<sbe::EIf>(makeFunction("isNull", patternVar.clone()),
                                                generateRegexNullResponse(exprName),
                                                makeRegexFunctionCall(std::move(compiledRegex)));
                },
                std::move(optionsArgument));
        }();

        auto resultExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputVar), generateRegexNullResponse(exprName)},
            CaseValuePair{generateNonStringCheck(inputVar),
                          makeError(5073401, "input must be of type string")},
            std::move(regexFunctionResult));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(resultExpr)));
    }

    /**
     * Generic logic for building $dateAdd and $dateSubtract expressions.
     */
    void generateDateArithmeticsExpression(const ExpressionDateArithmetics* expr,
                                           const std::string& dateExprName) {
        auto children = expr->getChildren();
        auto arity = children.size();
        invariant(arity == 4);
        _context->ensureArity(children[3] ? 4 : 3);

        auto timezoneExpr = [&]() {
            if (children[3]) {
                return _context->popExpr();
            }
            return makeConstant("UTC");
        }();
        auto amountExpr = _context->popExpr();
        auto unitExpr = _context->popExpr();
        auto startDateExpr = _context->popExpr();

        sbe::EExpression::Vector binds;
        binds.push_back(std::move(startDateExpr));
        binds.push_back(std::move(unitExpr));
        binds.push_back(std::move(amountExpr));
        binds.push_back(std::move(timezoneExpr));

        auto frameId = _context->state.frameId();
        sbe::EVariable startDateRef{frameId, 0};
        sbe::EVariable unitRef{frameId, 1};
        sbe::EVariable origAmountRef{frameId, 2};
        sbe::EVariable tzRef{frameId, 3};
        sbe::EVariable amountRef{frameId, 4};

        auto convertedAmountInt64 = [&]() {
            if (dateExprName == "dateAdd") {
                return sbe::makeE<sbe::ENumericConvert>(origAmountRef.clone(),
                                                        sbe::value::TypeTags::NumberInt64);
            } else if (dateExprName == "dateSubtract") {
                return sbe::makeE<sbe::ENumericConvert>(
                    sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::negate, origAmountRef.clone()),
                    sbe::value::TypeTags::NumberInt64);
            } else {
                MONGO_UNREACHABLE;
            }
        }();
        binds.push_back(std::move(convertedAmountInt64));

        sbe::EExpression::Vector args;
        auto timeZoneDBSlot = _context->state.data->env->getSlot("timeZoneDB"_sd);
        args.push_back(sbe::makeE<sbe::EVariable>(timeZoneDBSlot));
        args.push_back(startDateRef.clone());
        args.push_back(unitRef.clone());
        args.push_back(amountRef.clone());
        args.push_back(tzRef.clone());

        sbe::EExpression::Vector checkNullArg;
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            checkNullArg.push_back(generateNullOrMissing(frameId, slot));
        }

        using iter_t = sbe::EExpression::Vector::iterator;
        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkNullArg.begin() + 1),
            std::move_iterator<iter_t>(checkNullArg.end()),
            std::move(checkNullArg.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });

        auto dateAddExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          makeConstant(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(tzRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5166601},
                              str::stream() << "$" << dateExprName
                                            << " expects timezone argument of type string")},
            CaseValuePair{makeNot(makeFunction("isTimezone",
                                               sbe::makeE<sbe::EVariable>(timeZoneDBSlot),
                                               tzRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166602},
                                                 str::stream() << "$" << dateExprName
                                                               << " expects a valid timezone")},
            CaseValuePair{
                makeNot(
                    makeFunction("typeMatch",
                                 startDateRef.clone(),
                                 makeConstant(sbe::value::TypeTags::NumberInt64,
                                              sbe::value::bitcastFrom<int64_t>(dateTypeMask())))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166603},
                                       str::stream()
                                           << "$" << dateExprName
                                           << " must have startDate argument convertable to date")},
            CaseValuePair{
                generateNonStringCheck(unitRef),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166604},
                                       str::stream() << "$" << dateExprName
                                                     << " expects unit argument of type string")},
            CaseValuePair{makeNot(makeFunction("isTimeUnit", unitRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166605},
                                                 str::stream() << "$" << dateExprName
                                                               << " expects a valid time unit")},
            CaseValuePair{makeNot(makeFunction("exists", amountRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166606},
                                                 str::stream() << "invalid $" << dateExprName
                                                               << " 'amount' argument value")},
            sbe::makeE<sbe::EFunction>("dateAdd", std::move(args)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(dateAddExpr)));
    }

    void unsupportedExpression(const char* op) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(5182300, str::stream() << "Unsupported expression in SBE stage builder: " << op);
    }

    ExpressionVisitorContext* _context;
};
}  // namespace

std::unique_ptr<sbe::EExpression> generateCoerceToBoolExpression(sbe::EVariable branchRef) {
    auto makeNotNullOrUndefinedCheck = [&branchRef]() {
        return makeNot(makeFunction(
            "typeMatch",
            branchRef.clone(),
            makeConstant(sbe::value::TypeTags::NumberInt64,
                         sbe::value::bitcastFrom<int64_t>(getBSONTypeMask(BSONType::jstNULL) |
                                                          getBSONTypeMask(BSONType::Undefined)))));
    };

    auto makeNeqFalseCheck = [&branchRef]() {
        return makeBinaryOp(
            sbe::EPrimBinary::neq,
            makeBinaryOp(sbe::EPrimBinary::cmp3w,
                         branchRef.clone(),
                         sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                    sbe::value::bitcastFrom<bool>(false))),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(0)));
    };

    auto makeNeqZeroCheck = [&branchRef]() {
        return makeBinaryOp(
            sbe::EPrimBinary::neq,
            makeBinaryOp(sbe::EPrimBinary::cmp3w,
                         branchRef.clone(),
                         sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                    sbe::value::bitcastFrom<int64_t>(0))),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(0)));
    };

    return makeBinaryOp(
        sbe::EPrimBinary::logicAnd,
        makeFunction("exists", branchRef.clone()),
        makeBinaryOp(
            sbe::EPrimBinary::logicAnd,
            makeNotNullOrUndefinedCheck(),
            makeBinaryOp(sbe::EPrimBinary::logicAnd, makeNeqFalseCheck(), makeNeqZeroCheck())));
}

EvalExprStagePair generateExpression(StageBuilderState& state,
                                     const Expression* expr,
                                     EvalStage stage,
                                     boost::optional<sbe::value::SlotId> optionalRootSlot,
                                     PlanNodeId planNodeId) {
    ExpressionVisitorContext context(state, std::move(stage), optionalRootSlot, planNodeId);

    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk<const Expression>(expr, &walker);

    return context.done();
}
}  // namespace mongo::stage_builder
