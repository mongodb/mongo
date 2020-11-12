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
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
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
using MakePredicateFn =
    std::function<EvalExprStagePair(sbe::value::SlotId inputSlot, EvalStage inputStage)>;

/**
 * A struct for storing context across calls to visit() methods in MatchExpressionVisitor's.
 */
struct MatchExpressionVisitorContext {
    MatchExpressionVisitorContext(OperationContext* opCtx,
                                  sbe::value::SlotIdGenerator* slotIdGenerator,
                                  sbe::value::FrameIdGenerator* frameIdGenerator,
                                  EvalStage inputStage,
                                  sbe::value::SlotId inputSlot,
                                  const MatchExpression* root,
                                  sbe::RuntimeEnvironment* env,
                                  PlanNodeId planNodeId)
        : opCtx{opCtx},
          inputSlot{inputSlot},
          slotIdGenerator{slotIdGenerator},
          frameIdGenerator{frameIdGenerator},
          topLevelAnd{nullptr},
          env{env},
          planNodeId{planNodeId} {
        // Set up the top-level EvalFrame.
        evalStack.emplaceFrame(std::move(inputStage), inputSlot);

        // If the root node is an $and, store it in 'topLevelAnd'.
        // TODO: SERVER-50673: Revisit how we implement the top-level $and optimization.
        if (root->matchType() == MatchExpression::AND) {
            topLevelAnd = root;
        }
    }

    EvalStage done() {
        invariant(evalStack.framesCount() == 1);
        auto& frame = evalStack.topFrame();

        if (frame.exprsCount() > 0) {
            invariant(frame.exprsCount() == 1);
            frame.setStage(
                makeFilter<false>(frame.extractStage(), frame.popExpr().extractExpr(), planNodeId));
        }

        return frame.extractStage();
    }

    struct InputSlotFrameData {
        sbe::value::SlotId inputSlot;
    };

    OperationContext* opCtx;
    EvalStack<InputSlotFrameData> evalStack;
    sbe::value::SlotId inputSlot;
    sbe::value::SlotIdGenerator* slotIdGenerator;
    sbe::value::FrameIdGenerator* frameIdGenerator;
    const MatchExpression* topLevelAnd;
    sbe::RuntimeEnvironment* env;

    // The id of the 'QuerySolutionNode' which houses the match expression that we are converting to
    // SBE.
    const PlanNodeId planNodeId;
};

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
EvalExprStagePair generatePathTraversal(EvalStage inputStage,
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
    auto fromBranch =
        makeProject(std::move(inputStage),
                    planNodeId,
                    fieldSlot,
                    sbe::makeE<sbe::EFunction>("getField"sv,
                                               sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                                           sbe::makeE<sbe::EConstant>(fieldName))));

    if (isLeafField && mode == LeafTraversalMode::kDoNotTraverseLeaf) {
        return makePredicate(fieldSlot, std::move(fromBranch));
    }

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    auto [innerExpr, innerBranch] = isLeafField
        // Base case: Evaluate the predicate.
        ? makePredicate(fieldSlot, EvalStage{})
        // Recursive case.
        : generatePathTraversal(EvalStage{},
                                fieldSlot,
                                fp,
                                level + 1,
                                planNodeId,
                                slotIdGenerator,
                                makePredicate,
                                mode);

    sbe::value::SlotId innerSlot;
    std::tie(innerSlot, innerBranch) =
        projectEvalExpr(std::move(innerExpr), std::move(innerBranch), planNodeId, slotIdGenerator);

    // Generate the traverse stage for the current nested level.
    auto outputSlot = slotIdGenerator->generate();
    auto outputStage =
        makeTraverse(std::move(fromBranch),
                     std::move(innerBranch),  // NOLINT(bugprone-use-after-move)
                     fieldSlot,
                     outputSlot,
                     innerSlot,
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
                outputExpr.extractExpr()));

        return {std::move(outputExpr), std::move(outputStage)};  // NOLINT(bugprone-use-after-move)
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
void generatePredicate(MatchExpressionVisitorContext* context,
                       StringData path,
                       MakePredicateFn makePredicate,
                       LeafTraversalMode mode = LeafTraversalMode::kArrayAndItsElements) {
    auto& frame = context->evalStack.topFrame();
    auto&& [expr, stage] = [&]() {
        if (!path.empty()) {
            return generatePathTraversal(frame.extractStage(),
                                         frame.data().inputSlot,
                                         FieldPath{path},
                                         0,
                                         context->planNodeId,
                                         context->slotIdGenerator,
                                         makePredicate,
                                         mode);
        } else {
            // If matchExpr's parent is a ElemMatchValueMatchExpression, then matchExpr()->path()
            // will be empty. In this case, 'inputSlot' will be a "correlated slot" that holds the
            // value of the ElemMatchValueMatchExpression's field path, and we should apply the
            // predicate directly on 'inputSlot' without array traversal.
            return makePredicate(frame.data().inputSlot, frame.extractStage());
        }
    }();

    frame.setStage(std::move(stage));
    frame.pushExpr(std::move(expr));
}

/**
 * Generates a path traversal SBE plan stage sub-tree for matching arrays with '$size'. Applies
 * an extra project on top of the sub-tree to filter based on user provided value.
 */
void generateArraySize(MatchExpressionVisitorContext* context,
                       const SizeMatchExpression* matchExpr) {
    int size = matchExpr->getData();

    auto makePredicate = [&](sbe::value::SlotId inputSlot,
                             EvalStage inputStage) -> EvalExprStagePair {
        // Before generating a final leaf traverse stage, check that the thing we are about
        // traverse is indeed an array.
        auto fromBranch =
            makeFilter<false>(std::move(inputStage),
                              sbe::makeE<sbe::EFunction>(
                                  "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                              context->planNodeId);

        // Generate a traverse that projects the integer value 1 for each element in the array and
        // then sums up the 1's, resulting in the count of elements in the array.
        auto innerSlot = context->slotIdGenerator->generate();
        auto innerBranch =
            makeProject(EvalStage{},
                        context->planNodeId,
                        innerSlot,
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                   sbe::value::bitcastFrom<int64_t>(1)));

        auto traverseSlot = context->slotIdGenerator->generate();
        auto traverseStage =
            makeTraverse(std::move(fromBranch),
                         std::move(innerBranch),
                         inputSlot,
                         traverseSlot,
                         innerSlot,
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
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(size)),
            sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EFunction>("exists",
                                           sbe::makeEs(sbe::makeE<sbe::EVariable>(traverseSlot))),
                sbe::makeE<sbe::EVariable>(traverseSlot),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           sbe::value::bitcastFrom<int64_t>(0))));

        return {std::move(sizeOutput), std::move(traverseStage)};
    };

    generatePredicate(context,
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
    auto makePredicate = [expr, binaryOp](sbe::value::SlotId inputSlot,
                                          EvalStage inputStage) -> EvalExprStagePair {
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

    generatePredicate(context, expr->path(), std::move(makePredicate));
}

/**
 * Generates and pushes a constant boolean expression for either alwaysTrue or alwaysFalse.
 */
void generateAlwaysBoolean(MatchExpressionVisitorContext* context, bool value) {
    auto& frame = context->evalStack.topFrame();
    frame.pushExpr(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                              sbe::value::bitcastFrom<bool>(value)));
}

/**
 * Generates a SBE plan stage sub-tree which implements the bitwise match expression 'expr'. The
 * various bit test expressions accept a numeric, BinData or position list bitmask. Here we handle
 * building an EExpression for both the numeric and BinData or position list forms of the bitmask.
 */
void generateBitTest(MatchExpressionVisitorContext* context,
                     const BitTestMatchExpression* expr,
                     const sbe::BitTestBehavior& bitTestBehavior) {
    auto makePredicate = [expr, bitTestBehavior](sbe::value::SlotId inputSlot,
                                                 EvalStage inputStage) -> EvalExprStagePair {
        auto bitPositions = expr->getBitPositions();

        // Build an array set of bit positions for the bitmask, and remove duplicates in the
        // bitPositions vector since duplicates aren't handled in the match expression parser by
        // checking if an item has already been seen.
        auto [bitPosTag, bitPosVal] = sbe::value::makeNewArray();
        auto arr = sbe::value::getArrayView(bitPosVal);
        arr->reserve(bitPositions.size());

        std::set<uint32_t> seenBits;
        for (size_t index = 0; index < bitPositions.size(); ++index) {
            auto currentBit = bitPositions[index];
            if (auto result = seenBits.insert(currentBit); result.second) {
                arr->push_back(sbe::value::TypeTags::NumberInt64,
                               sbe::value::bitcastFrom<int64_t>(currentBit));
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
                                                   sbe::value::bitcastFrom<int32_t>(
                                                       static_cast<int32_t>(bitTestBehavior)))));

        // Build An EExpression for the numeric bitmask case. The AllSet case tests if (mask &
        // value) == mask, and AllClear case tests if (mask & value) == 0. The AnyClear and the
        // AnySet case is the negation of the AllSet and AllClear cases, respectively.
        auto numericBitTestEExpr =
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(expr->getBitMask()));
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

    generatePredicate(context, expr->path(), std::move(makePredicate));
}

// Each logical expression child is evaluated in a separate EvalFrame. Set up a new EvalFrame with a
// limit-1/coscan tree.
void pushFrameForLogicalExpressionChild(MatchExpressionVisitorContext* context,
                                        size_t numChildren) {
    if (numChildren <= 1) {
        // For logical expressions with no children, we return constant (handled in the
        // post-visitor). For expressions with 1 child, we evaluate the child within the current
        // EvalFrame.
        return;
    }

    const auto& frame = context->evalStack.topFrame();
    context->evalStack.emplaceFrame(EvalStage{}, frame.data().inputSlot);
}

// Build specified logical expression with branches stored on stack.
void buildLogicalExpression(sbe::EPrimBinary::Op op,
                            size_t numChildren,
                            MatchExpressionVisitorContext* context) {
    if (numChildren == 0) {
        // If logical expression does not have any children, constant is returned.
        generateAlwaysBoolean(context, op == sbe::EPrimBinary::logicAnd);
        return;
    } else if (numChildren == 1) {
        // For expressions with 1 child, do nothing and return. The post-visitor for the child
        // expression has already done all the necessary work.
        return;
    }

    // Move the children's outputs off of the evalStack into a vector in preparation for
    // calling generateShortCircuitingLogicalOp().
    std::vector<EvalExprStagePair> branches;
    for (size_t i = 0; i < numChildren; ++i) {
        auto [expr, stage] = context->evalStack.popFrame();
        branches.emplace_back(std::move(expr), std::move(stage));
    }
    std::reverse(branches.begin(), branches.end());

    auto& frame = context->evalStack.topFrame();
    auto&& [expr, opStage] = generateShortCircuitingLogicalOp(
        op, std::move(branches), context->planNodeId, context->slotIdGenerator);
    frame.pushExpr(std::move(expr));

    // Join frame.stage with opStage.
    frame.setStage(makeLoopJoin(frame.extractStage(), std::move(opStage), context->planNodeId));
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

    void visit(const AndMatchExpression* expr) final {
        if (expr == _context->topLevelAnd) {
            // For a top-level $and with at least one child, we evaluate each child within the
            // current EvalFrame ('frame') so that each child builds directly on top of
            // frame->stage.
            return;
        }

        // For non-top-level $and's, we evaluate each child in its own EvalFrame.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child
        invariant(matchExpr->numChildren() == 1);

        // We evaluate $elemMatch's child in a new EvalFrame. For the child's EvalFrame, we set the
        // 'stage' field to be a null tree, and we set the 'inputSlot' field to be a newly allocated
        // slot (childInputSlot). childInputSlot is a "correlated slot" that will be set up later
        // (handled in the post-visitor).
        auto childInputSlot = _context->slotIdGenerator->generate();
        _context->evalStack.emplaceFrame(EvalStage{}, childInputSlot);
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        invariant(matchExpr->numChildren() >= 1);

        // We evaluate each child in its own EvalFrame. Set up a new EvalFrame with a null tree
        // for the first child. For all of the children's EvalFrames, we set the 'inputSlot' field
        // to 'childInputSlot'. childInputSlot is a "correlated slot" that will be set up later in
        // the post-visitor (childInputSlot will be the correlated parameter of a TraverseStage).
        auto childInputSlot = _context->slotIdGenerator->generate();
        _context->evalStack.emplaceFrame(EvalStage{}, childInputSlot);
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
    void visit(const InMatchExpression* expr) final {}
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
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const NotMatchExpression* expr) final {
        invariant(expr->numChildren() == 1);
    }

    void visit(const OrMatchExpression* expr) final {
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
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

    void visit(const AndMatchExpression* expr) final {
        if (expr == _context->topLevelAnd) {
            // For a top-level $and with no children, do nothing and return. For top-level $and's
            // with at least one, we evaluate each child within the current EvalFrame.
            if (expr->numChildren() >= 1) {
                // Process the output of the last child.
                auto& frame = _context->evalStack.topFrame();
                invariant(frame.exprsCount() > 0);
                frame.setStage(makeFilter<false>(
                    frame.extractStage(), frame.popExpr().extractExpr(), _context->planNodeId));
            }
            return;
        }

        buildLogicalExpression(sbe::EPrimBinary::logicAnd, expr->numChildren(), _context);
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
        auto childInputSlot = _context->evalStack.topFrame().data().inputSlot;
        auto [filterSlot, filterStage] = [&]() {
            auto [expr, stage] = _context->evalStack.popFrame();

            if (matchExpr->getChild(0)->matchType() == MatchExpression::AND &&
                matchExpr->getChild(0)->numChildren() == 0) {
                auto childOutputSlot = _context->slotIdGenerator->generate();
                auto isObjectOrArrayExpr = sbe::makeE<sbe::EPrimBinary>(
                    sbe::EPrimBinary::logicOr,
                    sbe::makeE<sbe::EFunction>(
                        "isObject", sbe::makeEs(sbe::makeE<sbe::EVariable>(childInputSlot))),
                    sbe::makeE<sbe::EFunction>(
                        "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(childInputSlot))));
                return std::make_pair(childOutputSlot,
                                      makeProject(EvalStage{},
                                                  _context->planNodeId,
                                                  childOutputSlot,
                                                  std::move(isObjectOrArrayExpr)));
            }
            return projectEvalExpr(
                std::move(expr), std::move(stage), _context->planNodeId, _context->slotIdGenerator);
        }();

        // We're using 'kDoNotTraverseLeaf' traverse mode, so we're guaranteed that 'makePredcate'
        // will only be called once, so it's safe to capture and pass in the 'filterStage' subtree
        // here.
        auto makePredicate = [&, filterSlot = filterSlot, &filterStage = filterStage](
                                 sbe::value::SlotId inputSlot,
                                 EvalStage inputStage) -> EvalExprStagePair {
            // Generate the traverse.
            auto traverseSlot = _context->slotIdGenerator->generate();
            auto traverseStage = makeTraverse(
                // The 'filterStage' subtree was generated to read from 'childInputSlot', based on
                // the assumption that 'childInputSlot' is some correlated slot that will be made
                // available by childStages's parent. We add a projection here to 'inputStage' to
                // feed 'inputSlot' into 'childInputSlot'.
                makeProject(std::move(inputStage),
                            _context->planNodeId,
                            childInputSlot,
                            sbe::makeE<sbe::EVariable>(inputSlot)),
                makeFilter<true>(std::move(filterStage),
                                 sbe::makeE<sbe::EFunction>(
                                     "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                                 _context->planNodeId),
                childInputSlot,
                traverseSlot,
                filterSlot,
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                             sbe::makeE<sbe::EVariable>(traverseSlot),
                                             sbe::makeE<sbe::EVariable>(filterSlot)),
                sbe::makeE<sbe::EVariable>(traverseSlot),
                _context->planNodeId,
                1);

            return {traverseSlot, std::move(traverseStage)};
        };

        generatePredicate(_context,
                          matchExpr->path(),
                          std::move(makePredicate),
                          LeafTraversalMode::kDoNotTraverseLeaf);
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        auto numChildren = matchExpr->numChildren();
        invariant(numChildren >= 1);

        auto childInputSlot = _context->evalStack.topFrame().data().inputSlot;

        // Move the children's outputs off of the evalStack into a vector in preparation for
        // calling generateShortCircuitingLogicalOp().
        std::vector<EvalExprStagePair> childStages;
        for (size_t i = 0; i < numChildren; ++i) {
            auto [expr, stage] = _context->evalStack.popFrame();
            childStages.emplace_back(std::move(expr), std::move(stage));
        }
        std::reverse(childStages.begin(), childStages.end());

        auto [filterExpr, filterStage] =
            generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicAnd,
                                             std::move(childStages),
                                             _context->planNodeId,
                                             _context->slotIdGenerator);

        sbe::value::SlotId filterSlot;
        std::tie(filterSlot, filterStage) = projectEvalExpr(std::move(filterExpr),
                                                            std::move(filterStage),
                                                            _context->planNodeId,
                                                            _context->slotIdGenerator);

        // We're using 'kDoNotTraverseLeaf' traverse mode, so we're guaranteed that 'makePredcate'
        // will only be called once, so it's safe to capture and pass in the 'filterStage' subtree
        // here.
        auto makePredicate = [&,
                              filterSlot = filterSlot,
                              &filterStage = filterStage](  // NOLINT(bugprone-use-after-move)
                                 sbe::value::SlotId inputSlot,
                                 EvalStage inputStage) -> EvalExprStagePair {
            invariant(filterStage.stage);

            // Generate the traverse.
            auto traverseSlot = _context->slotIdGenerator->generate();
            auto traverseStage = makeTraverse(
                // The 'childStage' subtree was generated to read from 'childInputSlot', based
                // on the assumption that 'childInputSlot' is some correlated slot that will be
                // made available by childStages's parent. We add a projection here to 'inputStage'
                // to feed 'inputSlot' into 'childInputSlot'.
                makeProject(std::move(inputStage),
                            _context->planNodeId,
                            childInputSlot,
                            sbe::makeE<sbe::EVariable>(inputSlot)),
                makeFilter<true>(std::move(filterStage),
                                 sbe::makeE<sbe::EFunction>(
                                     "isArray", sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                                 _context->planNodeId),
                childInputSlot,
                traverseSlot,
                filterSlot,
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                             sbe::makeE<sbe::EVariable>(traverseSlot),
                                             sbe::makeE<sbe::EVariable>(filterSlot)),
                sbe::makeE<sbe::EVariable>(traverseSlot),
                _context->planNodeId,
                1);

            return {traverseSlot, std::move(traverseStage)};
        };

        generatePredicate(_context,
                          matchExpr->path(),
                          std::move(makePredicate),
                          LeafTraversalMode::kDoNotTraverseLeaf);
    }

    void visit(const EqualityMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::eq);
    }

    void visit(const ExistsMatchExpression* expr) final {
        auto makePredicate = [](sbe::value::SlotId inputSlot,
                                EvalStage inputStage) -> EvalExprStagePair {
            return {sbe::makeE<sbe::EFunction>("exists",
                                               sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                    std::move(inputStage)};
        };

        generatePredicate(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const ExprMatchExpression* matchExpr) final {
        auto& frame = _context->evalStack.topFrame();

        // The $expr expression must by applied to the current $$ROOT document, so make sure that
        // an input slot associated with the current frame is the same slot as the input slot for
        // the entire match expression we're translating.
        invariant(frame.data().inputSlot == _context->inputSlot);

        auto currentStage = stageOrLimitCoScan(frame.extractStage(), _context->planNodeId);
        auto&& [_, expr, stage] = generateExpression(_context->opCtx,
                                                     matchExpr->getExpression().get(),
                                                     std::move(currentStage.stage),
                                                     _context->slotIdGenerator,
                                                     _context->frameIdGenerator,
                                                     frame.data().inputSlot,
                                                     _context->env,
                                                     _context->planNodeId,
                                                     &currentStage.outSlots);
        auto frameId = _context->frameIdGenerator->generate();

        // We will need to convert the result of $expr to a boolean value, so we'll wrap it into an
        // expression which does exactly that.
        auto logicExpr = generateCoerceToBoolExpression(sbe::EVariable{frameId, 0});

        frame.pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, sbe::makeEs(std::move(expr)), std::move(logicExpr)));
        frame.setStage(EvalStage{std::move(stage), std::move(currentStage.outSlots)});
    }

    void visit(const GTEMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::greaterEq);
    }

    void visit(const GTMatchExpression* expr) final {
        generateComparison(_context, expr, sbe::EPrimBinary::greater);
    }

    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}

    void visit(const InMatchExpression* expr) final {
        auto equalities = expr->getEqualities();

        // Build an ArraySet for testing membership of the field in the equalities vector of the
        // InMatchExpression.
        auto [arrSetTag, arrSetVal] = sbe::value::makeNewArraySet();
        sbe::value::ValueGuard arrSetGuard{arrSetTag, arrSetVal};

        auto arrSet = sbe::value::getArraySetView(arrSetVal);

        for (auto&& equality : equalities) {

            auto [tagView, valView] = sbe::bson::convertFrom(true,
                                                             equality.rawdata(),
                                                             equality.rawdata() + equality.size(),
                                                             equality.fieldNameSize() - 1);

            // An ArraySet assumes ownership of it's values so we have to make a copy here.
            auto [tag, val] = sbe::value::copyValue(tagView, valView);
            arrSet->push_back(tag, val);
        }

        // If the InMatchExpression doesn't carry any regex patterns, we can just check if the value
        // in bound to the inputSlot is a member of the equalities set.
        if (expr->getRegexes().size() == 0) {
            auto makePredicate = [&, arrSetTag = arrSetTag, arrSetVal = arrSetVal](
                                     sbe::value::SlotId inputSlot,
                                     EvalStage inputStage) -> EvalExprStagePair {
                // Copy the ArraySet because the the sbe EConstant assmumes ownership and the
                // makePredicate function can be invoked multiple times in 'generateTraverse'.
                auto [equalitiesTag, equalitiesVal] = sbe::value::copyValue(arrSetTag, arrSetVal);

                return {sbe::makeE<sbe::EFunction>(
                            "isMember",
                            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                        sbe::makeE<sbe::EConstant>(equalitiesTag, equalitiesVal))),
                        std::move(inputStage)};
            };

            generatePredicate(_context, expr->path(), std::move(makePredicate));
            return;
        } else {
            // If the InMatchExpression contains regex patterns, then we need to handle a regex-only
            // case and a case where both equalities and regexes are present. The regex-only case is
            // handled by building a traversal stage to traverse the array of regexes and call the
            // 'regexMatch' built-in to check if the field being traversed has a value that matches
            // a regex. The combined case uses a short-circuiting limit-1/union OR stage to first
            // exhaust the equalities 'isMember' check, and then if no match is found it executes
            // the regex-only traversal stage.
            auto& regexes = expr->getRegexes();

            auto [arrTag, arrVal] = sbe::value::makeNewArray();
            sbe::value::ValueGuard arrGuard{arrTag, arrVal};

            auto arr = sbe::value::getArrayView(arrVal);

            arr->reserve(regexes.size());

            for (auto&& r : regexes) {
                auto regex = RegexMatchExpression::makeRegex(r->getString(), r->getFlags());
                arr->push_back(sbe::value::TypeTags::pcreRegex,
                               sbe::value::bitcastFrom<pcrecpp::RE*>(regex.release()));
            }

            auto makePredicate =
                [&, arrSetTag = arrSetTag, arrSetVal = arrSetVal, arrTag = arrTag, arrVal = arrVal](
                    sbe::value::SlotId inputSlot, EvalStage inputStage) -> EvalExprStagePair {
                auto regexArraySlot{_context->slotIdGenerator->generate()};
                auto regexInputSlot{_context->slotIdGenerator->generate()};
                auto regexOutputSlot{_context->slotIdGenerator->generate()};

                // Build a traverse stage that traverses the query regex pattern array. Here the
                // FROM branch binds an array constant carrying the regex patterns to a slot. Then
                // the inner branch executes 'regexMatch' once per regex.
                auto [regexTag, regexVal] = sbe::value::copyValue(arrTag, arrVal);
                auto regexFromStage = makeProject(EvalStage{},
                                                  _context->planNodeId,
                                                  regexArraySlot,
                                                  sbe::makeE<sbe::EConstant>(regexTag, regexVal));

                auto regexInnerStage =
                    makeProject(EvalStage{},
                                _context->planNodeId,
                                regexInputSlot,
                                makeFillEmptyFalse(sbe::makeE<sbe::EFunction>(
                                    "regexMatch",
                                    sbe::makeEs(sbe::makeE<sbe::EVariable>(regexArraySlot),
                                                sbe::makeE<sbe::EVariable>(inputSlot)))));

                auto regexStage = makeTraverse(
                    std::move(regexFromStage),
                    std::move(regexInnerStage),
                    regexArraySlot,
                    regexOutputSlot,
                    regexInputSlot,
                    sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                                 sbe::makeE<sbe::EVariable>(regexOutputSlot),
                                                 sbe::makeE<sbe::EVariable>(regexInputSlot)),
                    sbe::makeE<sbe::EVariable>(regexOutputSlot),
                    _context->planNodeId,
                    0);

                // If equalities are present in addition to regexes, build a limit-1/union
                // short-circuiting OR between a filter stage that checks membership of the field
                // being traversed in the equalities and the regex traverse stage
                if (equalities.size() > 0) {
                    auto [equalitiesTag, equalitiesVal] =
                        sbe::value::copyValue(arrSetTag, arrSetVal);
                    std::vector<EvalExprStagePair> branches;
                    branches.emplace_back(
                        sbe::makeE<sbe::EFunction>(
                            "isMember",
                            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                        sbe::makeE<sbe::EConstant>(equalitiesTag, equalitiesVal))),
                        EvalStage{});
                    branches.emplace_back(regexOutputSlot, std::move(regexStage));

                    return generateShortCircuitingLogicalOp(sbe::EPrimBinary::logicOr,
                                                            std::move(branches),
                                                            _context->planNodeId,
                                                            _context->slotIdGenerator);
                }

                return {regexOutputSlot, std::move(regexStage)};
            };
            generatePredicate(_context,
                              expr->path(),
                              std::move(makePredicate),
                              LeafTraversalMode::kArrayElementsOnly);
        }
    }

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
        auto makePredicate = [expr](sbe::value::SlotId inputSlot,
                                    EvalStage inputStage) -> EvalExprStagePair {
            return {makeFillEmptyFalse(sbe::makeE<sbe::EPrimBinary>(
                        sbe::EPrimBinary::eq,
                        sbe::makeE<sbe::EFunction>(
                            "mod",
                            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot),
                                        sbe::makeE<sbe::EConstant>(
                                            sbe::value::TypeTags::NumberInt64,
                                            sbe::value::bitcastFrom<int64_t>(expr->getDivisor())))),
                        sbe::makeE<sbe::EConstant>(
                            sbe::value::TypeTags::NumberInt64,
                            sbe::value::bitcastFrom<int64_t>(expr->getRemainder())))),
                    std::move(inputStage)};
        };

        generatePredicate(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const NorMatchExpression* expr) final {
        // $nor is implemented as a negation of $or. First step is to build $or expression from
        // stack.
        buildLogicalExpression(sbe::EPrimBinary::logicOr, expr->numChildren(), _context);

        // Second step is to negate the result of $or expression.
        auto& frame = _context->evalStack.topFrame();
        frame.pushExpr(makeNot(frame.popExpr().extractExpr()));
    }

    void visit(const NotMatchExpression* expr) final {
        auto& frame = _context->evalStack.topFrame();

        // Negate the result of $not's child.
        frame.pushExpr(makeNot(frame.popExpr().extractExpr()));
    }

    void visit(const OrMatchExpression* expr) final {
        buildLogicalExpression(sbe::EPrimBinary::logicOr, expr->numChildren(), _context);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makePredicate = [expr](sbe::value::SlotId inputSlot,
                                    EvalStage inputStage) -> EvalExprStagePair {
            auto regex = RegexMatchExpression::makeRegex(expr->getString(), expr->getFlags());
            auto ownedRegexVal = sbe::value::bitcastFrom<pcrecpp::RE*>(regex.release());

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

        generatePredicate(_context, expr->path(), std::move(makePredicate));
    }

    void visit(const SizeMatchExpression* expr) final {
        generateArraySize(_context, expr);
    }

    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}

    void visit(const TypeMatchExpression* expr) final {
        auto makePredicate = [expr](sbe::value::SlotId inputSlot,
                                    EvalStage inputStage) -> EvalExprStagePair {
            const MatcherTypeSet& ts = expr->typeSet();
            return {sbe::makeE<sbe::ETypeMatch>(sbe::makeE<sbe::EVariable>(inputSlot),
                                                ts.getBSONTypeMask()),
                    std::move(inputStage)};
        };

        generatePredicate(_context, expr->path(), std::move(makePredicate));
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

    void visit(const AndMatchExpression* expr) final {
        if (expr == _context->topLevelAnd) {
            // For a top-level $and, we evaluate each child within the current EvalFrame.
            // Process the output of the most recently evaluated child.
            auto& frame = _context->evalStack.topFrame();
            invariant(frame.exprsCount() > 0);
            frame.setStage(makeFilter<false>(
                frame.extractStage(), frame.popExpr().extractExpr(), _context->planNodeId));
            return;
        }

        // For non-top-level $and's, we evaluate each child in its own EvalFrame, and we
        // leave these EvalFrames on the stack until we're done evaluating all the children.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        // ElemMatchObjectMatchExpression is guaranteed to always have exactly 1 child, so we don't
        // need to do anything here.
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        const auto& frame = _context->evalStack.topFrame();

        // We leave each child's EvalFrame on the stack until we're finished evaluating all of
        // the children. Set up a new EvalFrame for the next child with a null tree and with the
        // 'inputSlot' field set to 'childInputSlot'. childInputSlot is a "correlated slot" that
        // will be set up later (handled in the post-visitor).
        _context->evalStack.emplaceFrame(EvalStage{}, frame.data().inputSlot);
    }

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

    void visit(const NorMatchExpression* expr) final {
        // We leave the EvalFrame of each child on the stack until we're done evaluating all the
        // children.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
    }

    void visit(const NotMatchExpression* expr) final {}

    void visit(const OrMatchExpression* expr) final {
        // We leave the EvalFrame of each child on the stack until we're done evaluating all the
        // children.
        pushFrameForLogicalExpressionChild(_context, expr->numChildren());
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

    // If 'inputSlot' is not present within 'relevantSlots', add it now.
    if (!std::count(relevantSlots.begin(), relevantSlots.end(), inputSlot)) {
        relevantSlots.push_back(inputSlot);
    }

    MatchExpressionVisitorContext context{opCtx,
                                          slotIdGenerator,
                                          frameIdGenerator,
                                          EvalStage{std::move(stage), std::move(relevantSlots)},
                                          inputSlot,
                                          root,
                                          env,
                                          planNodeId};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    return context.done().stage;
}
}  // namespace mongo::stage_builder
