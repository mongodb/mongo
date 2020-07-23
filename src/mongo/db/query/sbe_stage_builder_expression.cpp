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

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
std::pair<sbe::value::TypeTags, sbe::value::Value> convertFrom(Value val) {
    // TODO: Either make this conversion unnecessary by changing the value representation in
    // ExpressionConstant, or provide a nicer way to convert directly from Document/Value to
    // sbe::Value.
    BSONObjBuilder bob;
    val.addToBsonObj(&bob, ""_sd);
    auto obj = bob.done();
    auto be = obj.objdata();
    auto end = be + sbe::value::readFromMemory<uint32_t>(be);
    return sbe::bson::convertFrom(false, be + 4, end, 0);
}

struct ExpressionVisitorContext {
    struct VarsFrame {
        std::deque<Variables::Id> variablesToBind;
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> boundVariables;

        template <class... Args>
        VarsFrame(Args&&... args)
            : variablesToBind{std::forward<Args>(args)...}, boundVariables{} {}
    };

    struct LogicalExpressionEvalFrame {
        std::unique_ptr<sbe::PlanStage> savedTraverseStage;
        sbe::value::SlotVector savedRelevantSlots;

        sbe::value::SlotId nextBranchResultSlot;

        std::vector<std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>> branches;

        LogicalExpressionEvalFrame(std::unique_ptr<sbe::PlanStage> traverseStage,
                                   const sbe::value::SlotVector& relevantSlots,
                                   sbe::value::SlotId nextBranchResultSlot)
            : savedTraverseStage(std::move(traverseStage)),
              savedRelevantSlots(relevantSlots),
              nextBranchResultSlot(nextBranchResultSlot) {}
    };

    ExpressionVisitorContext(std::unique_ptr<sbe::PlanStage> inputStage,
                             sbe::value::SlotIdGenerator* slotIdGenerator,
                             sbe::value::FrameIdGenerator* frameIdGenerator,
                             sbe::value::SlotId rootSlot,
                             sbe::value::SlotVector* relevantSlots)
        : traverseStage(std::move(inputStage)),
          slotIdGenerator(slotIdGenerator),
          frameIdGenerator(frameIdGenerator),
          rootSlot(rootSlot),
          relevantSlots(relevantSlots) {}

    void ensureArity(size_t arity) {
        invariant(exprs.size() >= arity);
    }

    std::unique_ptr<sbe::EExpression> popExpr() {
        auto expr = std::move(exprs.top());
        exprs.pop();
        return expr;
    }

    void pushExpr(std::unique_ptr<sbe::EExpression> expr) {
        exprs.push(std::move(expr));
    }

    void pushExpr(std::unique_ptr<sbe::EExpression> expr, std::unique_ptr<sbe::PlanStage> stage) {
        exprs.push(std::move(expr));
        traverseStage = std::move(stage);
    }

    /**
     * Temporarily reset 'traverseStage' and 'relevantSlots' so they are prepared for translating a
     * $and/$or branch. (They will be restored later using the saved values in the
     * 'logicalExpressionEvalFrameStack' top entry.) The new 'traverseStage' is actually a
     * projection that will evaluate to a constant false (for $and) or true (for $or). Once this
     * branch is fully contructed, it will have a filter stage that will either filter out the
     * constant (when branch evaluation does not short circuit) or produce the constant value
     * (therefore producing the short-circuit result). These branches are part of a union stage, so
     * each time a branch fails to produce a value, execution moves on to the next branch. A limit
     * stage above the union ensures that execution halts once one of the branches produces a
     * result.
     */
    void prepareToTranslateShortCircuitingBranch(sbe::EPrimBinary::Op logicOp,
                                                 sbe::value::SlotId branchResultSlot) {
        invariant(!logicalExpressionEvalFrameStack.empty());
        logicalExpressionEvalFrameStack.top().nextBranchResultSlot = branchResultSlot;

        auto shortCircuitVal = (logicOp == sbe::EPrimBinary::logicOr);
        auto shortCircuitExpr = sbe::makeE<sbe::EConstant>(
            sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom(shortCircuitVal));
        traverseStage = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            branchResultSlot,
            std::move(shortCircuitExpr));

        // Slots created in a previous branch for this $and/$or are not accessible to any stages in
        // this new branch, so we clear them from the 'relevantSlots' list.
        *relevantSlots = logicalExpressionEvalFrameStack.top().savedRelevantSlots;

        // The 'branchResultSlot' is where the new branch will place its result in the event of a
        // short circuit, and it must be visible to the union stage after the branch executes.
        relevantSlots->push_back(branchResultSlot);
    }

    /**
     * This does the same thing as 'prepareToTranslateShortCircuitingBranch' but is intended to the
     * last branch in an $and/$or, which cannot short circuit.
     */
    void prepareToTranslateConcludingLogicalBranch() {
        invariant(!logicalExpressionEvalFrameStack.empty());

        traverseStage = sbe::makeS<sbe::CoScanStage>();
        *relevantSlots = logicalExpressionEvalFrameStack.top().savedRelevantSlots;
    }

    std::tuple<sbe::value::SlotId,
               std::unique_ptr<sbe::EExpression>,
               std::unique_ptr<sbe::PlanStage>>
    done() {
        invariant(exprs.size() == 1);
        return {slotIdGenerator->generate(), popExpr(), std::move(traverseStage)};
    }

    std::unique_ptr<sbe::PlanStage> traverseStage;
    sbe::value::SlotIdGenerator* slotIdGenerator;
    sbe::value::FrameIdGenerator* frameIdGenerator;
    sbe::value::SlotId rootSlot;
    std::stack<std::unique_ptr<sbe::EExpression>> exprs;
    std::map<Variables::Id, sbe::value::SlotId> environment;
    std::stack<VarsFrame> varsFrameStack;
    std::stack<LogicalExpressionEvalFrame> logicalExpressionEvalFrameStack;
    // See the comment above the generateExpression() declaration for an explanation of the
    // 'relevantSlots' list.
    sbe::value::SlotVector* relevantSlots;
};

/**
 * Generate an EExpression that converts a value (contained in a variable bound to 'branchRef') that
 * can be of any type to a Boolean value based on MQL's definition of truth for the branch of any
 * logical expression.
 */
std::unique_ptr<sbe::EExpression> generateExpressionForLogicBranch(sbe::EVariable branchRef) {
    // Make an expression that compares the value in 'branchRef' to the result of evaluating the
    // 'valExpr' expression. The comparison uses cmp3w, so that can handle comparisons between
    // values with different types.
    auto makeNeqCheck = [&branchRef](std::unique_ptr<sbe::EExpression> valExpr) {
        return sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::neq,
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::cmp3w, branchRef.clone(), std::move(valExpr)),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0));
    };

    // If any of these are false, the branch is considered false for the purposes of the
    // any logical expression.
    auto checkExists = sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(branchRef.clone()));
    auto checkNotNull = sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isNull", sbe::makeEs(branchRef.clone())));
    auto checkNotFalse =
        makeNeqCheck(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false));
    auto checkNotZero =
        makeNeqCheck(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0));

    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicAnd,
        std::move(checkExists),
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                     std::move(checkNotNull),
                                     sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                                                  std::move(checkNotFalse),
                                                                  std::move(checkNotZero))));
}

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateTraverseHelper(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    size_t level,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // The field we will be traversing at the current nested level.
    auto fieldSlot{slotIdGenerator->generate()};
    // The result coming from the 'in' branch of the traverse plan stage.
    auto outputSlot{slotIdGenerator->generate()};

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldSlot'.
    inputStage = sbe::makeProjectStage(
        std::move(inputStage),
        fieldSlot,
        sbe::makeE<sbe::EFunction>(
            "getField"sv,
            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot), sbe::makeE<sbe::EConstant>([&]() {
                            auto fieldName = fp.getFieldName(level);
                            return std::string_view{fieldName.rawData(), fieldName.size()};
                        }()))));

    std::unique_ptr<sbe::PlanStage> innerBranch;
    if (level == fp.getPathLength() - 1) {
        innerBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            outputSlot,
            sbe::makeE<sbe::EVariable>(fieldSlot));
    } else {
        // Generate nested traversal.
        auto [slot, stage] = generateTraverseHelper(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            fieldSlot,
            fp,
            level + 1,
            slotIdGenerator);
        innerBranch =
            sbe::makeProjectStage(std::move(stage), outputSlot, sbe::makeE<sbe::EVariable>(slot));
    }

    // The final traverse stage for the current nested level.
    return {outputSlot,
            sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                           std::move(innerBranch),
                                           fieldSlot,
                                           outputSlot,
                                           outputSlot,
                                           sbe::makeSV(),
                                           nullptr,
                                           nullptr,
                                           1)};
}

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateTraverse(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    bool expectsDocumentInputOnly,
    const FieldPath& fp,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputSlot' will be a document and _not_ an array (such as
        // when traversing the root document), we can generate a simpler expression.
        return generateTraverseHelper(std::move(inputStage), inputSlot, fp, 0, slotIdGenerator);
    } else {
        // The general case: the value in the 'inputSlot' may be an array that will require
        // traversal.
        auto outputSlot{slotIdGenerator->generate()};
        auto [innerBranchOutputSlot, innerBranch] = generateTraverseHelper(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            inputSlot,
            fp,
            0,  // level
            slotIdGenerator);
        return {outputSlot,
                sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                               std::move(innerBranch),
                                               inputSlot,
                                               outputSlot,
                                               innerBranchOutputSlot,
                                               sbe::makeSV(),
                                               nullptr,
                                               nullptr,
                                               1)};
    }
}

class ExpressionPreVisitor final : public ExpressionVisitor {
public:
    ExpressionPreVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {}
    void visit(ExpressionAbs* expr) final {}
    void visit(ExpressionAdd* expr) final {}
    void visit(ExpressionAllElementsTrue* expr) final {}
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {}
    void visit(ExpressionArray* expr) final {}
    void visit(ExpressionArrayElemAt* expr) final {}
    void visit(ExpressionFirst* expr) final {}
    void visit(ExpressionLast* expr) final {}
    void visit(ExpressionObjectToArray* expr) final {}
    void visit(ExpressionArrayToObject* expr) final {}
    void visit(ExpressionBsonSize* expr) final {}
    void visit(ExpressionCeil* expr) final {}
    void visit(ExpressionCoerceToBool* expr) final {}
    void visit(ExpressionCompare* expr) final {}
    void visit(ExpressionConcat* expr) final {}
    void visit(ExpressionConcatArrays* expr) final {}
    void visit(ExpressionCond* expr) final {}
    void visit(ExpressionDateFromString* expr) final {}
    void visit(ExpressionDateFromParts* expr) final {}
    void visit(ExpressionDateToParts* expr) final {}
    void visit(ExpressionDateToString* expr) final {}
    void visit(ExpressionDivide* expr) final {}
    void visit(ExpressionExp* expr) final {}
    void visit(ExpressionFieldPath* expr) final {}
    void visit(ExpressionFilter* expr) final {}
    void visit(ExpressionFloor* expr) final {}
    void visit(ExpressionIfNull* expr) final {}
    void visit(ExpressionIn* expr) final {}
    void visit(ExpressionIndexOfArray* expr) final {}
    void visit(ExpressionIndexOfBytes* expr) final {}
    void visit(ExpressionIndexOfCP* expr) final {}
    void visit(ExpressionIsNumber* expr) final {}
    void visit(ExpressionLet* expr) final {
        _context->varsFrameStack.push(ExpressionVisitorContext::VarsFrame{
            std::begin(expr->getOrderedVariableIds()), std::end(expr->getOrderedVariableIds())});
    }
    void visit(ExpressionLn* expr) final {}
    void visit(ExpressionLog* expr) final {}
    void visit(ExpressionLog10* expr) final {}
    void visit(ExpressionMap* expr) final {}
    void visit(ExpressionMeta* expr) final {}
    void visit(ExpressionMod* expr) final {}
    void visit(ExpressionMultiply* expr) final {}
    void visit(ExpressionNot* expr) final {}
    void visit(ExpressionObject* expr) final {}
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {}
    void visit(ExpressionRange* expr) final {}
    void visit(ExpressionReduce* expr) final {}
    void visit(ExpressionReplaceOne* expr) final {}
    void visit(ExpressionReplaceAll* expr) final {}
    void visit(ExpressionSetDifference* expr) final {}
    void visit(ExpressionSetEquals* expr) final {}
    void visit(ExpressionSetIntersection* expr) final {}
    void visit(ExpressionSetIsSubset* expr) final {}
    void visit(ExpressionSetUnion* expr) final {}
    void visit(ExpressionSize* expr) final {}
    void visit(ExpressionReverseArray* expr) final {}
    void visit(ExpressionSlice* expr) final {}
    void visit(ExpressionIsArray* expr) final {}
    void visit(ExpressionRound* expr) final {}
    void visit(ExpressionSplit* expr) final {}
    void visit(ExpressionSqrt* expr) final {}
    void visit(ExpressionStrcasecmp* expr) final {}
    void visit(ExpressionSubstrBytes* expr) final {}
    void visit(ExpressionSubstrCP* expr) final {}
    void visit(ExpressionStrLenBytes* expr) final {}
    void visit(ExpressionBinarySize* expr) final {}
    void visit(ExpressionStrLenCP* expr) final {}
    void visit(ExpressionSubtract* expr) final {}
    void visit(ExpressionSwitch* expr) final {}
    void visit(ExpressionToLower* expr) final {}
    void visit(ExpressionToUpper* expr) final {}
    void visit(ExpressionTrim* expr) final {}
    void visit(ExpressionTrunc* expr) final {}
    void visit(ExpressionType* expr) final {}
    void visit(ExpressionZip* expr) final {}
    void visit(ExpressionConvert* expr) final {}
    void visit(ExpressionRegexFind* expr) final {}
    void visit(ExpressionRegexFindAll* expr) final {}
    void visit(ExpressionRegexMatch* expr) final {}
    void visit(ExpressionCosine* expr) final {}
    void visit(ExpressionSine* expr) final {}
    void visit(ExpressionTangent* expr) final {}
    void visit(ExpressionArcCosine* expr) final {}
    void visit(ExpressionArcSine* expr) final {}
    void visit(ExpressionArcTangent* expr) final {}
    void visit(ExpressionArcTangent2* expr) final {}
    void visit(ExpressionHyperbolicArcTangent* expr) final {}
    void visit(ExpressionHyperbolicArcCosine* expr) final {}
    void visit(ExpressionHyperbolicArcSine* expr) final {}
    void visit(ExpressionHyperbolicTangent* expr) final {}
    void visit(ExpressionHyperbolicCosine* expr) final {}
    void visit(ExpressionHyperbolicSine* expr) final {}
    void visit(ExpressionDegreesToRadians* expr) final {}
    void visit(ExpressionRadiansToDegrees* expr) final {}
    void visit(ExpressionDayOfMonth* expr) final {}
    void visit(ExpressionDayOfWeek* expr) final {}
    void visit(ExpressionDayOfYear* expr) final {}
    void visit(ExpressionHour* expr) final {}
    void visit(ExpressionMillisecond* expr) final {}
    void visit(ExpressionMinute* expr) final {}
    void visit(ExpressionMonth* expr) final {}
    void visit(ExpressionSecond* expr) final {}
    void visit(ExpressionWeek* expr) final {}
    void visit(ExpressionIsoWeekYear* expr) final {}
    void visit(ExpressionIsoDayOfWeek* expr) final {}
    void visit(ExpressionIsoWeek* expr) final {}
    void visit(ExpressionYear* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(ExpressionTests::Testable* expr) final {}
    void visit(ExpressionInternalJsEmit* expr) final {}
    void visit(ExpressionInternalFindSlice* expr) final {}
    void visit(ExpressionInternalFindPositional* expr) final {}
    void visit(ExpressionInternalFindElemMatch* expr) final {}
    void visit(ExpressionFunction* expr) final {}
    void visit(ExpressionRandom* expr) final {}

private:
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);

        if (expr->getChildren().size() < 2) {
            // All this bookkeeping is only necessary for short circuiting, so we can skip it if we
            // don't have two or more branches.
            return;
        }

        auto branchResultSlot = _context->slotIdGenerator->generate();
        _context->logicalExpressionEvalFrameStack.emplace(
            std::move(_context->traverseStage), *_context->relevantSlots, branchResultSlot);

        _context->prepareToTranslateShortCircuitingBranch(logicOp, branchResultSlot);
    }

    ExpressionVisitorContext* _context;
};

class ExpressionInVisitor final : public ExpressionVisitor {
public:
    ExpressionInVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {}
    void visit(ExpressionAbs* expr) final {}
    void visit(ExpressionAdd* expr) final {}
    void visit(ExpressionAllElementsTrue* expr) final {}
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {}
    void visit(ExpressionArray* expr) final {}
    void visit(ExpressionArrayElemAt* expr) final {}
    void visit(ExpressionFirst* expr) final {}
    void visit(ExpressionLast* expr) final {}
    void visit(ExpressionObjectToArray* expr) final {}
    void visit(ExpressionArrayToObject* expr) final {}
    void visit(ExpressionBsonSize* expr) final {}
    void visit(ExpressionCeil* expr) final {}
    void visit(ExpressionCoerceToBool* expr) final {}
    void visit(ExpressionCompare* expr) final {}
    void visit(ExpressionConcat* expr) final {}
    void visit(ExpressionConcatArrays* expr) final {}
    void visit(ExpressionCond* expr) final {}
    void visit(ExpressionDateFromString* expr) final {}
    void visit(ExpressionDateFromParts* expr) final {}
    void visit(ExpressionDateToParts* expr) final {}
    void visit(ExpressionDateToString* expr) final {}
    void visit(ExpressionDivide* expr) final {}
    void visit(ExpressionExp* expr) final {}
    void visit(ExpressionFieldPath* expr) final {}
    void visit(ExpressionFilter* expr) final {}
    void visit(ExpressionFloor* expr) final {}
    void visit(ExpressionIfNull* expr) final {}
    void visit(ExpressionIn* expr) final {}
    void visit(ExpressionIndexOfArray* expr) final {}
    void visit(ExpressionIndexOfBytes* expr) final {}
    void visit(ExpressionIndexOfCP* expr) final {}
    void visit(ExpressionIsNumber* expr) final {}
    void visit(ExpressionLet* expr) final {
        // This visitor fires after each variable definition in a $let expression. The top of the
        // _context's expression stack will be an expression defining the variable initializer. We
        // use a separate frame stack ('varsFrameStack') to keep track of which variable we are
        // visiting, so we can appropriately bind the initializer.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();

        invariant(!currentFrame.variablesToBind.empty());

        auto varToBind = currentFrame.variablesToBind.front();
        currentFrame.variablesToBind.pop_front();

        // We create two bindings. First, the initializer result is bound to a slot when this
        // ProjectStage executes.
        auto slotToBind = _context->slotIdGenerator->generate();
        invariant(currentFrame.boundVariables.find(slotToBind) ==
                  currentFrame.boundVariables.end());
        _context->ensureArity(1);
        currentFrame.boundVariables.insert(std::make_pair(slotToBind, _context->popExpr()));

        // Second, we bind this variables AST-level name (with type Variable::Id) to the SlotId that
        // will be used for compilation and execution. Once this "stage builder" finishes, these
        // Variable::Id bindings will no longer be relevant.
        invariant(_context->environment.find(varToBind) == _context->environment.end());
        _context->environment.insert({varToBind, slotToBind});

        if (currentFrame.variablesToBind.empty()) {
            // We have traversed every variable initializer, and this is the last "infix" visit for
            // this $let. Add the the ProjectStage that will perform the actual binding now, so that
            // it executes before the "in" portion of the $let statement does.
            _context->traverseStage = sbe::makeS<sbe::ProjectStage>(
                std::move(_context->traverseStage), std::move(currentFrame.boundVariables));
        }
    }
    void visit(ExpressionLn* expr) final {}
    void visit(ExpressionLog* expr) final {}
    void visit(ExpressionLog10* expr) final {}
    void visit(ExpressionMap* expr) final {}
    void visit(ExpressionMeta* expr) final {}
    void visit(ExpressionMod* expr) final {}
    void visit(ExpressionMultiply* expr) final {}
    void visit(ExpressionNot* expr) final {}
    void visit(ExpressionObject* expr) final {}
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {}
    void visit(ExpressionRange* expr) final {}
    void visit(ExpressionReduce* expr) final {}
    void visit(ExpressionReplaceOne* expr) final {}
    void visit(ExpressionReplaceAll* expr) final {}
    void visit(ExpressionSetDifference* expr) final {}
    void visit(ExpressionSetEquals* expr) final {}
    void visit(ExpressionSetIntersection* expr) final {}
    void visit(ExpressionSetIsSubset* expr) final {}
    void visit(ExpressionSetUnion* expr) final {}
    void visit(ExpressionSize* expr) final {}
    void visit(ExpressionReverseArray* expr) final {}
    void visit(ExpressionSlice* expr) final {}
    void visit(ExpressionIsArray* expr) final {}
    void visit(ExpressionRound* expr) final {}
    void visit(ExpressionSplit* expr) final {}
    void visit(ExpressionSqrt* expr) final {}
    void visit(ExpressionStrcasecmp* expr) final {}
    void visit(ExpressionSubstrBytes* expr) final {}
    void visit(ExpressionSubstrCP* expr) final {}
    void visit(ExpressionStrLenBytes* expr) final {}
    void visit(ExpressionBinarySize* expr) final {}
    void visit(ExpressionStrLenCP* expr) final {}
    void visit(ExpressionSubtract* expr) final {}
    void visit(ExpressionSwitch* expr) final {}
    void visit(ExpressionToLower* expr) final {}
    void visit(ExpressionToUpper* expr) final {}
    void visit(ExpressionTrim* expr) final {}
    void visit(ExpressionTrunc* expr) final {}
    void visit(ExpressionType* expr) final {}
    void visit(ExpressionZip* expr) final {}
    void visit(ExpressionConvert* expr) final {}
    void visit(ExpressionRegexFind* expr) final {}
    void visit(ExpressionRegexFindAll* expr) final {}
    void visit(ExpressionRegexMatch* expr) final {}
    void visit(ExpressionCosine* expr) final {}
    void visit(ExpressionSine* expr) final {}
    void visit(ExpressionTangent* expr) final {}
    void visit(ExpressionArcCosine* expr) final {}
    void visit(ExpressionArcSine* expr) final {}
    void visit(ExpressionArcTangent* expr) final {}
    void visit(ExpressionArcTangent2* expr) final {}
    void visit(ExpressionHyperbolicArcTangent* expr) final {}
    void visit(ExpressionHyperbolicArcCosine* expr) final {}
    void visit(ExpressionHyperbolicArcSine* expr) final {}
    void visit(ExpressionHyperbolicTangent* expr) final {}
    void visit(ExpressionHyperbolicCosine* expr) final {}
    void visit(ExpressionHyperbolicSine* expr) final {}
    void visit(ExpressionDegreesToRadians* expr) final {}
    void visit(ExpressionRadiansToDegrees* expr) final {}
    void visit(ExpressionDayOfMonth* expr) final {}
    void visit(ExpressionDayOfWeek* expr) final {}
    void visit(ExpressionDayOfYear* expr) final {}
    void visit(ExpressionHour* expr) final {}
    void visit(ExpressionMillisecond* expr) final {}
    void visit(ExpressionMinute* expr) final {}
    void visit(ExpressionMonth* expr) final {}
    void visit(ExpressionSecond* expr) final {}
    void visit(ExpressionWeek* expr) final {}
    void visit(ExpressionIsoWeekYear* expr) final {}
    void visit(ExpressionIsoDayOfWeek* expr) final {}
    void visit(ExpressionIsoWeek* expr) final {}
    void visit(ExpressionYear* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(ExpressionTests::Testable* expr) final {}
    void visit(ExpressionInternalJsEmit* expr) final {}
    void visit(ExpressionInternalFindSlice* expr) final {}
    void visit(ExpressionInternalFindPositional* expr) final {}
    void visit(ExpressionInternalFindElemMatch* expr) final {}
    void visit(ExpressionFunction* expr) final {}
    void visit(ExpressionRandom* expr) final {}

private:
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        // The infix visitor should only visit expressions with more than one child.
        invariant(expr->getChildren().size() >= 2);
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);

        auto frameId = _context->frameIdGenerator->generate();
        auto branchExpr = generateExpressionForLogicBranch(sbe::EVariable{frameId, 0});
        std::unique_ptr<sbe::EExpression> shortCircuitCondition;
        if (logicOp == sbe::EPrimBinary::logicAnd) {
            // The filter should take the short circuit path when the branch resolves to _false_, so
            // we invert the filter condition.
            shortCircuitCondition = sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(_context->popExpr()),
                sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(branchExpr)));
        } else {
            // For $or, keep the filter condition as is; the filter will take the short circuit path
            // when the branch resolves to true.
            shortCircuitCondition = sbe::makeE<sbe::ELocalBind>(
                frameId, sbe::makeEs(_context->popExpr()), std::move(branchExpr));
        }

        auto branchStage = sbe::makeS<sbe::FilterStage<false>>(std::move(_context->traverseStage),
                                                               std::move(shortCircuitCondition));

        auto& currentFrameStack = _context->logicalExpressionEvalFrameStack.top();
        currentFrameStack.branches.emplace_back(
            std::make_pair(currentFrameStack.nextBranchResultSlot, std::move(branchStage)));

        if (currentFrameStack.branches.size() < (expr->getChildren().size() - 1)) {
            _context->prepareToTranslateShortCircuitingBranch(
                logicOp, _context->slotIdGenerator->generate());
        } else {
            // We have already translated all but one of the branches, meaning the next branch we
            // translate will be the final one and does not need an short-circuit logic.
            _context->prepareToTranslateConcludingLogicalBranch();
        }
    }

    ExpressionVisitorContext* _context;
};

class ExpressionPostVisitor final : public ExpressionVisitor {
public:
    ExpressionPostVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {
        auto [tag, val] = convertFrom(expr->getValue());
        _context->pushExpr(sbe::makeE<sbe::EConstant>(tag, val));
    }

    void visit(ExpressionAbs* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto checkNullOrEmpty = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicOr,
            sbe::makeE<sbe::EPrimUnary>(
                sbe::EPrimUnary::logicNot,
                sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(inputRef.clone()))),
            sbe::makeE<sbe::EFunction>("isNull", sbe::makeEs(inputRef.clone())));

        auto absExpr = sbe::makeE<sbe::EIf>(
            std::move(checkNullOrEmpty),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(inputRef.clone())),
                sbe::makeE<sbe::EFunction>("abs", sbe::makeEs(inputRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4822870},
                                       "$abs only supports numeric types, not string")));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(absExpr)));
    }
    void visit(ExpressionAdd* expr) final {
        _context->ensureArity(2);
        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();
        _context->pushExpr(
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::add, std::move(lhs), std::move(rhs)));
    }
    void visit(ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArrayElemAt* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFirst* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionLast* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionObjectToArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArrayToObject* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionBsonSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionCeil* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionCoerceToBool* expr) final {
        unsupportedExpression("$coerceToBool");
    }
    void visit(ExpressionCompare* expr) final {
        _context->ensureArity(2);
        std::vector<std::unique_ptr<sbe::EExpression>> operands(2);
        for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
            *it = _context->popExpr();
        }

        auto frameId = _context->frameIdGenerator->generate();
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

        // We use the "cmp3e" primitive for every comparison, because it "type brackets" its
        // comparisons (for example, a number will always compare as less than a string). The other
        // comparison primitives are designed for comparing values of the same type.
        auto cmp3w =
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::cmp3w, lhsRef.clone(), rhsRef.clone());
        auto cmp = (comparisonOperator == sbe::EPrimBinary::cmp3w)
            ? std::move(cmp3w)
            : sbe::makeE<sbe::EPrimBinary>(
                  comparisonOperator,
                  std::move(cmp3w),
                  sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 0));

        // If either operand evaluates to "Nothing," then the entire operation expressed by 'cmp'
        // will also evaluate to "Nothing." MQL comparisons, however, treat "Nothing" as if it is a
        // value that is less than everything other than MinKey. (Notably, two expressions that
        // evaluate to "Nothing" are considered equal to each other.)
        auto nothingFallbackCmp = sbe::makeE<sbe::EPrimBinary>(
            comparisonOperator,
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(lhsRef.clone())),
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(rhsRef.clone())));

        auto cmpWithFallback = sbe::makeE<sbe::EFunction>(
            "fillEmpty", sbe::makeEs(std::move(cmp), std::move(nothingFallbackCmp)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(operands), std::move(cmpWithFallback)));
    }
    void visit(ExpressionConcat* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionConcatArrays* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionCond* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionDateFromString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDateFromParts* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDateToParts* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDateToString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDivide* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionExp* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFieldPath* expr) final {
        if (expr->getVariableId() == Variables::kRemoveId) {
            // The case of $$REMOVE. Note that MQL allows a path in this situation (e.g.,
            // "$$REMOVE.foo.bar") but ignores it.
            _context->pushExpr(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0));
            return;
        }

        sbe::value::SlotId slotId;
        if (expr->isRootFieldPath()) {
            slotId = _context->rootSlot;
        } else {
            auto it = _context->environment.find(expr->getVariableId());
            invariant(it != _context->environment.end());
            slotId = it->second;
        }

        if (expr->getFieldPath().getPathLength() == 1) {
            // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
            // traversal.
            _context->pushExpr(sbe::makeE<sbe::EVariable>(slotId));
            return;
        }

        // Dereference a dotted path, which may contain arrays requiring implicit traversal.
        const bool expectsDocumentInputOnly = slotId == _context->rootSlot;
        auto [outputSlot, stage] = generateTraverse(std::move(_context->traverseStage),
                                                    slotId,
                                                    expectsDocumentInputOnly,
                                                    expr->getFieldPathWithoutCurrentPrefix(),
                                                    _context->slotIdGenerator);
        _context->pushExpr(sbe::makeE<sbe::EVariable>(outputSlot), std::move(stage));
        _context->relevantSlots->push_back(outputSlot);
    }
    void visit(ExpressionFilter* expr) final {
        unsupportedExpression("$filter");
    }
    void visit(ExpressionFloor* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIfNull* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIn* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIndexOfArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIndexOfBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIndexOfCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIsNumber* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto exprIsNum = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(inputRef.clone())),
            sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(inputRef.clone())),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprIsNum)));
    }
    void visit(ExpressionLet* expr) final {
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
            if (currentFrame.boundVariables.count(it->first)) {
                it = _context->environment.erase(it);
            } else {
                ++it;
            }
        }
        _context->varsFrameStack.pop();

        // Note that there is no need to remove SlotId bindings from the the _context's environment.
        // The AST parser already enforces scope rules.
    }
    void visit(ExpressionLn* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionLog10* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }
    void visit(ExpressionMeta* expr) final {
        unsupportedExpression("$meta");
    }
    void visit(ExpressionMod* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionMultiply* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionNot* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());

        auto notExpr = sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                                   generateExpressionForLogicBranch({frameId, 0}));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(notExpr)));
    }
    void visit(ExpressionObject* expr) final {
        unsupportedExpression("$object");
    }
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {
        unsupportedExpression("$pow");
    }
    void visit(ExpressionRange* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(ExpressionReplaceOne* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReplaceAll* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetDifference* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetEquals* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetIntersection* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetIsSubset* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetUnion* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReverseArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIsArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionRound* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSplit* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSqrt* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrcasecmp* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubstrBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubstrCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrLenBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrLenCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubtract* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSwitch* expr) final {
        unsupportedExpression("$switch");
    }
    void visit(ExpressionToLower* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionToUpper* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionTrim* expr) final {
        unsupportedExpression("$trim");
    }
    void visit(ExpressionTrunc* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionType* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionZip* expr) final {
        unsupportedExpression("$zip");
    }
    void visit(ExpressionConvert* expr) final {
        unsupportedExpression("$convert");
    }
    void visit(ExpressionRegexFind* expr) final {
        unsupportedExpression("$regexFind");
    }
    void visit(ExpressionRegexFindAll* expr) final {
        unsupportedExpression("$regexFind");
    }
    void visit(ExpressionRegexMatch* expr) final {
        unsupportedExpression("$regexFind");
    }
    void visit(ExpressionCosine* expr) final {
        unsupportedExpression("$cos");
    }
    void visit(ExpressionSine* expr) final {
        unsupportedExpression("$sin");
    }
    void visit(ExpressionTangent* expr) final {
        unsupportedExpression("$tan");
    }
    void visit(ExpressionArcCosine* expr) final {
        unsupportedExpression("$acos");
    }
    void visit(ExpressionArcSine* expr) final {
        unsupportedExpression("$asin");
    }
    void visit(ExpressionArcTangent* expr) final {
        unsupportedExpression("$atan");
    }
    void visit(ExpressionArcTangent2* expr) final {
        unsupportedExpression("$atan2");
    }
    void visit(ExpressionHyperbolicArcTangent* expr) final {
        unsupportedExpression("$atanh");
    }
    void visit(ExpressionHyperbolicArcCosine* expr) final {
        unsupportedExpression("$acosh");
    }
    void visit(ExpressionHyperbolicArcSine* expr) final {
        unsupportedExpression("$asinh");
    }
    void visit(ExpressionHyperbolicTangent* expr) final {
        unsupportedExpression("$tanh");
    }
    void visit(ExpressionHyperbolicCosine* expr) final {
        unsupportedExpression("$cosh");
    }
    void visit(ExpressionHyperbolicSine* expr) final {
        unsupportedExpression("$sinh");
    }
    void visit(ExpressionDegreesToRadians* expr) final {
        unsupportedExpression("$degreesToRadians");
    }
    void visit(ExpressionRadiansToDegrees* expr) final {
        unsupportedExpression("$radiansToDegrees");
    }
    void visit(ExpressionDayOfMonth* expr) final {
        unsupportedExpression("$dayOfMonth");
    }
    void visit(ExpressionDayOfWeek* expr) final {
        unsupportedExpression("$dayOfWeek");
    }
    void visit(ExpressionDayOfYear* expr) final {
        unsupportedExpression("$dayOfYear");
    }
    void visit(ExpressionHour* expr) final {
        unsupportedExpression("$hour");
    }
    void visit(ExpressionMillisecond* expr) final {
        unsupportedExpression("$millisecond");
    }
    void visit(ExpressionMinute* expr) final {
        unsupportedExpression("$minute");
    }
    void visit(ExpressionMonth* expr) final {
        unsupportedExpression("$month");
    }
    void visit(ExpressionSecond* expr) final {
        unsupportedExpression("$second");
    }
    void visit(ExpressionWeek* expr) final {
        unsupportedExpression("$week");
    }
    void visit(ExpressionIsoWeekYear* expr) final {
        unsupportedExpression("$isoWeekYear");
    }
    void visit(ExpressionIsoDayOfWeek* expr) final {
        unsupportedExpression("$isoDayOfWeek");
    }
    void visit(ExpressionIsoWeek* expr) final {
        unsupportedExpression("$isoWeek");
    }
    void visit(ExpressionYear* expr) final {
        unsupportedExpression("$year");
    }
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionTests::Testable* expr) final {
        unsupportedExpression("$test");
    }
    void visit(ExpressionInternalJsEmit* expr) final {
        unsupportedExpression("$internalJsEmit");
    }
    void visit(ExpressionInternalFindSlice* expr) final {
        unsupportedExpression("$internalFindSlice");
    }
    void visit(ExpressionInternalFindPositional* expr) final {
        unsupportedExpression("$internalFindPositional");
    }
    void visit(ExpressionInternalFindElemMatch* expr) final {
        unsupportedExpression("$internalFindElemMatch");
    }
    void visit(ExpressionFunction* expr) final {
        unsupportedExpression("$function");
    }

    void visit(ExpressionRandom* expr) final {
        unsupportedExpression(expr->getOpName());
    }

private:
    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

        if (expr->getChildren().size() == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value: true
            // and false, respectively.
            bool logicIdentityVal = (logicOp == sbe::EPrimBinary::logicAnd);
            _context->pushExpr(sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom(logicIdentityVal)));
            return;
        } else if (expr->getChildren().size() == 1) {
            // No need for short circuiting logic in a singleton $and/$or. Just execute the branch
            // and return its result as a bool.
            auto frameId = _context->frameIdGenerator->generate();
            _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(_context->popExpr()),
                generateExpressionForLogicBranch(sbe::EVariable{frameId, 0})));

            return;
        }

        auto& LogicalExpressionEvalFrame = _context->logicalExpressionEvalFrameStack.top();

        // The last branch works differently from the others. It just uses a project stage to
        // produce a true or false value for the branch result.
        auto frameId = _context->frameIdGenerator->generate();
        auto lastBranchExpr = sbe::makeE<sbe::ELocalBind>(
            frameId,
            sbe::makeEs(_context->popExpr()),
            generateExpressionForLogicBranch(sbe::EVariable{frameId, 0}));
        auto lastBranchResultSlot = _context->slotIdGenerator->generate();
        auto lastBranch = sbe::makeProjectStage(
            std::move(_context->traverseStage), lastBranchResultSlot, std::move(lastBranchExpr));
        LogicalExpressionEvalFrame.branches.emplace_back(
            std::make_pair(lastBranchResultSlot, std::move(lastBranch)));

        std::vector<sbe::value::SlotVector> branchSlots;
        std::vector<std::unique_ptr<sbe::PlanStage>> branchStages;
        for (auto&& [slot, stage] : LogicalExpressionEvalFrame.branches) {
            branchSlots.push_back(sbe::makeSV(slot));
            branchStages.push_back(std::move(stage));
        }

        auto branchResultSlot = _context->slotIdGenerator->generate();
        auto unionOfBranches = sbe::makeS<sbe::UnionStage>(
            std::move(branchStages), std::move(branchSlots), sbe::makeSV(branchResultSlot));

        // Restore 'relevantSlots' to the way it was before we started translating the logic
        // operator.
        *_context->relevantSlots = std::move(LogicalExpressionEvalFrame.savedRelevantSlots);

        // Get a list of slots that are used by $let expressions. These slots need to be available
        // to the inner side of the LoopJoinStage, in case any of the branches want to reference one
        // of the variables bound by the $let.
        sbe::value::SlotVector letBindings;
        for (auto&& [_, slot] : _context->environment) {
            letBindings.push_back(slot);
        }

        // The LoopJoinStage we are creating here will not expose any of the slots from its outer
        // side except for the ones we explicity ask for. For that reason, we maintain the
        // 'relevantSlots' list of slots that may still be referenced above this stage. All of the
        // slots in 'letBindings' are relevant by this definition, but we track them separately,
        // which is why we need to add them in now.
        auto relevantSlotsWithLetBindings(*_context->relevantSlots);
        relevantSlotsWithLetBindings.insert(
            relevantSlotsWithLetBindings.end(), letBindings.begin(), letBindings.end());

        // Put the union into a nested loop. The inner side of the nested loop will execute exactly
        // once, trying each branch of the union until one of them short circuits or until it
        // reaches the end. This process also restores the old 'traverseStage' value from before we
        // started translating the logic operator, by placing it below the new nested loop stage.
        auto stage = sbe::makeS<sbe::LoopJoinStage>(
            std::move(LogicalExpressionEvalFrame.savedTraverseStage),
            sbe::makeS<sbe::LimitSkipStage>(std::move(unionOfBranches), 1, boost::none),
            std::move(relevantSlotsWithLetBindings),
            std::move(letBindings),
            nullptr /* predicate */);

        // We've already restored all necessary state from the top 'logicalExpressionEvalFrameStack'
        // entry, so we are done with it.
        _context->logicalExpressionEvalFrameStack.pop();

        // The final true/false result of the logic operator is stored in the 'branchResultSlot'
        // slot.
        _context->relevantSlots->push_back(branchResultSlot);
        _context->pushExpr(sbe::makeE<sbe::EVariable>(branchResultSlot), std::move(stage));
    }

    void unsupportedExpression(const char* op) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Expression is not supported in SBE: " << op);
    }

    ExpressionVisitorContext* _context;
};  // namespace

class ExpressionWalker final {
public:
    ExpressionWalker(ExpressionVisitor* preVisitor,
                     ExpressionVisitor* inVisitor,
                     ExpressionVisitor* postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(Expression* expr) {
        expr->acceptVisitor(_preVisitor);
    }

    void inVisit(long long count, Expression* expr) {
        expr->acceptVisitor(_inVisitor);
    }

    void postVisit(Expression* expr) {
        expr->acceptVisitor(_postVisitor);
    }

private:
    ExpressionVisitor* _preVisitor;
    ExpressionVisitor* _inVisitor;
    ExpressionVisitor* _postVisitor;
};
}  // namespace

std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::PlanStage>>
generateExpression(Expression* expr,
                   std::unique_ptr<sbe::PlanStage> stage,
                   sbe::value::SlotIdGenerator* slotIdGenerator,
                   sbe::value::FrameIdGenerator* frameIdGenerator,
                   sbe::value::SlotId rootSlot,
                   sbe::value::SlotVector* relevantSlots) {
    auto tempRelevantSlots = sbe::makeSV(rootSlot);
    relevantSlots = relevantSlots ? relevantSlots : &tempRelevantSlots;

    ExpressionVisitorContext context(
        std::move(stage), slotIdGenerator, frameIdGenerator, rootSlot, relevantSlots);
    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk(&walker, expr);
    return context.done();
}
}  // namespace mongo::stage_builder
