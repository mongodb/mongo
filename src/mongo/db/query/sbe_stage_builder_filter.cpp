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

#include "mongo/db/query/sbe_stage_builder_filter.h"

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
/**
 * Helper functions for building common EExpressions and PlanStage trees.
 */
std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit = 1) {
    return sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(planNodeId), limit, boost::none, planNodeId);
}

std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
    return sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(e));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return sbe::makeE<sbe::EFunction>(
        "fillEmpty"sv,
        sbe::makeEs(std::move(e), sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, 0)));
}

/**
 * EvalExpr is a wrapper around an EExpression that can also carry a SlotId.
 */
struct EvalExpr {
    EvalExpr() {}

    EvalExpr(EvalExpr&& e) : expr(std::move(e.expr)), slot(e.slot) {
        e.slot = boost::none;
    }

    EvalExpr(std::unique_ptr<sbe::EExpression>&& e) : expr(std::move(e)) {}

    EvalExpr(sbe::value::SlotId s) : expr(sbe::makeE<sbe::EVariable>(s)), slot(s) {}

    EvalExpr& operator=(EvalExpr&& e) {
        expr = std::move(e.expr);
        slot = e.slot;
        e.slot = boost::none;
        return *this;
    }

    EvalExpr& operator=(std::unique_ptr<sbe::EExpression>&& e) {
        expr = std::move(e);
        slot = boost::none;
        return *this;
    }

    EvalExpr& operator=(sbe::value::SlotId s) {
        expr = sbe::makeE<sbe::EVariable>(s);
        slot = s;
        return *this;
    }

    explicit operator bool() const {
        return static_cast<bool>(expr);
    }

    void reset() {
        expr.reset();
        slot = boost::none;
    }

    std::unique_ptr<sbe::EExpression> expr;
    boost::optional<sbe::value::SlotId> slot;
};

/**
 * To support non-leaf operators in general, MatchExpressionVisitorContext maintains a stack of
 * EvalFrames. An EvalFrame holds a subtree to build on top of (stage), the relevantSlots vector
 * for the subtree (in case it's needed for plumbing slots through a LoopJoinStage), an input slot
 * to read from when evaluating predicates (inputSlot), and a place to store the output expression
 * for an operator (output). Initially there is only one EvalFrame on the stack which holds the
 * main tree. Non-leaf operators can decide to push an EvalFrame on the stack before each of their
 * children is evaluated if desired. If a non-leaf operator pushes one or more EvalFrames onto the
 * stack, it is responsible for removing these EvalFrames from the stack later.
 *
 * When an operator stores its output into an EvalFrame, it has the option of storing the output
 * as an EExpression or as a SlotId. This flexibility helps us avoid creating extra slots and
 * ProjectStages that aren't needed.
 */
struct EvalFrame {
    EvalFrame(std::unique_ptr<sbe::PlanStage> stage,
              sbe::value::SlotId inputSlot,
              sbe::value::SlotVector relevantSlots)
        : inputSlot(inputSlot), stage(std::move(stage)), relevantSlots{std::move(relevantSlots)} {}

    sbe::value::SlotId inputSlot;
    std::unique_ptr<sbe::PlanStage> stage;
    sbe::value::SlotVector relevantSlots;
    EvalExpr output;
};

/**
 * The various flavors of PathMatchExpressions require the same skeleton of traverse operators in
 * order to perform implicit path traversal, but may translate differently to an SBE expression that
 * actually applies the predicate against an individual array element.
 *
 * A function of this type can be called to generate an EExpression which applies a predicate to the
 * value found in 'inputSlot'.
 */
using MakePredicateFn = std::function<std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>>(
    sbe::value::SlotId inputSlot, std::unique_ptr<sbe::PlanStage> inputStage)>;

using MakePredicateReturnType = std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>>;

/**
 * A struct for storing context across calls to visit() methods in MatchExpressionVisitor's.
 */
struct MatchExpressionVisitorContext {
    MatchExpressionVisitorContext(OperationContext* opCtx,
                                  sbe::value::SlotIdGenerator* slotIdGenerator,
                                  sbe::value::FrameIdGenerator* frameIdGenerator,
                                  std::unique_ptr<sbe::PlanStage> inputStage,
                                  sbe::value::SlotId inputSlotIn,
                                  sbe::value::SlotVector relevantSlotsIn,
                                  const MatchExpression* root,
                                  sbe::RuntimeEnvironment* env,
                                  PlanNodeId planNodeId)
        : opCtx{opCtx},
          inputSlot{inputSlotIn},
          relevantSlots{std::move(relevantSlotsIn)},
          slotIdGenerator{slotIdGenerator},
          frameIdGenerator{frameIdGenerator},
          topLevelAnd{nullptr},
          env{env},
          planNodeId{planNodeId} {
        // If 'inputSlot' is not present within 'relevantSlots', add it now.
        if (!std::count(relevantSlots.begin(), relevantSlots.end(), inputSlot)) {
            relevantSlots.push_back(inputSlot);
        }

        // Set up the top-level EvalFrame.
        evalStack.emplace_back(std::move(inputStage), inputSlot, relevantSlots);

        // If the root node is an $and, store it in 'topLevelAnd'.
        // TODO: SERVER-50673: Revisit how we implement the top-level $and optimization.
        if (root->matchType() == MatchExpression::AND) {
            topLevelAnd = root;
        }
    }

    std::unique_ptr<sbe::PlanStage> done() {
        invariant(evalStack.size() == 1);
        auto& frame = evalStack.back();

        if (frame.output) {
            frame.stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(frame.stage), std::move(frame.output.expr), planNodeId);
            frame.output.reset();
        }

        return std::move(frame.stage);
    }

    OperationContext* opCtx;
    std::vector<EvalFrame> evalStack;
    sbe::value::SlotId inputSlot;
    sbe::value::SlotVector relevantSlots;
    sbe::value::SlotIdGenerator* slotIdGenerator;
    sbe::value::FrameIdGenerator* frameIdGenerator;
    const MatchExpression* topLevelAnd;
    sbe::RuntimeEnvironment* env;

    // The id of the 'QuerySolutionNode' which houses the match expression that we are converting to
    // SBE.
    const PlanNodeId planNodeId;
};

/**
 * projectEvalExpr() takes an EvalExpr's value and materializes it into a slot (if it's not
 * already in a slot), and then it returns the slot.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> projectEvalExpr(
    EvalExpr evalExpr,
    std::unique_ptr<sbe::PlanStage> stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    // If evalExpr's value is already in a slot, return the slot.
    if (evalExpr.slot) {
        return {*evalExpr.slot, std::move(stage)};
    }

    // If evalExpr's value is an expression, create a ProjectStage to evaluate the expression
    // into a slot.
    auto slot = slotIdGenerator->generate();
    stage = sbe::makeProjectStage(std::move(stage), planNodeId, slot, std::move(evalExpr.expr));
    return {slot, std::move(stage)};
}

std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>> generateShortCircuitingLogicalOp(
    sbe::EPrimBinary::Op logicOp,
    std::vector<std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>>> branches,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

    // For AND and OR, if 'branches' only has one element, we can just return branches[0].
    if (branches.size() == 1) {
        return std::move(branches[0]);
    }

    // Prepare to create limit-1/union with N branches (where N is the number of operands). Each
    // branch will be evaluated from left to right until one of the branches produces a value. The
    // first N-1 branches have a FilterStage to control whether they produce a value. If a branch's
    // filter condition is true, the branch will produce a value and the remaining branches will not
    // be evaluated. In other words, the evaluation process will "short-circuit". If a branch's
    // filter condition is false, the branch will not produce a value and the evaluation process
    // will continue. The last branch doesn't have a FilterStage and will always produce a value.
    std::vector<std::unique_ptr<sbe::PlanStage>> stages;
    std::vector<sbe::value::SlotVector> inputs;
    for (size_t i = 0, n = branches.size(); i < n; ++i) {
        auto [expr, stage] = std::move(branches[i]);
        sbe::value::SlotId slot;

        if (i != n - 1) {
            // Create a FilterStage for each branch (except the last one). If a branch's filter
            // condition is true, it will "short-circuit" the evaluation process. For AND, short-
            // circuiting should happen if an operand evalautes to false. For OR, short-circuiting
            // should happen if an operand evaluates to true.
            auto filterExpr = (logicOp == sbe::EPrimBinary::logicAnd)
                ? makeNot(std::move(expr.expr))
                : std::move(expr.expr);
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), std::move(filterExpr), planNodeId);

            // Set up an output value to be returned if short-circuiting occurs. For AND, when
            // short-circuiting occurs, the output returned should be false. For OR, when short-
            // circuiting occurs, the output returned should be true.
            auto shortCircuitVal = sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::Boolean, (logicOp == sbe::EPrimBinary::logicOr));
            slot = slotIdGenerator->generate();
            stage = sbe::makeProjectStage(
                std::move(stage), planNodeId, slot, std::move(shortCircuitVal));
        } else {
            std::tie(slot, stage) =
                projectEvalExpr(std::move(expr), std::move(stage), planNodeId, slotIdGenerator);
        }

        stages.emplace_back(std::move(stage));
        inputs.emplace_back(sbe::makeSV(slot));
    }

    // Return a union wrapped in a limit-1.
    auto outputSlot = slotIdGenerator->generate();
    auto outputStage = sbe::makeS<sbe::UnionStage>(
        std::move(stages), std::move(inputs), sbe::makeSV(outputSlot), planNodeId);
    outputStage =
        sbe::makeS<sbe::LimitSkipStage>(std::move(outputStage), 1, boost::none, planNodeId);

    return {outputSlot, std::move(outputStage)};
}

enum class LeafTraversalMode {
    // Don't generate a TraverseStage for the leaf.
    kDoNotTraverseLeaf = 0,

    // Traverse the leaf, ard for arrays visit both the array's elements _and_ the array itself.
    kArrayAndItsElements = 1,

    // Traverse the leaf, and for arrays visit the array's elements but not the array itself.
    kArrayElementsOnly = 2,
};

/**
 * This function generates a path traversal plan stage at the given nested 'level' of the traversal
 * path. For example, for a dotted path expression {'a.b': 2}, the traversal sub-tree will look like
 * this:
 *
 *     traverse
 *          outputSlot1 // the traversal result
 *          innerSlot1 // the result coming from the 'in' branch
 *          fieldSlot1 // field 'a' projected in the 'from' branch, this is the field we will be
 *                     // traversing
 *          {outputSlot1 || innerSlot1} // the folding expression - combining
 *                                      // results for each element
 *          {outputSlot1} // final (early out) expression - when we hit the 'true' value,
 *                        // we don't have to traverse the whole array
 *      in
 *          project [innerSlot1 =                               // if getField(fieldSlot1,'b')
 *                    fillEmpty(outputSlot2, false) ||          // returns an array, compare the
 *                    (fillEmpty(isArray(fieldSlot), false) &&  // array itself to 2 as well
 *                     fillEmpty(fieldSlot2==2, false))]
 *          traverse // nested traversal
 *              outputSlot2 // the traversal result
 *              innerSlot2 // the result coming from the 'in' branch
 *              fieldSlot2 // field 'b' projected in the 'from' branch, this is the field we will be
 *                         // traversing
 *              {outputSlot2 || innerSlot2} // the folding expression
 *              {outputSlot2} // final (early out) expression
 *          in
 *              project [innerSlot2 =                        // compare the field 'b' to 2 and store
 *                         fillEmpty(fieldSlot2==2, false)]  // the bool result in innerSlot2
 *              limit 1
 *              coscan
 *          from
 *              project [fieldSlot2 = getField(fieldSlot1, 'b')] // project field 'b' from the
 *                                                               // document  bound to 'fieldSlot1',
 *                                                               // which is field 'a'
 *              limit 1
 *              coscan
 *      from
 *         project [fieldSlot1 = getField(inputSlot, 'a')] // project field 'a' from the document
 *                                                         // bound to 'inputSlot'
 *         <inputStage>  // e.g., COLLSCAN
 */
std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>> generateTraverseHelper(
    std::unique_ptr<sbe::PlanStage> inputStage,
    const sbe::value::SlotVector& relevantSlots,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    size_t level,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    const MakePredicateFn& makePredicate,
    LeafTraversalMode mode) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    const bool isLeafField = (level == fp.getPathLength() - 1u);

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldSlot'.
    std::string_view fieldName{fp.getFieldName(level).rawData(), fp.getFieldName(level).size()};
    auto fieldSlot{slotIdGenerator->generate()};
    auto fromBranch = sbe::makeProjectStage(
        std::move(inputStage),
        planNodeId,
        fieldSlot,
        sbe::makeE<sbe::EFunction>("getField"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                               sbe::makeE<sbe::EConstant>(fieldName))));

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    auto [innerExpr, innerBranch] = isLeafField
        // Base case: Evaluate the predicate.
        ? makePredicate(fieldSlot, makeLimitCoScanTree(planNodeId))
        // Recursive case.
        : generateTraverseHelper(makeLimitCoScanTree(planNodeId),
                                 sbe::makeSV(),
                                 fieldSlot,
                                 fp,
                                 level + 1,
                                 planNodeId,
                                 slotIdGenerator,
                                 makePredicate,
                                 mode);

    if (isLeafField && mode == LeafTraversalMode::kDoNotTraverseLeaf) {
        auto relSlots = relevantSlots;
        relSlots.push_back(fieldSlot);
        auto outputStage = sbe::makeS<sbe::LoopJoinStage>(
            std::move(fromBranch), std::move(innerBranch), relSlots, relSlots, nullptr, planNodeId);

        return {std::move(innerExpr), std::move(outputStage)};
    }

    sbe::value::SlotId innerSlot;
    std::tie(innerSlot, innerBranch) =
        projectEvalExpr(std::move(innerExpr), std::move(innerBranch), planNodeId, slotIdGenerator);

    // Generate the traverse stage for the current nested level.
    auto outputSlot = slotIdGenerator->generate();
    auto outputStage = sbe::makeS<sbe::TraverseStage>(
        std::move(fromBranch),
        std::move(innerBranch),
        fieldSlot,
        outputSlot,
        innerSlot,
        sbe::makeSV(),
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                     sbe::makeE<sbe::EVariable>(outputSlot),
                                     sbe::makeE<sbe::EVariable>(innerSlot)),
        sbe::makeE<sbe::EVariable>(outputSlot),
        planNodeId,
        1);

    if (isLeafField && mode == LeafTraversalMode::kArrayAndItsElements) {
        // For the last level, if 'mode' == kArrayAndItsElements and getField() returns an array we
        // need to apply the predicate both to the elements of the array _and_ to the array itself.
        // By itself, TraverseStage only applies the predicate to the elements of the array. Thus,
        // for the last level, we add a ProjectStage so that we also apply the predicate to the
        // array itself. (For cases where getField() doesn't return an array, this additional
        // ProjectStage is effectively a no-op.)
        EvalExpr outputExpr;
        std::tie(outputExpr, outputStage) = makePredicate(fieldSlot, std::move(outputStage));

        outputExpr = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicOr,
            makeFillEmptyFalse(sbe::makeE<sbe::EVariable>(outputSlot)),
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicAnd,
                makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                    "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlot)))),
                std::move(outputExpr.expr)));

        return {std::move(outputExpr), std::move(outputStage)};
    } else {
        return {outputSlot, std::move(outputStage)};
    }
}

/**
 * Given a field path 'path' and a predicate 'makePredicate', this function generates an SBE tree
 * that will evaluate the predicate on the field path. When 'path' is not empty string (""), this
 * function generates a sequence of nested traverse operators to traverse the field path and it uses
 * 'makePredicate' to generate an SBE expression for evaluating the predicate on individual value.
 * When 'path' is empty, this function simply uses 'makePredicate' to generate an SBE expression for
 * evaluating the predicate on a single value.
 */
void generateTraverse(MatchExpressionVisitorContext* context,
                      StringData path,
                      MakePredicateFn makePredicate,
                      LeafTraversalMode mode = LeafTraversalMode::kArrayAndItsElements) {
    auto& frame = context->evalStack.back();
    std::tie(frame.output, frame.stage) = generateTraverseHelper(std::move(frame.stage),
                                                                 frame.relevantSlots,
                                                                 frame.inputSlot,
                                                                 FieldPath{path},
                                                                 0,
                                                                 context->planNodeId,
                                                                 context->slotIdGenerator,
                                                                 makePredicate,
                                                                 mode);
}

/**
 * Generates a path traversal SBE plan stage sub-tree for matching arrays with '$size'. Applies
 * an extra project on top of the sub-tree to filter based on user provided value.
 */
void generateArraySize(MatchExpressionVisitorContext* context,
                       const SizeMatchExpression* matchExpr) {
    int size = matchExpr->getData();

    auto makePredicate =
        [&](sbe::value::SlotId inputSlot,
            std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
        // Before generating a final leaf traverse stage, check that the thing we are about
        // traverse is indeed an array.
        auto fromBranch = sbe::makeS<sbe::FilterStage<false>>(
            std::move(inputStage),
            sbe::makeE<sbe::EFunction>("isArray",
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
            context->planNodeId);

        // Generate a traverse that projects the integer value 1 for each element in the array and
        // then sums up the 1's, resulting in the count of elements in the array.
        auto innerSlot = context->slotIdGenerator->generate();
        auto traverseSlot = context->slotIdGenerator->generate();
        auto innerBranch =
            sbe::makeProjectStage(makeLimitCoScanTree(context->planNodeId),
                                  context->planNodeId,
                                  innerSlot,
                                  sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 1));
        auto traverseStage = sbe::makeS<sbe::TraverseStage>(
            std::move(fromBranch),
            std::move(innerBranch),
            inputSlot,
            traverseSlot,
            innerSlot,
            sbe::makeSV(),
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::add,
                                         sbe::makeE<sbe::EVariable>(traverseSlot),
                                         sbe::makeE<sbe::EVariable>(innerSlot)),
            nullptr,
            context->planNodeId,
            1);

        // If the traversal result was not Nothing, compare it to the user provided value. If the
        // traversal result was Nothing, that means the array was empty, so replace Nothing with 0
        // and compare it to the user provided value.
        auto sizeOutput = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::eq,
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, size),
            sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EFunction>("exists",
                                           sbe::makeEs(sbe::makeE<sbe::EVariable>(traverseSlot))),
                sbe::makeE<sbe::EVariable>(traverseSlot),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0)));

        return {std::move(sizeOutput), std::move(traverseStage)};
    };

    generateTraverse(context,
                     matchExpr->path(),
                     std::move(makePredicate),
                     LeafTraversalMode::kDoNotTraverseLeaf);
}

/**
 * Generates a path traversal SBE plan stage sub-tree which implments the comparison match
 * expression 'expr'. The comparison itself executes using the given 'binaryOp'.
 */
void generateComparison(MatchExpressionVisitorContext* context,
                        const ComparisonMatchExpression* expr,
                        sbe::EPrimBinary::Op binaryOp) {
    auto makePredicate =
        [expr, binaryOp](sbe::value::SlotId inputSlot,
                         std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
        const auto& rhs = expr->getData();
        auto [tagView, valView] = sbe::bson::convertFrom(
            true, rhs.rawdata(), rhs.rawdata() + rhs.size(), rhs.fieldNameSize() - 1);

        // SBE EConstant assumes ownership of the value so we have to make a copy here.
        auto [tag, val] = sbe::value::copyValue(tagView, valView);

        return {
            makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(binaryOp,
                                                            sbe::makeE<sbe::EVariable>(inputSlot),
                                                            sbe::makeE<sbe::EConstant>(tag, val))),
            std::move(inputStage)};
    };

    generateTraverse(context, expr->path(), std::move(makePredicate));
}

/**
 * Generates and pushes a constant boolean expression for either alwaysTrue or alwaysFalse.
 */
void generateAlwaysBoolean(MatchExpressionVisitorContext* context, bool value) {
    auto& frame = context->evalStack.back();

    frame.output = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, value);
}

/**
 * Generates a SBE plan stage sub-tree which implements the bitwise match expression 'expr'. The
 * various bit test expressions accept a numeric, BinData or position list bitmask. Here we handle
 * building an EExpression for both the numeric and BinData or position list forms of the bitmask.
 */
void generateBitTest(MatchExpressionVisitorContext* context,
                     const BitTestMatchExpression* expr,
                     const sbe::BitTestBehavior& bitTestBehavior) {
    auto makePredicate =
        [expr,
         bitTestBehavior](sbe::value::SlotId inputSlot,
                          std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
        auto bitPositions = expr->getBitPositions();

        // Build an array set of bit positions for the bitmask, and remove duplicates in the
        // bitPositions vector since duplicates aren't handled in the match expression parser by
        // checking if an item has already been seen.
        auto [bitPosTag, bitPosVal] = sbe::value::makeNewArray();
        auto arr = sbe::value::getArrayView(bitPosVal);
        arr->reserve(bitPositions.size());

        std::set<int> seenBits;
        for (size_t index = 0; index < bitPositions.size(); ++index) {
            auto currentBit = bitPositions[index];
            if (auto result = seenBits.insert(currentBit); result.second) {
                arr->push_back(sbe::value::TypeTags::NumberInt64, currentBit);
            }
        }

        // An EExpression for the BinData and position list for the binary case of
        // BitTestMatchExpressions. This function will be applied to values carrying BinData
        // elements.
        auto binaryBitTestEExpr = sbe::makeE<sbe::EFunction>(
            "bitTestPosition",
            sbe::makeEs(sbe::makeE<sbe::EConstant>(bitPosTag, bitPosVal),
                        sbe::makeE<sbe::EVariable>(inputSlot),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   static_cast<int32_t>(bitTestBehavior))));

        // Build An EExpression for the numeric bitmask case. The AllSet case tests if (mask &
        // value) == mask, and AllClear case tests if (mask & value) == 0. The AnyClear and the
        // AnySet case is the negation of the AllSet and AllClear cases, respectively.
        auto numericBitTestEExpr =
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, expr->getBitMask());
        if (bitTestBehavior == sbe::BitTestBehavior::AllSet ||
            bitTestBehavior == sbe::BitTestBehavior::AnyClear) {
            numericBitTestEExpr = sbe::makeE<sbe::EFunction>(
                "bitTestMask",
                sbe::makeEs(std::move(numericBitTestEExpr), sbe::makeE<sbe::EVariable>(inputSlot)));

            // The AnyClear case is the negation of the AllSet case.
            if (bitTestBehavior == sbe::BitTestBehavior::AnyClear) {
                numericBitTestEExpr = sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                                                  std::move(numericBitTestEExpr));
            }
        } else if (bitTestBehavior == sbe::BitTestBehavior::AllClear ||
                   bitTestBehavior == sbe::BitTestBehavior::AnySet) {
            numericBitTestEExpr = sbe::makeE<sbe::EFunction>(
                "bitTestZero",
                sbe::makeEs(std::move(numericBitTestEExpr), sbe::makeE<sbe::EVariable>(inputSlot)));

            // The AnySet case is the negation of the AllClear case.
            if (bitTestBehavior == sbe::BitTestBehavior::AnySet) {
                numericBitTestEExpr = sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                                                  std::move(numericBitTestEExpr));
            }
        } else {
            MONGO_UNREACHABLE;
        }

        return {sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EFunction>("isBinData",
                                               sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                    std::move(binaryBitTestEExpr),
                    std::move(numericBitTestEExpr)),
                std::move(inputStage)};
    };

    generateTraverse(context, expr->path(), std::move(makePredicate));
}

/**
 * A match expression pre-visitor used for maintaining nested logical expressions while traversing
 * the match expression tree.
 */
class MatchExpressionPreVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionPreVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}

    void visit(const AndMatchExpression* matchExpr) final {
        auto& frame = _context->evalStack.back();

        if (matchExpr->numChildren() <= 1 || matchExpr == _context->topLevelAnd) {
            // For $and's with no children, we output true (handled in the post-visitor). For a
            // top-level $and with at least one child, and for non-top-level $and's with exactly
            // one child, we evaluate each child within the current EvalFrame ('frame') so that
            // each child builds directly on top of frame->stage.
            return;
        }

        // For non-top-level $and's, we evaluate each child in its own EvalFrame. Set up a new
        // EvalFrame with a limit-1/coscan tree for the first child.
        _context->evalStack.emplace_back(
            makeLimitCoScanTree(_context->planNodeId), frame.inputSlot, sbe::makeSV());
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child
        invariant(matchExpr->numChildren() == 1);

        // We evaluate $elemMatch's child in a new EvalFrame. For the child's EvalFrame, we set the
        // 'stage' field to be a limit-1/coscan tree, and we set the 'inputSlot' field to be a newly
        // allocated slot (childInputSlot). childInputSlot is a "correlated slot" that will be set
        // up later (handled in the post-visitor).
        auto childInputSlot = _context->slotIdGenerator->generate();
        _context->evalStack.emplace_back(
            makeLimitCoScanTree(_context->planNodeId), childInputSlot, sbe::makeSV());
    }

    void visit(const ElemMatchValueMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {
        unsupportedExpression(expr);
    }

    void visit(const NotMatchExpression* expr) final {
        invariant(expr->numChildren() == 1);
    }

    void visit(const OrMatchExpression* matchExpr) final {
        auto& frame = _context->evalStack.back();

        if (matchExpr->numChildren() <= 1) {
            // For $or's with no children, we output false (handled in the post-visitor). For $or's
            // with 1 child, we evaluate the child within the current EvalFrame.
            return;
        }

        // Set up a new EvalFrame with a limit-1/coscan tree for the first child.
        _context->evalStack.emplace_back(
            makeLimitCoScanTree(_context->planNodeId), frame.inputSlot, sbe::makeSV());
    }

    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}

    void visit(const TextMatchExpression* expr) final {
        // The QueryPlanner always converts a $text predicate into a query solution involving the
        // 'TextNode' which is translated to an SBE plan elsewhere. Therefore, no $text predicates
        // should remain in the MatchExpression tree when converting it to SBE.
        MONGO_UNREACHABLE;
    }

    void visit(const TextNoOpMatchExpression* expr) final {
        // No-op $text match expressions exist as a crutch for parsing a $text predicate without
        // having access to the FTS subsystem. We should never attempt to execute a MatchExpression
        // containing such a no-op node.
        MONGO_UNREACHABLE;
    }

    void visit(const TwoDPtInAnnulusExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) const {
        uasserted(4822878,
                  str::stream() << "Match expression is not supported in SBE: "
                                << expr->matchType());
    }

    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression post-visitor which does all the job to translate the match expression tree
 * into an SBE plan stage sub-tree.
 */
class MatchExpressionPostVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionPostVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {
        generateAlwaysBoolean(_context, false);
    }

    void visit(const AlwaysTrueMatchExpression* expr) final {
        generateAlwaysBoolean(_context, true);
    }

    void visit(const AndMatchExpression* matchExpr) final {
        auto numChildren = matchExpr->numChildren();
        if (matchExpr == _context->topLevelAnd) {
            // For a top-level $and with no children, do nothing and return. For top-level $and's
            // with at least one, we evaluate each child within the current EvalFrame.
            if (numChildren >= 1) {
                // Process the output of the last child.
                auto& frame = _context->evalStack.back();
                invariant(frame.output);
                frame.stage = sbe::makeS<sbe::FilterStage<false>>(
                    std::move(frame.stage), std::move(frame.output.expr), _context->planNodeId);
                frame.output.reset();
            }
            return;
        } else if (numChildren == 0) {
            // For non-top-level $and's with no children, output true.
            generateAlwaysBoolean(_context, true);
            return;
        } else if (numChildren == 1) {
            // For non-top-level $and's with one child, do nothing and return. The post-visitor for
            // the child expression has already done all the necessary work.
            return;
        }

        // For non-top-level $and's, we evaluate each child in its own EvalFrame. Now that we're
        // done evaluating the children, move the children's outputs off of the evalStack into
        // a vector in preparation for calling generateShortCircuitingLogicalOp().
        std::vector<std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>>> branches;
        for (size_t i = 0, stackSize = _context->evalStack.size(); i < numChildren; ++i) {
            auto& childFrame = _context->evalStack[stackSize - numChildren + i];
            branches.emplace_back(std::move(childFrame.output), std::move(childFrame.stage));
        }

        for (size_t i = 0; i < numChildren; ++i) {
            _context->evalStack.pop_back();
        }

        auto& frame = _context->evalStack.back();

        std::unique_ptr<sbe::PlanStage> andStage;
        std::tie(frame.output, andStage) =
            generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicAnd,
                                             std::move(branches),
                                             _context->planNodeId,
                                             _context->slotIdGenerator);

        // Join frame stage with andStage.
        auto& relSlots = frame.relevantSlots;
        frame.stage = sbe::makeS<sbe::LoopJoinStage>(std::move(frame.stage),
                                                     std::move(andStage),
                                                     relSlots,
                                                     relSlots,
                                                     nullptr,
                                                     _context->planNodeId);
    }

    void visit(const BitsAllClearMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AllClear);
    }

    void visit(const BitsAllSetMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AllSet);
    }

    void visit(const BitsAnyClearMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AnyClear);
    }

    void visit(const BitsAnySetMatchExpression* expr) final {
        generateBitTest(_context, expr, sbe::BitTestBehavior::AnySet);
    }

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child
        invariant(matchExpr->numChildren() == 1);

        // Extract the input slot, the output, and the stage from of the child's EvalFrame, and
        // remove the child's EvalFrame from the stack.
        auto childInputSlot = _context->evalStack.back().inputSlot;
        auto [childOutputSlot, childStage] = [&]() {
            if (matchExpr->getChild(0)->matchType() == MatchExpression::AND &&
                matchExpr->getChild(0)->numChildren() == 0) {
                auto childOutputSlot = _context->slotIdGenerator->generate();
                auto isObjectOrArrayExpr = sbe::makeE<sbe::EPrimBinary>(
                    sbe::EPrimBinary::logicOr,
                    sbe::makeE<sbe::EFunction>(
                        "isObject", sbe::makeEs(sbe::makeE<sbe::EVariable>(childInputSlot))),
                    sbe::makeE<sbe::EFunction>(
                        "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(childInputSlot))));
                return std::make_pair(
                    childOutputSlot,
                    sbe::makeProjectStage(makeLimitCoScanTree(_context->planNodeId),
                                          _context->planNodeId,
                                          childOutputSlot,
                                          std::move(isObjectOrArrayExpr)));
            }
            return projectEvalExpr(std::move(_context->evalStack.back().output),
                                   std::move(_context->evalStack.back().stage),
                                   _context->planNodeId,
                                   _context->slotIdGenerator);
        }();

        _context->evalStack.pop_back();

        auto makePredicate = [&, childOutputSlot = childOutputSlot, &childStage = childStage](
                                 sbe::value::SlotId inputSlot,
                                 std::unique_ptr<sbe::PlanStage> inputStage) {
            // The 'childStage' subtree was generated to read from 'childInputSlot', based on the
            // assumption that 'childInputSlot' is some correlated slot that will be made available
            // by childStages's parent. We add a projection here to 'inputStage' to feed 'inputSlot'
            // into 'childInputSlot'.
            inputStage = sbe::makeProjectStage(std::move(inputStage),
                                               _context->planNodeId,
                                               childInputSlot,
                                               sbe::makeE<sbe::EVariable>(inputSlot));

            // Generate a subtree to check if inputSlot is an array.
            auto isArrayExpr = sbe::makeE<sbe::EFunction>(
                "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot)));

            // Generate the traverse.
            auto traverseSlot = _context->slotIdGenerator->generate();
            auto traverseStage = sbe::makeS<sbe::TraverseStage>(
                std::move(inputStage),
                std::move(childStage),
                childInputSlot,
                traverseSlot,
                childOutputSlot,
                sbe::makeSV(),
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                             sbe::makeE<sbe::EVariable>(traverseSlot),
                                             sbe::makeE<sbe::EVariable>(childOutputSlot)),
                sbe::makeE<sbe::EVariable>(traverseSlot),
                _context->planNodeId,
                1);

            // Use the short-circuiting 'logicAnd' operator to combine the 'isArray' check and the
            // predicate.
            std::vector<std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>>> branches;
            branches.emplace_back(std::move(isArrayExpr),
                                  makeLimitCoScanTree(_context->planNodeId));
            branches.emplace_back(traverseSlot, std::move(traverseStage));

            return generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicAnd,
                                                    std::move(branches),
                                                    _context->planNodeId,
                                                    _context->slotIdGenerator);
        };

        generateTraverse(_context,
                         matchExpr->path(),
                         std::move(makePredicate),
                         LeafTraversalMode::kDoNotTraverseLeaf);
    }

    void visit(const ElemMatchValueMatchExpression* expr) final {}

    void visit(const EqualityMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::eq);
    }

    void visit(const ExistsMatchExpression* expr) final {
        auto makePredicate =
            [](sbe::value::SlotId inputSlot,
               std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
            return {sbe::makeE<sbe::EFunction>("exists",
                                               sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                    std::move(inputStage)};
        };

        generateTraverse(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const ExprMatchExpression* matchExpr) final {
        auto& frame = _context->evalStack.back();

        // The $expr expression must by applied to the current $$ROOT document, so make sure that
        // an input slot associated with the current frame is the same slot as the input slot for
        // the entire match expression we're translating.
        invariant(frame.inputSlot == _context->inputSlot);

        auto&& [_, expr, stage] = generateExpression(_context->opCtx,
                                                     matchExpr->getExpression().get(),
                                                     std::move(frame.stage),
                                                     _context->slotIdGenerator,
                                                     _context->frameIdGenerator,
                                                     frame.inputSlot,
                                                     _context->env,
                                                     _context->planNodeId,
                                                     &frame.relevantSlots);
        auto frameId = _context->frameIdGenerator->generate();

        // We will need to convert the result of $expr to a boolean value, so we'll wrap it into an
        // expression which does exactly that.
        auto logicExpr = generateCoerceToBoolExpression(sbe::EVariable{frameId, 0});

        frame.output = sbe::makeE<sbe::ELocalBind>(
            frameId, sbe::makeEs(std::move(expr)), std::move(logicExpr));
        frame.stage = std::move(stage);
    }

    void visit(const GTEMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::greaterEq);
    }

    void visit(const GTMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::greater);
    }

    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {
        // This is a no-op. The $_internalExprEq match expression is produced internally by
        // rewriting an $expr expression to an AND($expr, $_internalExprEq), which can later be
        // eliminated by via a conversion into EXACT index bounds, or remains present. In the latter
        // case we can simply ignore it, as the result of AND($expr, $_internalExprEq) is equal to
        // just $expr.
        generateAlwaysBoolean(_context, true);
    }
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}

    void visit(const LTEMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::lessEq);
    }

    void visit(const LTMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::less);
    }

    void visit(const ModMatchExpression* expr) final {
        // The mod function returns the result of the mod operation between the operand and
        // given divisor, so construct an expression to then compare the result of the operation
        // to the given remainder.
        auto makePredicate =
            [expr](sbe::value::SlotId inputSlot,
                   std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
            return {
                makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(
                    sbe::EPrimBinary::eq,
                    sbe::makeE<sbe::EFunction>(
                        "mod",
                        sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                               expr->getDivisor()))),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                               expr->getRemainder()))),
                std::move(inputStage)};
        };

        generateTraverse(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const NorMatchExpression* expr) final {}

    void visit(const NotMatchExpression* expr) final {
        auto& frame = _context->evalStack.back();

        // Negate the result of $not's child.
        frame.output = makeNot(std::move(frame.output.expr));
    }

    void visit(const OrMatchExpression* matchExpr) final {
        auto numChildren = matchExpr->numChildren();
        if (numChildren == 0) {
            // For $or's with no children, output false.
            generateAlwaysBoolean(_context, false);
            return;
        } else if (numChildren == 1) {
            // For $or's with 1 child, do nothing and return. The post-visitor for the child
            // expression has already done all the necessary work.
            return;
        }

        // Move the children's outputs off of the evalStack into a vector in preparation for
        // calling generateShortCircuitingLogicalOp().
        std::vector<std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>>> branches;
        for (size_t i = 0, stackSize = _context->evalStack.size(); i < numChildren; ++i) {
            auto& childFrame = _context->evalStack[stackSize - numChildren + i];
            branches.emplace_back(std::move(childFrame.output), std::move(childFrame.stage));
        }

        for (size_t i = 0; i < numChildren; ++i) {
            _context->evalStack.pop_back();
        }

        auto& frame = _context->evalStack.back();
        std::unique_ptr<sbe::PlanStage> orStage;
        std::tie(frame.output, orStage) =
            generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicOr,
                                             std::move(branches),
                                             _context->planNodeId,
                                             _context->slotIdGenerator);

        // Join frame.stage with orStage.
        auto& relSlots = frame.relevantSlots;
        frame.stage = sbe::makeS<sbe::LoopJoinStage>(std::move(frame.stage),
                                                     std::move(orStage),
                                                     relSlots,
                                                     relSlots,
                                                     nullptr,
                                                     _context->planNodeId);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makePredicate =
            [expr](sbe::value::SlotId inputSlot,
                   std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
            auto regex = RegexMatchExpression::makeRegex(expr->getString(), expr->getFlags());
            auto ownedRegexVal = sbe::value::bitcastFrom(regex.release());

            // TODO: In the future, this needs to account for the fact that the regex match
            // expression matches strings, but also matches stored regexes. For example,
            // {$match: {a: /foo/}} matches the document {a: /foo/} in addition to {a: "foobar"}.
            return {makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                        "regexMatch",
                        sbe::makeEs(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::pcreRegex,
                                                               ownedRegexVal),
                                    sbe::makeE<sbe::EVariable>(inputSlot)))),
                    std::move(inputStage)};
        };

        generateTraverse(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const SizeMatchExpression* expr) final {
        generateArraySize(_context, expr);
    }

    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}

    void visit(const TypeMatchExpression* expr) final {
        auto makePredicate =
            [expr](sbe::value::SlotId inputSlot,
                   std::unique_ptr<sbe::PlanStage> inputStage) -> MakePredicateReturnType {
            const MatcherTypeSet& ts = expr->typeSet();
            return {sbe::makeE<sbe::ETypeMatch>(sbe::makeE<sbe::EVariable>(inputSlot),
                                                ts.getBSONTypeMask()),
                    std::move(inputStage)};
        };

        generateTraverse(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression in-visitor used for maintaining the counter of the processed child
 * expressions of the nested logical expressions in the match expression tree being traversed.
 */
class MatchExpressionInVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionInVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}

    void visit(const AndMatchExpression* matchExpr) final {
        // This method should never be called for $and's with less than 2 children.
        invariant(matchExpr->numChildren() >= 2);
        auto& frame = _context->evalStack.back();

        if (matchExpr == _context->topLevelAnd) {
            // For a top-level $and, we evaluate each child within the current EvalFrame.
            // Process the output of the most recently evaluated child.
            invariant(frame.output);
            frame.stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(frame.stage), std::move(frame.output.expr), _context->planNodeId);
            frame.output.reset();
        } else {
            // For non-top-level $and's, we evaluate each child in its own EvalFrame, and we
            // leave these EvalFrames on the stack until we're done evaluating all the children.
            // Set up a new EvalFrame with a limit-1/coscan tree for the next child.
            _context->evalStack.emplace_back(
                makeLimitCoScanTree(_context->planNodeId), frame.inputSlot, sbe::makeSV());
        }
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child, so we don't
        // need to do anything here.
    }

    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}

    void visit(const OrMatchExpression* matchExpr) final {
        // This method should never be called for $or's with less than 2 children.
        invariant(matchExpr->numChildren() >= 2);
        auto& frame = _context->evalStack.back();

        // We leave the EvalFrame of each child on the stack until we're done evaluating all the
        // children. Set up a new EvalFrame with a limit-1/coscan tree for the next child.
        _context->evalStack.emplace_back(
            makeLimitCoScanTree(_context->planNodeId), frame.inputSlot, sbe::makeSV());
    }

    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};
}  // namespace

std::unique_ptr<sbe::PlanStage> generateFilter(OperationContext* opCtx,
                                               const MatchExpression* root,
                                               std::unique_ptr<sbe::PlanStage> stage,
                                               sbe::value::SlotIdGenerator* slotIdGenerator,
                                               sbe::value::FrameIdGenerator* frameIdGenerator,
                                               sbe::value::SlotId inputSlot,
                                               sbe::RuntimeEnvironment* env,
                                               sbe::value::SlotVector relevantSlots,
                                               PlanNodeId planNodeId) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return stage;
    }

    MatchExpressionVisitorContext context{opCtx,
                                          slotIdGenerator,
                                          frameIdGenerator,
                                          std::move(stage),
                                          inputSlot,
                                          relevantSlots,
                                          root,
                                          env,
                                          planNodeId};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    return context.done();
}
}  // namespace mongo::stage_builder
