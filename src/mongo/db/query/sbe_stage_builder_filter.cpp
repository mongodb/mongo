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
 * The various flavors of PathMatchExpressions require the same skeleton of traverse operators in
 * order to perform implicit path traversal, but may translate differently to an SBE expression that
 * actually applies the predicate against an individual array element.
 *
 * A function of this type can be called to generate an EExpression which applies a predicate to the
 * value found in 'inputSlot'.
 */
using MakePredicateEExprFn =
    std::function<std::unique_ptr<sbe::EExpression>(sbe::value::SlotId inputSlot)>;

std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(long long limit = 1) {
    return sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), limit, boost::none);
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

    operator bool() const {
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
 * A struct for storing context across calls to visit() methods in MatchExpressionVisitor's.
 */
struct MatchExpressionVisitorContext {
    MatchExpressionVisitorContext(sbe::value::SlotIdGenerator* slotIdGenerator,
                                  std::unique_ptr<sbe::PlanStage> inputStage,
                                  sbe::value::SlotId inputSlotIn,
                                  sbe::value::SlotVector relevantSlotsIn,
                                  const MatchExpression* root)
        : inputSlot{inputSlotIn},
          relevantSlots{std::move(relevantSlotsIn)},
          slotIdGenerator{slotIdGenerator},
          topLevelAnd{nullptr} {
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
            frame.stage = sbe::makeS<sbe::FilterStage<false>>(std::move(frame.stage),
                                                              std::move(frame.output.expr));
            frame.output.reset();
        }

        return std::move(frame.stage);
    }

    std::vector<EvalFrame> evalStack;
    sbe::value::SlotId inputSlot;
    sbe::value::SlotVector relevantSlots;
    sbe::value::SlotIdGenerator* slotIdGenerator;
    const MatchExpression* topLevelAnd;
};

/**
 * projectEvalExpr() takes an EvalExpr's value and materializes it into a slot (if it's not
 * already in a slot), and then it returns the slot.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> projectEvalExpr(
    EvalExpr evalExpr,
    std::unique_ptr<sbe::PlanStage> stage,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    // If evalExpr's value is already in a slot, return the slot.
    if (evalExpr.slot) {
        return {*evalExpr.slot, std::move(stage)};
    }

    // If evalExpr's value is an expression, create a ProjectStage to evaluate the expression
    // into a slot.
    auto slot = slotIdGenerator->generate();
    stage = sbe::makeProjectStage(std::move(stage), slot, std::move(evalExpr.expr));
    return {slot, std::move(stage)};
}

enum class LeafArrayTraversalMode {
    kArrayAndItsElements = 0,  // Visit both the array's elements _and_ the array itself
    kArrayElementsOnly = 1,    // Visit the array's elements but don't visit the array itself
};

/**
 * A helper function to generate a path traversal plan stage at the given nested 'level' of the
 * traversal path. For example, for a dotted path expression {'a.b': 2}, the traversal sub-tree will
 * look like this:
 *
 *     traverse
 *          outputSlot1 // the traversal result
 *          innerSlot1 // the result coming from the 'in' branch
 *          fieldSlot1 // field 'a' projected in the 'from' branch, this is the field we will be
 *                     // traversing
 *          {outputSlot1 || innerSlot1} // the folding expression - combining
 *                                      // results for each element
 *          {outputSlot1} // final (early out) expression - when we hit the 'true' value,
 *  i                     // we don't have to traverse the whole array
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
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    size_t level,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    const MakePredicateEExprFn& makePredicate,
    LeafArrayTraversalMode mode) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldSlot'.
    std::string_view fieldName{fp.getFieldName(level).rawData(), fp.getFieldName(level).size()};
    auto fieldSlot{slotIdGenerator->generate()};
    auto fromBranch = sbe::makeProjectStage(
        std::move(inputStage),
        fieldSlot,
        sbe::makeE<sbe::EFunction>("getField"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                               sbe::makeE<sbe::EConstant>(fieldName))));

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    sbe::value::SlotId innerSlot;
    std::unique_ptr<sbe::PlanStage> innerBranch;

    if (level == fp.getPathLength() - 1u) {
        // Base case: Genereate a ProjectStage to evaluate the predicate.
        innerSlot = slotIdGenerator->generate();
        innerBranch =
            sbe::makeProjectStage(makeLimitCoScanTree(), innerSlot, makePredicate(fieldSlot));
    } else {
        // Recursive case.
        auto [expr, stage] = generateTraverseHelper(
            makeLimitCoScanTree(), fieldSlot, fp, level + 1, slotIdGenerator, makePredicate, mode);

        std::tie(innerSlot, stage) =
            projectEvalExpr(std::move(expr), std::move(stage), slotIdGenerator);
        innerBranch = std::move(stage);
    }

    // Generate the traverse stage for the current nested level.
    auto outputSlot{slotIdGenerator->generate()};
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
        1);

    auto outputExpr = sbe::makeE<sbe::EVariable>(outputSlot);

    if (mode == LeafArrayTraversalMode::kArrayAndItsElements && level == fp.getPathLength() - 1u) {
        // For the last level, if 'mode' == kArrayAndItsElements and getField() returns an array we
        // need to apply the predicate both to the elements of the array _and_ to the array itself.
        // By itself, TraverseStage only applies the predicate to the elements of the array. Thus,
        // for the last level, we add a ProjectStage so that we also apply the predicate to the
        // array itself. (For cases where getField() doesn't return an array, this additional
        // ProjectStage is effectively a no-op.)
        outputExpr = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicOr,
            makeFillEmptyFalse(std::move(outputExpr)),
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicAnd,
                makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                    "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlot)))),
                makePredicate(fieldSlot)));

        return {std::move(outputExpr), std::move(outputStage)};
    } else {
        return {outputSlot, std::move(outputStage)};
    }
}

/*
 * A helper function for 'generateTraverseForArraySize' similar to the 'generateTraverseHelper'. The
 * function extends the traverse sub-tree generation by retuning a special leaf-level traverse stage
 * that uses a fold expression to add counts of elements in the array, as well as performs an extra
 * check that the leaf-level traversal is being done on a valid array.
 */
std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>> generateTraverseForArraySizeHelper(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    size_t level,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int size) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldSlot'.
    std::string_view fieldName{fp.getFieldName(level).rawData(), fp.getFieldName(level).size()};
    auto fieldSlot{slotIdGenerator->generate()};
    auto fromBranch = sbe::makeProjectStage(
        std::move(inputStage),
        fieldSlot,
        sbe::makeE<sbe::EFunction>("getField"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                               sbe::makeE<sbe::EConstant>(fieldName))));

    sbe::value::SlotId innerSlot;
    std::unique_ptr<sbe::PlanStage> innerBranch;

    if (level == fp.getPathLength() - 1u) {
        innerSlot = slotIdGenerator->generate();

        // Before generating a final leaf traverse stage, check that the thing we are about to
        // traverse is indeed an array.
        fromBranch = sbe::makeS<sbe::FilterStage<false>>(
            std::move(fromBranch),
            sbe::makeE<sbe::EFunction>("isArray",
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlot))));

        // Project '1' for each element in the array, then sum up using a fold expression.
        innerBranch =
            sbe::makeProjectStage(makeLimitCoScanTree(),
                                  innerSlot,
                                  sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 1));

        // The final traverse stage for the leaf level with a fold expression that sums up
        // values in slot fieldSlot, resulting in the count of elements in the array.
        auto outputSlot{slotIdGenerator->generate()};
        auto leafLevelTraverseStage = sbe::makeS<sbe::TraverseStage>(
            std::move(fromBranch),
            std::move(innerBranch),
            fieldSlot,
            outputSlot,
            innerSlot,
            sbe::makeSV(),
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::add,
                                         sbe::makeE<sbe::EVariable>(outputSlot),
                                         sbe::makeE<sbe::EVariable>(innerSlot)),
            nullptr,
            1);

        // Final project stage to filter based on the user provided value. If the traversal result
        // was not evaluated to Nothing, then compare to the user provided value. If the traversal
        // final result did evaluate to Nothing, the only way the fold expression result would be
        // Nothing is if the array was empty, so replace Nothing with 0 and compare to the user
        // provided value.
        auto outputExpr = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::eq,
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, size),
            sbe::makeE<sbe::EIf>(sbe::makeE<sbe::EFunction>(
                                     "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(outputSlot))),
                                 sbe::makeE<sbe::EVariable>(outputSlot),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0)));

        return {std::move(outputExpr), std::move(leafLevelTraverseStage)};
    } else {
        // Recursive case.
        auto [expr, stage] = generateTraverseForArraySizeHelper(
            makeLimitCoScanTree(), fieldSlot, fp, level + 1, slotIdGenerator, size);

        std::tie(innerSlot, stage) =
            projectEvalExpr(std::move(expr), std::move(stage), slotIdGenerator);
        innerBranch = std::move(stage);
    }

    // The final traverse stage for the current nested level.
    auto outputSlot{slotIdGenerator->generate()};
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
        1);

    return {outputSlot, std::move(outputStage)};
}

/**
 * For the given PathMatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the expression. Generates a sequence of nested traverse operators in order to
 * perform nested array traversal, and then calls 'makeEExprCallback' in order to generate an SBE
 * expression responsible for applying the predicate to individual array elements.
 */
void generateTraverse(MatchExpressionVisitorContext* context,
                      const PathMatchExpression* matchExpr,
                      MakePredicateEExprFn makePredicate) {
    auto& frame = context->evalStack.back();

    FieldPath fp{matchExpr->path()};

    std::tie(frame.output, frame.stage) =
        generateTraverseHelper(std::move(frame.stage),
                               frame.inputSlot,
                               fp,
                               0,
                               context->slotIdGenerator,
                               makePredicate,
                               LeafArrayTraversalMode::kArrayAndItsElements);
}

/**
 * Generates a path traversal SBE plan stage sub-tree for matching arrays with '$size'. Applies
 * an extra project on top of the sub-tree to filter based on user provided value.
 */
void generateTraverseForArraySize(MatchExpressionVisitorContext* context,
                                  const SizeMatchExpression* matchExpr) {
    auto& frame = context->evalStack.back();

    FieldPath fp{matchExpr->path()};

    std::tie(frame.output, frame.stage) =
        generateTraverseForArraySizeHelper(std::move(frame.stage),
                                           frame.inputSlot,
                                           fp,
                                           0,
                                           context->slotIdGenerator,
                                           matchExpr->getData());
}

/**
 * Generates a path traversal SBE plan stage sub-tree which implments the comparison match
 * expression 'expr'. The comparison itself executes using the given 'binaryOp'.
 */
void generateTraverseForComparisonPredicate(MatchExpressionVisitorContext* context,
                                            const ComparisonMatchExpression* expr,
                                            sbe::EPrimBinary::Op binaryOp) {
    auto makeEExprFn = [expr, binaryOp](sbe::value::SlotId inputSlot) {
        const auto& rhs = expr->getData();
        auto [tagView, valView] = sbe::bson::convertFrom(
            true, rhs.rawdata(), rhs.rawdata() + rhs.size(), rhs.fieldNameSize() - 1);

        // SBE EConstant assumes ownership of the value so we have to make a copy here.
        auto [tag, val] = sbe::value::copyValue(tagView, valView);

        return makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(
            binaryOp, sbe::makeE<sbe::EVariable>(inputSlot), sbe::makeE<sbe::EConstant>(tag, val)));
    };
    generateTraverse(context, expr, std::move(makeEExprFn));
}

/**
 * Generates and pushes a constant boolean expression for either alwaysTrue or alwaysFalse.
 */
void generateAlwaysBoolean(MatchExpressionVisitorContext* context, bool value) {
    auto& frame = context->evalStack.back();

    frame.output = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, value);
}

std::pair<EvalExpr, std::unique_ptr<sbe::PlanStage>> generateShortCircuitingLogicalOp(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotVector relevantSlots,
    sbe::EPrimBinary::Op logicOp,
    std::vector<std::unique_ptr<sbe::PlanStage>> stages,
    std::vector<std::unique_ptr<sbe::EExpression>> outputs,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);
    invariant(stages.size() == outputs.size());

    // Prepare to create limit-1/union with N branches (where N is the number of operands). Each
    // branch will be evaluated from left to right until one of the branches produces a value. The
    // first N-1 branches have a FilterStage to control whether they produce a value. If a branch's
    // filter condition is true, the branch will produce a value and the remaining branches will not
    // be evaluated. In other words, the evaluation process will "short-circuit". If a branch's
    // filter condition is false, the branch will not produce a value and the evaluation process
    // will continue. The last branch doesn't have a FilterStage and will always produce a value.
    std::vector<sbe::value::SlotVector> inputVals;
    for (size_t i = 0, n = stages.size(); i < n; ++i) {
        if (i != n - 1) {
            // Create a FilterStage for each branch (except the last one). If a branch's filter
            // condition is true, it will "short-circuit" the evaluation process. For AND, short-
            // circuiting should happen if an operand evalautes to false. For OR, short-circuiting
            // should happen if an operand evaluates to true.
            stages[i] = sbe::makeS<sbe::FilterStage<false>>(std::move(stages[i]),
                                                            logicOp == sbe::EPrimBinary::logicAnd
                                                                ? makeNot(std::move(outputs[i]))
                                                                : std::move(outputs[i]));

            // Set up an output value to be returned if short-circuiting occurs. For AND, when
            // short-circuiting occurs, the output returned should be false. For OR, when short-
            // circuiting occurs, the output returned should be true.
            bool shortCircuitVal = (logicOp == sbe::EPrimBinary::logicOr);
            outputs[i] = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, shortCircuitVal);
        }

        // Project the output expression into a slot, and add the slot to the union's vector of
        // input slots.
        auto slot = slotIdGenerator->generate();
        stages[i] = sbe::makeProjectStage(std::move(stages[i]), slot, std::move(outputs[i]));
        inputVals.emplace_back(sbe::makeSV(slot));
    }

    // Generate the union wrapped in a limit-1.
    auto outputSlot = slotIdGenerator->generate();
    auto unionStage = sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::UnionStage>(
            std::move(stages), std::move(inputVals), sbe::makeSV(outputSlot)),
        1,
        boost::none);

    // Join inputStage with unionStage and return it.
    auto outputStage = sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage), std::move(unionStage), relevantSlots, relevantSlots, nullptr);

    // The UnionStage's output slot holds the result of the logical operation ('logicOp').
    return {outputSlot, std::move(outputStage)};
}

/**
 * Generates a SBE plan stage sub-tree which implements the bitwise match expression 'expr'. The
 * various bit test expressions accept a numeric, BinData or position list bitmask. Here we handle
 * building an EExpression for both the numeric and BinData or position list forms of the bitmask.
 */
void generateTraverseForBitTests(MatchExpressionVisitorContext* context,
                                 const BitTestMatchExpression* expr,
                                 const sbe::BitTestBehavior& bitTestBehavior) {
    auto makeEExprFn = [expr, bitTestBehavior](sbe::value::SlotId inputSlot) {
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
        return sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EFunction>("isBinData",
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
            std::move(binaryBitTestEExpr),
            std::move(numericBitTestEExpr));
    };
    generateTraverse(context, expr, std::move(makeEExprFn));
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
        _context->evalStack.emplace_back(makeLimitCoScanTree(), frame.inputSlot, sbe::makeSV());
    }
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
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
    void visit(const InternalExprEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
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
        _context->evalStack.emplace_back(makeLimitCoScanTree(), frame.inputSlot, sbe::makeSV());
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
                frame.stage = sbe::makeS<sbe::FilterStage<false>>(std::move(frame.stage),
                                                                  std::move(frame.output.expr));
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

        // For non-top-level $and's, we evaluate each child in its own EvalFrame. Now that
        // we're done evaluating each child, process their outputs.

        // Move the outputs from the evalStack into various data structures in preparation for
        // generating a UnionStage.
        std::vector<std::unique_ptr<sbe::PlanStage>> stages;
        std::vector<std::unique_ptr<sbe::EExpression>> outputs;
        for (size_t i = 0, stackSize = _context->evalStack.size(); i < numChildren; ++i) {
            auto& childFrame = _context->evalStack[stackSize - numChildren + i];
            stages.emplace_back(std::move(childFrame.stage));
            outputs.emplace_back(std::move(childFrame.output.expr));
        }
        // Remove the children's EvalFrames from the stack.
        for (size_t i = 0; i < numChildren; ++i) {
            _context->evalStack.pop_back();
        }

        auto& frame = _context->evalStack.back();

        std::tie(frame.output, frame.stage) =
            generateShortCircuitingLogicalOp(std::move(frame.stage),
                                             frame.relevantSlots,
                                             sbe::EPrimBinary::logicAnd,
                                             std::move(stages),
                                             std::move(outputs),
                                             _context->slotIdGenerator);
    }

    void visit(const BitsAllClearMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AllClear);
    }

    void visit(const BitsAllSetMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AllSet);
    }

    void visit(const BitsAnyClearMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AnyClear);
    }

    void visit(const BitsAnySetMatchExpression* expr) final {
        generateTraverseForBitTests(_context, expr, sbe::BitTestBehavior::AnySet);
    }

    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}

    void visit(const EqualityMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::eq);
    }

    void visit(const ExistsMatchExpression* expr) final {
        auto makeEExprFn = [](sbe::value::SlotId inputSlot) {
            return sbe::makeE<sbe::EFunction>("exists",
                                              sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot)));
        };
        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const ExprMatchExpression* expr) final {}

    void visit(const GTEMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::greaterEq);
    }

    void visit(const GTMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::greater);
    }

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

    void visit(const LTEMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::lessEq);
    }

    void visit(const LTMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::less);
    }

    void visit(const ModMatchExpression* expr) final {
        // The mod function returns the result of the mod operation between the operand and
        // given divisor, so construct an expression to then compare the result of the operation
        // to the given remainder.
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            return makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::eq,
                sbe::makeE<sbe::EFunction>(
                    "mod",
                    sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                           expr->getDivisor()))),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           expr->getRemainder())));
        };

        generateTraverse(_context, expr, std::move(makeEExprFn));
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
            // For $or's with 1 child, do nothing and return.
            return;
        }

        // For $or's, we evaluate each child in its own EvalFrame. Now that  we're done evaluating
        // each child, process their outputs.

        // Move the outputs from the evalStack into various data structures in preparation for
        // generating a UnionStage.
        std::vector<std::unique_ptr<sbe::PlanStage>> stages;
        std::vector<std::unique_ptr<sbe::EExpression>> outputs;
        for (size_t i = 0, stackSize = _context->evalStack.size(); i < numChildren; ++i) {
            auto& childFrame = _context->evalStack[stackSize - numChildren + i];
            stages.emplace_back(std::move(childFrame.stage));
            outputs.emplace_back(std::move(childFrame.output.expr));
        }
        // Remove the children's EvalFrames from the stack.
        for (size_t i = 0; i < numChildren; ++i) {
            _context->evalStack.pop_back();
        }

        auto& frame = _context->evalStack.back();

        std::tie(frame.output, frame.stage) =
            generateShortCircuitingLogicalOp(std::move(frame.stage),
                                             frame.relevantSlots,
                                             sbe::EPrimBinary::logicOr,
                                             std::move(stages),
                                             std::move(outputs),
                                             _context->slotIdGenerator);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            auto regex = RegexMatchExpression::makeRegex(expr->getString(), expr->getFlags());
            auto ownedRegexVal = sbe::value::bitcastFrom(regex.release());

            // TODO: In the future, this needs to account for the fact that the regex match
            // expression matches strings, but also matches stored regexes. For example,
            // {$match: {a: /foo/}} matches the document {a: /foo/} in addition to {a: "foobar"}.
            return makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                "regexMatch",
                sbe::makeEs(
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::pcreRegex, ownedRegexVal),
                    sbe::makeE<sbe::EVariable>(inputSlot))));
        };

        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const SizeMatchExpression* expr) final {
        generateTraverseForArraySize(_context, expr);
    }

    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}

    void visit(const TypeMatchExpression* expr) final {
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            const MatcherTypeSet& ts = expr->typeSet();
            return sbe::makeE<sbe::ETypeMatch>(sbe::makeE<sbe::EVariable>(inputSlot),
                                               ts.getBSONTypeMask());
        };
        generateTraverse(_context, expr, std::move(makeEExprFn));
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
            frame.stage = sbe::makeS<sbe::FilterStage<false>>(std::move(frame.stage),
                                                              std::move(frame.output.expr));
            frame.output.reset();
        } else {
            // For non-top-level $and's, we evaluate each child in its own EvalFrame, and we
            // leave these EvalFrames on the stack until we're done evaluating all the children.
            // Set up a new EvalFrame with a limit-1/coscan tree for the next child.
            _context->evalStack.emplace_back(makeLimitCoScanTree(), frame.inputSlot, sbe::makeSV());
        }
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
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
        _context->evalStack.emplace_back(makeLimitCoScanTree(), frame.inputSlot, sbe::makeSV());
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

std::unique_ptr<sbe::PlanStage> generateFilter(const MatchExpression* root,
                                               std::unique_ptr<sbe::PlanStage> stage,
                                               sbe::value::SlotIdGenerator* slotIdGenerator,
                                               sbe::value::SlotId inputSlot,
                                               sbe::value::SlotVector relevantSlots) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return stage;
    }

    MatchExpressionVisitorContext context{
        slotIdGenerator, std::move(stage), inputSlot, relevantSlots, root};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    return context.done();
}
}  // namespace mongo::stage_builder
