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

#include "mongo/db/query/stage_builder/sbe/gen_filter.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/exec/sbe/util/pcre.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/matcher_type_set.h"
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
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_expression.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>

#include <s2cellid.h>

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::stage_builder {
namespace {
/**
 * A function of type 'MakePredicateFn' can be called to generate an SbExpr which applies
 * a predicate to the value found in 'inputExpr'.
 */
using MakePredicateFn = std::function<SbExpr(SbExpr inputExpr)>;

/**
 * A struct for storing context across calls to visit() methods in MatchExpressionPreVisitor,
 * MatchExpressionInVisitor, and MatchExpressionPostVisitor.
 */
struct MatchExpressionVisitorContext {
    struct MatchFrame {
        /**
         * MatchFrame's constructor has 3 parameters. 'inputExpr' provides the input source, and is
         * expected to be a local variable or a slot. 'frameId' is the FrameId of the current lambda
         * (or boost::none if there is no current lambda). By default, 'childOfElemMatchValue' is
         * false and generatePredicate() will generate a traversal for the current MatchExpression's
         * field path (using 'inputExpr' as the base of the traversal) when applying the predicate.
         * When 'childOfElemMatchValue' is set to true, generatePredicate() will ignore the current
         * MatchExpression's field path and just apply the predicate directly on 'inputExpr'.
         */
        MatchFrame(StageBuilderState& state,
                   SbExpr inputExpr,
                   boost::optional<sbe::FrameId> frameId = boost::none,
                   bool childOfElemMatchValue = false)
            : state(state),
              inputExpr(std::move(inputExpr)),
              frameId(frameId),
              childOfElemMatchValue(childOfElemMatchValue) {}

        void pushExpr(SbExpr expr) {
            exprStack.push_back(std::move(expr));
        }

        SbExpr popExpr() {
            tassert(6987609, "Expected 'exprStack' to be non-empty", !exprStack.empty());
            auto expr = std::move(exprStack.back());
            exprStack.pop_back();
            return expr;
        }

        size_t exprsCount() const {
            return exprStack.size();
        }

        StageBuilderState& state;
        SbExpr inputExpr;
        boost::optional<sbe::FrameId> frameId;
        bool childOfElemMatchValue = false;
        std::vector<SbExpr> exprStack;
    };

    MatchExpressionVisitorContext(StageBuilderState& state,
                                  boost::optional<SbSlot> rootSlot,
                                  const MatchExpression* root,
                                  const PlanStageSlots* slots,
                                  bool isFilterOverIxscan)
        : state{state}, rootSlot{rootSlot}, slots{slots}, isFilterOverIxscan{isFilterOverIxscan} {
        tassert(
            7097201, "Expected 'rootSlot' or 'slots' to be defined", rootSlot || slots != nullptr);

        // Set up the top-level MatchFrame.
        emplaceFrame(state, SbExpr{rootSlot});
    }

    SbExpr done() {
        invariant(framesCount() == 1);
        auto& frame = topFrame();

        if (frame.exprsCount() > 0) {
            invariant(frame.exprsCount() == 1);
            return frame.popExpr();
        }

        return SbExpr{};
    }

    template <typename... Args>
    void emplaceFrame(Args&&... args) {
        matchStack.emplace_back(std::forward<Args>(args)...);
    }

    MatchFrame& topFrame() {
        tassert(6987600, "Expected matchStack to be non-empty", !matchStack.empty());
        return matchStack.back();
    }

    const MatchFrame& topFrame() const {
        tassert(6987601, "Expected matchStack to be non-empty", !matchStack.empty());
        return matchStack.back();
    }

    void popFrame() {
        tassert(6987602, "Expected frame's exprStack to be empty", topFrame().exprsCount() == 0);
        matchStack.pop_back();
    }

    size_t framesCount() const {
        return matchStack.size();
    }

    StageBuilderState& state;
    std::vector<MatchFrame> matchStack;

    // The current context must be initialized either with a slot that contains the root
    // document ('rootSlot') or with the set of kField slots ('slots').
    boost::optional<SbSlot> rootSlot;
    const PlanStageSlots* slots = nullptr;
    bool isFilterOverIxscan = false;
};

enum class LeafTraversalMode {
    // Don't traverse the leaf.
    kDoNotTraverseLeaf = 0,

    // Traverse the leaf, and for arrays visit both the array's elements _and_ the array itself.
    kArrayAndItsElements = 1,

    // Traverse the leaf, and for arrays visit the array's elements but not the array itself.
    kArrayElementsOnly = 2,
};

SbExpr generateTraverseF(SbExpr inputExpr,
                         boost::optional<SbSlot> topLevelFieldSlot,
                         const sbe::MatchPath& fp,
                         FieldIndex level,
                         sbe::value::FrameIdGenerator* frameIdGenerator,
                         StageBuilderState& state,
                         const MakePredicateFn& makePredicate,
                         bool matchesNothing,
                         LeafTraversalMode mode) {
    tassert(7097202,
            "Expected an input expression or top level field",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    SbExprBuilder b(state);

    // If 'level' is currently pointing to the second last part of the field path AND the last
    // part of the field path is "", then 'childIsLeafWithEmptyName' will be true. Otherwise it
    // will be false.
    const bool childIsLeafWithEmptyName =
        (level == fp.numParts() - 2u) && fp.isPathComponentEmpty(level + 1);
    int arrayIndex = 0;
    const bool isNumericField = (level < fp.FieldRef::numParts()) &&
        fp.isNumericPathComponentStrict(level) &&
        NumberParser{}(fp.getPart(level), &arrayIndex).isOK();
    int arrayIndexNext = 0;
    const bool isNumericFieldNext = (level + 1 < fp.FieldRef::numParts()) &&
        fp.isNumericPathComponentStrict(level + 1) &&
        NumberParser{}(fp.getPart(level + 1), &arrayIndexNext).isOK();
    const bool isLeafField = (level == fp.numParts() - 1u) || childIsLeafWithEmptyName;
    const bool needsArrayCheck = (isLeafField && mode == LeafTraversalMode::kArrayAndItsElements);
    const bool needsNothingCheck = !isLeafField && matchesNothing;
    const bool isLeafNumeric =
        (isNumericField && (!isNumericFieldNext || mode == LeafTraversalMode::kDoNotTraverseLeaf));

    auto lambdaFrameId = frameIdGenerator->generate();
    auto lambdaParam = SbExpr{SbVar{lambdaFrameId, 0}};
    auto getFieldName = isNumericField ? "getFieldOrElement"_sd : "getField"_sd;
    SbExpr fieldExpr = topLevelFieldSlot
        ? SbExpr{*topLevelFieldSlot}
        : b.makeFunction(getFieldName, inputExpr.clone(), b.makeStrConstant(fp.getPart(level)));

    if (childIsLeafWithEmptyName) {
        auto frameId = frameIdGenerator->generate();
        auto getFieldValue = SbLocalVar{frameId, 0};
        auto expr = b.makeIf(b.makeFunction("isArray", getFieldValue),
                             getFieldValue,
                             b.makeFunction("getField", getFieldValue, b.makeStrConstant(""_sd)));

        fieldExpr = b.makeLet(frameId, SbExpr::makeSeq(std::move(fieldExpr)), std::move(expr));
    }

    auto resultExpr = isLeafField ? makePredicate(lambdaParam.clone())
                                  : generateTraverseF(lambdaParam.clone(),
                                                      boost::none /* topLevelFieldSlot */,
                                                      fp,
                                                      level + 1,
                                                      frameIdGenerator,
                                                      state,
                                                      makePredicate,
                                                      matchesNothing,
                                                      mode);

    if ((isLeafField && mode == LeafTraversalMode::kDoNotTraverseLeaf) ||
        (!isNumericField && isNumericFieldNext)) {
        return b.makeLet(
            lambdaFrameId, SbExpr::makeSeq(std::move(fieldExpr)), std::move(resultExpr));
    }

    // When the predicate can match Nothing, we need to do some extra work for non-leaf fields.
    if (needsNothingCheck) {
        // Add a check that will return false if the lambda's parameter is not an object. This
        // effectively allows us to skip over cases where we would be calling getField() on a scalar
        // value or an array and getting back Nothing. The subset of such cases where we should
        // return true is handled by the previous level before execution would reach here.
        auto cond = b.makeFillEmptyFalse(b.makeFunction("isObject", lambdaParam.clone()));

        resultExpr = b.makeIf(std::move(cond), std::move(resultExpr), b.makeBoolConstant(false));
    }

    auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(resultExpr));

    boost::optional<sbe::FrameId> frameId;
    SbExpr::Vector binds;

    if (needsNothingCheck) {
        frameId = frameIdGenerator->generate();
        binds.emplace_back(std::move(fieldExpr));
        fieldExpr = SbVar(*frameId, 0);
    }

    // traverseF() can return Nothing only when the lambda returns Nothing. All expressions that we
    // generate return Boolean, so there is no need for explicit fillEmpty here.
    auto traverseFExpr = isNumericField
        ? b.makeFunction(
              "magicTraverseF",
              inputExpr.clone(),
              b.makeStrConstant(fp.getPart(level)),
              b.makeInt32Constant(arrayIndex),
              std::move(lambdaExpr),
              b.makeInt32Constant(
                  (isLeafNumeric ? sbe::vm::MagicTraverse::kPreTraverse : 0) +
                  (isNumericFieldNext || isLeafField ? 0 : sbe::vm::MagicTraverse::kPostTraverse)))
        : b.makeFunction("traverseF",
                         fieldExpr.clone(),
                         std::move(lambdaExpr),
                         b.makeBoolConstant(needsArrayCheck));

    // When the predicate can match Nothing, we need to do some extra work for non-leaf fields.
    if (needsNothingCheck) {
        // If the result of getField() was Nothing or a scalar value, then don't bother traversing
        // the remaining levels of the path and just decide now if we should return true or false
        // for this value.
        traverseFExpr = b.makeIf(
            b.makeFillEmptyFalse(
                b.makeFunction("typeMatch",
                               fieldExpr.clone(),
                               b.makeInt32Constant(getBSONTypeMask(BSONType::array) |
                                                   getBSONTypeMask(BSONType::object)))),
            std::move(traverseFExpr),
            !inputExpr.isNull()
                ? b.makeNot(b.makeFillEmptyFalse(b.makeFunction("isArray", inputExpr.clone())))
                : b.makeBoolConstant(true));
    }

    if (frameId) {
        traverseFExpr = b.makeLet(*frameId, std::move(binds), std::move(traverseFExpr));
    }

    return traverseFExpr;
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
                       const sbe::MatchPath& path,
                       const MakePredicateFn& makePredicate,
                       LeafTraversalMode mode,
                       bool matchesNothing = false) {
    auto& frame = context->topFrame();

    if (frame.childOfElemMatchValue) {
        tassert(7097204, "Expected input expr to be defined", !frame.inputExpr.isNull());

        // If matchExpr's parent is a ElemMatchValueMatchExpression, then we should just
        // apply the predicate directly on 'inputExpr'. 'inputExpr' will be a lambda
        // parameter that holds the value of the ElemMatchValueMatchExpression's field path.
        frame.pushExpr(makePredicate(frame.inputExpr.clone()));
        return;
    }

    const bool isFieldPathOnRootDoc = context->framesCount() == 1;
    auto* slots = context->slots;

    boost::optional<SbSlot> topLevelFieldSlot;
    if (isFieldPathOnRootDoc && slots) {
        // If we are generating a filter over an index scan, search for a kField slot that
        // corresponds to the full path 'path'.
        if (context->isFilterOverIxscan && !path.empty()) {
            auto name = std::make_pair(PlanStageSlots::kField, path.dottedField());
            if (auto slot = slots->getIfExists(name)) {
                // We found a kField slot that matches. We don't need to perform any traversal;
                // we can just evaluate the predicate on the slot directly and return.
                frame.pushExpr(makePredicate(*slot));
                return;
            }
        }

        // Check if this operation is supposed to work only on the array elements and that the
        // navigation of the full path has been made available via the dedicated slot type; in this
        // case generate a special version of traverseF that doesn't have a runtime counterpart and
        // can only be processed by the block vectorizer.

        // TODO : Remove "&& !matchesNothing" when SERVER-87238 and SERVER-87243 have been resolved.
        if (auto slot = slots->getIfExists(
                std::make_pair(PlanStageSlots::kFilterCellField, path.dottedField()));
            slot && mode == LeafTraversalMode::kArrayElementsOnly && !matchesNothing) {
            SbExprBuilder b(context->state);
            auto lambdaFrameId = context->state.frameIdGenerator->generate();
            auto traverseFExpr = b.makeFunction(
                "blockTraverseFPlaceholder"_sd,
                SbExpr{*slot},
                b.makeLocalLambda(lambdaFrameId, makePredicate(SbExpr{SbVar{lambdaFrameId, 0}})));
            frame.pushExpr(std::move(traverseFExpr));
            return;
        } else {
            // Search for a kField slot whose path matches the first part of 'path'.
            topLevelFieldSlot =
                slots->getIfExists(std::make_pair(PlanStageSlots::kField, path.getPart(0)));
        }
    }

    tassert(7097205,
            "Expected either input expr or top-level field slot to be defined",
            !frame.inputExpr.isNull() || topLevelFieldSlot.has_value());

    frame.pushExpr(generateTraverseF(frame.inputExpr.clone(),
                                     topLevelFieldSlot,
                                     path,
                                     0, /* level */
                                     context->state.frameIdGenerator,
                                     context->state,
                                     makePredicate,
                                     matchesNothing,
                                     mode));
}

/**
 * Generates and pushes a constant boolean expression for either alwaysTrue or alwaysFalse.
 */
void generateAlwaysBoolean(MatchExpressionVisitorContext* context, bool value) {
    SbExprBuilder b(context->state);
    auto& frame = context->topFrame();

    frame.pushExpr(b.makeBoolConstant(value));
}

/**
 * Generates a path traversal SBE plan stage sub-tree for matching arrays with '$size'. Applies
 * an extra project on top of the sub-tree to filter based on user provided value.
 */
void generateArraySize(MatchExpressionVisitorContext* context,
                       const SizeMatchExpression* matchExpr) {
    SbExprBuilder b(context->state);
    int32_t size = matchExpr->getData();

    // If there's an "inputParamId" in 'matchExpr' meaning this expr got parameterized, we can
    // register a SlotId for it and use the slot directly.
    boost::optional<sbe::value::SlotId> inputParamSlotId;
    if (auto inputParam = matchExpr->getInputParamId()) {
        inputParamSlotId = context->state.registerInputParamSlot(*inputParam);
    }

    // If the expr did not get parametrized and it is less than 0, then we should always
    // return false.
    if (size < 0 && !inputParamSlotId) {
        generateAlwaysBoolean(context, false);
        return;
    }

    auto makePredicate = [&](SbExpr inputExpr) {
        auto sizeExpr =
            inputParamSlotId ? SbExpr{SbSlot{*inputParamSlotId}} : b.makeInt32Constant(size);
        return b.makeFillEmptyFalse(
            b.makeBinaryOp(abt::Operations::Eq,
                           b.makeFunction("getArraySize", std::move(inputExpr)),
                           std::move(sizeExpr)));
    };

    const auto traversalMode = LeafTraversalMode::kDoNotTraverseLeaf;
    generatePredicate(context, *matchExpr->fieldRef(), makePredicate, traversalMode);
}

/**
 * Generates a path traversal SBE plan stage sub-tree which implements the comparison match
 * expression 'expr'. The comparison itself executes using the given 'binaryOp'.
 */
void generateComparison(MatchExpressionVisitorContext* context,
                        const ComparisonMatchExpression* expr,
                        abt::Operations binaryOp) {
    auto makePredicate = [context, expr, binaryOp](SbExpr inputExpr) {
        return generateComparisonExpr(context->state, expr, binaryOp, std::move(inputExpr));
    };

    // A 'kArrayAndItsElements' traversal mode matches the following semantics: when the path we are
    // comparing is a path to an array, the comparison is considered true if it evaluates to true
    // for the array itself or for any of the array's elements.
    // However, we use 'kArrayElementsOnly' for the general case, because the comparison with the
    // array will almost always be false. There are two exceptions:
    // 1) when the 'rhs' operand is an array and
    // 2) when the 'rhs' operand is MinKey or MaxKey.
    // In the former case, the comparison we would skip by using 'kArrayElementsOnly' mode is an
    // array-to-array comparison that can return true. In the latter case, we are avoiding a
    // potential bug where traversing the path to the empty array ([]) would prevent _any_
    // comparison, meaning a comparison like {$gt: MinKey} would return false.
    const auto& rhs = expr->getData();
    const auto checkWholeArray = rhs.type() == BSONType::array || rhs.type() == BSONType::minKey ||
        rhs.type() == BSONType::maxKey;
    const auto traversalMode = checkWholeArray ? LeafTraversalMode::kArrayAndItsElements
                                               : LeafTraversalMode::kArrayElementsOnly;

    bool matchesNothing = false;
    if (rhs.type() == BSONType::null &&
        (binaryOp == abt::Operations::Eq || binaryOp == abt::Operations::Lte ||
         binaryOp == abt::Operations::Gte)) {
        matchesNothing = true;
    }

    generatePredicate(context, *expr->fieldRef(), makePredicate, traversalMode, matchesNothing);
}

/**
 * Generates a SBE plan stage sub-tree which implements the bitwise match expression 'expr'. The
 * various bit test expressions accept a numeric, BinData or position list bitmask. Here we handle
 * building an SbExpr for both the numeric and BinData or position list forms of the bitmask.
 */
void generateBitTest(MatchExpressionVisitorContext* context,
                     const BitTestMatchExpression* expr,
                     const sbe::BitTestBehavior& bitOp) {
    auto makePredicate = [context, expr, bitOp](SbExpr inputExpr) {
        return generateBitTestExpr(context->state, expr, bitOp, std::move(inputExpr));
    };

    const auto traversalMode = LeafTraversalMode::kArrayElementsOnly;
    generatePredicate(context, *expr->fieldRef(), makePredicate, traversalMode);
}

// Build specified logical expression with branches stored on stack.
void buildLogicalExpression(abt::Operations op,
                            size_t numChildren,
                            MatchExpressionVisitorContext* context) {
    SbExprBuilder b(context->state);

    if (numChildren == 0) {
        // If an $and or $or expression does not have any children, a constant is returned.
        generateAlwaysBoolean(context, op == abt::Operations::And);
        return;
    } else if (numChildren == 1) {
        // For $and or $or expressions with 1 child, do nothing and return. The post-visitor for
        // the child expression has already done all the necessary work.
        return;
    }

    auto& frame = context->topFrame();

    // Move the children's outputs off of the matchStack into a vector in preparation for
    // calling makeBooleanOpTree().
    std::vector<SbExpr> exprs;
    exprs.reserve(numChildren);
    for (size_t i = 0; i < numChildren; ++i) {
        exprs.emplace_back(frame.popExpr());
    }
    std::reverse(exprs.begin(), exprs.end());

    frame.pushExpr(b.makeBooleanOpTree(op, std::move(exprs)));
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
    void visit(const AndMatchExpression* expr) final {}
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}

    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {
        SbExprBuilder b(_context->state);
        auto numChildren = matchExpr->numChildren();
        tassert(6987603, "Expected ElemMatchObject to have exactly 1 child", numChildren == 1);

        // We evaluate $elemMatch's child in a new MatchFrame. For the child's MatchFrame, we set
        // the 'inputExpr' field to be the lambda's parameter (lambdaParam).
        auto lambdaFrameId = _context->state.frameId();
        auto lambdaParam = SbExpr{SbLocalVar{lambdaFrameId, 0}};
        _context->emplaceFrame(_context->state, std::move(lambdaParam), lambdaFrameId);
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        SbExprBuilder b(_context->state);
        auto numChildren = matchExpr->numChildren();
        tassert(6987604, "Expected ElemMatchValue to have at least 1 child", numChildren >= 1);

        // We create a new MatchFrame for evaluating $elemMatch's children. For this new MatchFrame,
        // we set the 'inputExpr' field to be the lambda's parameter (lambdaParam).
        auto lambdaFrameId = _context->state.frameId();
        auto lambdaParam = SbExpr{SbLocalVar{lambdaFrameId, 0}};
        bool childOfElemMatchValue = true;
        _context->emplaceFrame(
            _context->state, std::move(lambdaParam), lambdaFrameId, childOfElemMatchValue);
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
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalEqHashedKey* expr) final {
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
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
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
    void visit(const NorMatchExpression* expr) final {}

    void visit(const NotMatchExpression* expr) final {
        invariant(expr->numChildren() == 1);
    }

    void visit(const OrMatchExpression* expr) final {}
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
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(4822878,
                  str::stream() << "Unsupported match expression in SBE stage builder: "
                                << expr->matchType());
    }

    MatchExpressionVisitorContext* _context;
};

std::tuple<SbExpr, bool, bool, bool> _generateInExprInternal(StageBuilderState& state,
                                                             const InMatchExpression* expr) {
    SbExprBuilder b(state);

    bool exprIsParameterized = static_cast<bool>(expr->getInputParamId());

    // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
    // register a SlotId for it and use the slot directly. Note we don't auto-parameterize
    // $in if it contains null, regexes, or nested arrays or objects.
    if (exprIsParameterized) {
        auto var = SbExpr{SbSlot{state.registerInputParamSlot(*expr->getInputParamId())}};
        return std::make_tuple(std::move(var), false, false, false);
    }

    sbe::InList* inList = state.makeOwnedInList(expr);

    auto tag = sbe::value::TypeTags::inList;
    auto val = sbe::value::bitcastFrom<sbe::InList*>(inList);
    const bool owned = false;

    auto var = SbExpr{SbSlot{state.env->registerSlot(tag, val, owned, state.slotIdGenerator)}};

    return std::make_tuple(std::move(var), expr->hasArray(), expr->hasObject(), expr->hasNull());
}

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
        buildLogicalExpression(abt::Operations::And, expr->numChildren(), _context);
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
        SbExprBuilder b(_context->state);
        auto numChildren = matchExpr->numChildren();
        tassert(6987605, "Expected ElemMatchObject to have exactly 1 child", numChildren == 1);
        tassert(
            6987606, "Expected frameId to be defined", _context->topFrame().frameId.has_value());

        auto lambdaFrameId = *_context->topFrame().frameId;
        auto lambdaParam = SbLocalVar{lambdaFrameId, 0};

        auto lambdaBodyExpr = b.makeBooleanOpTree(
            abt::Operations::And,
            b.makeFunction("typeMatch",
                           lambdaParam,
                           b.makeInt32Constant(getBSONTypeMask(BSONType::array) |
                                               getBSONTypeMask(BSONType::object))),
            _context->topFrame().popExpr());

        _context->popFrame();

        auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(lambdaBodyExpr));

        auto makePredicate = [&](SbExpr inputExpr) {
            return b.makeFillEmptyFalse(
                b.makeBooleanOpTree(abt::Operations::And,
                                    b.makeFunction("isArray", inputExpr.clone()),
                                    b.makeFunction("traverseF",
                                                   inputExpr.clone(),
                                                   std::move(lambdaExpr),
                                                   b.makeBoolConstant(false))));
        };

        const auto traversalMode = LeafTraversalMode::kDoNotTraverseLeaf;
        generatePredicate(_context, *matchExpr->fieldRef(), makePredicate, traversalMode);
    }

    void visit(const ElemMatchValueMatchExpression* matchExpr) final {
        SbExprBuilder b(_context->state);
        auto numChildren = matchExpr->numChildren();
        tassert(6987607, "Expected ElemMatchValue to have at least 1 child", numChildren >= 1);
        tassert(
            6987608, "Expected frameId to be defined", _context->topFrame().frameId.has_value());

        auto lambdaFrameId = *_context->topFrame().frameId;

        // Move the children's outputs off of the expr stack into a vector in preparation for
        // calling makeBooleanOpTree().
        std::vector<SbExpr> exprs;
        exprs.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            exprs.emplace_back(_context->topFrame().popExpr());
        }
        std::reverse(exprs.begin(), exprs.end());

        _context->popFrame();

        auto lambdaBodyExpr = b.makeBooleanOpTree(abt::Operations::And, std::move(exprs));

        auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(lambdaBodyExpr));

        auto makePredicate = [&](SbExpr inputExpr) {
            return b.makeFillEmptyFalse(
                b.makeBooleanOpTree(abt::Operations::And,
                                    b.makeFunction("isArray", inputExpr.clone()),
                                    b.makeFunction("traverseF",
                                                   inputExpr.clone(),
                                                   std::move(lambdaExpr),
                                                   b.makeBoolConstant(false))));
        };

        const auto traversalMode = LeafTraversalMode::kDoNotTraverseLeaf;
        generatePredicate(_context, *matchExpr->fieldRef(), makePredicate, traversalMode);
    }

    void visit(const EqualityMatchExpression* expr) final {
        generateComparison(_context, expr, abt::Operations::Eq);
    }

    void visit(const ExistsMatchExpression* expr) final {
        SbExprBuilder b(_context->state);

        auto makePredicate = [this, &b](SbExpr inputExpr) {
            return b.makeFunction("exists", std::move(inputExpr));
        };

        const auto traversalMode = LeafTraversalMode::kDoNotTraverseLeaf;
        generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode);
    }

    void visit(const ExprMatchExpression* matchExpr) final {
        SbExprBuilder b(_context->state);
        auto& frame = _context->topFrame();

        // The $expr expression is always applied to the current $$ROOT document.
        auto expr = generateExpression(_context->state,
                                       matchExpr->getExpression().get(),
                                       _context->rootSlot,
                                       *_context->slots);

        // Convert the result of the '{$expr: ..}' expression to a boolean value.
        frame.pushExpr(b.makeFillEmptyFalse(b.makeFunction("coerceToBool"_sd, std::move(expr))));
    }

    void visit(const GTEMatchExpression* expr) final {
        generateComparison(_context, expr, abt::Operations::Gte);
    }

    void visit(const GTMatchExpression* expr) final {
        generateComparison(_context, expr, abt::Operations::Gt);
    }

    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}

    void visit(const InMatchExpression* expr) final {
        SbExprBuilder b(_context->state);
        bool exprIsParameterized = static_cast<bool>(expr->getInputParamId());

        auto [equalities, hasArray, hasObject, hasNull] =
            _generateInExprInternal(_context->state, expr);

        auto equalitiesExpr = std::move(equalities);

        const auto traversalMode = hasArray ? LeafTraversalMode::kArrayAndItsElements
                                            : LeafTraversalMode::kArrayElementsOnly;

        if (exprIsParameterized || expr->getRegexes().size() == 0) {
            auto makePredicate = [&, hasNull = hasNull](SbExpr inputExpr) {
                // We have to match nulls and missing if a 'null' is present in equalities.
                auto valueExpr = !hasNull ? std::move(inputExpr)
                                          : b.makeIf(b.generateNullOrMissing(inputExpr.clone()),
                                                     b.makeNullConstant(),
                                                     inputExpr.clone());

                return b.makeFunction("isMember", std::move(valueExpr), std::move(equalitiesExpr));
            };

            generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode, hasNull);
            return;
        }

        // If the InMatchExpression contains regex patterns, then we need to handle the regex-only
        // case, and we also must handle the case where both equalities and regexes are present. For
        // the regex-only case, we call regexMatch() to see if any of the values match against any
        // of the regexes, and we also call isMember() to see if any of the values are of type
        // 'bsonRegex' and are considered equal to any of the regexes. For the case where both
        // regexes and equalities are present, we use the "logicOr" operator to combine the logic
        // for equalities with the logic for regexes.
        auto [pcreArrTag, pcreArrVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard pcreArrGuard{pcreArrTag, pcreArrVal};
        auto pcreArr = sbe::value::getArrayView(pcreArrVal);

        auto [regexSetTag, regexSetVal] = sbe::value::makeNewArraySet();
        sbe::value::ValueGuard regexArrSetGuard{regexSetTag, regexSetVal};
        auto regexArrSet = sbe::value::getArraySetView(regexSetVal);

        if (auto& regexes = expr->getRegexes(); regexes.size() > 0) {
            pcreArr->reserve(regexes.size());

            for (auto&& r : regexes) {
                auto [pcreRegexTag, pcreRegexVal] =
                    sbe::makeNewPcreRegex(r->getString(), r->getFlags());
                pcreArr->push_back(pcreRegexTag, pcreRegexVal);

                auto [regexSetTag, regexSetVal] =
                    sbe::value::makeNewBsonRegex(r->getString(), r->getFlags());
                regexArrSet->push_back(regexSetTag, regexSetVal);
            }
        }

        auto pcreRegexesConstant = b.makeConstant(pcreArrTag, pcreArrVal);
        pcreArrGuard.reset();

        auto regexSetConstant = b.makeConstant(regexSetTag, regexSetVal);
        regexArrSetGuard.reset();

        auto makePredicate = [&, hasNull = hasNull](SbExpr inputExpr) {
            auto resultExpr = b.makeBooleanOpTree(
                abt::Operations::Or,
                b.makeFillEmptyFalse(
                    b.makeFunction("isMember", inputExpr.clone(), std::move(regexSetConstant))),
                b.makeFillEmptyFalse(b.makeFunction(
                    "regexMatch", std::move(pcreRegexesConstant), inputExpr.clone())));

            if (expr->getEqualities().size() > 0) {
                // We have to match nulls and missing if a 'null' is present in equalities.
                if (hasNull) {
                    inputExpr = b.makeIf(b.generateNullOrMissing(inputExpr.clone()),
                                         b.makeNullConstant(),
                                         inputExpr.clone());
                }

                resultExpr = b.makeBooleanOpTree(
                    abt::Operations::Or,
                    b.makeFunction("isMember", std::move(inputExpr), std::move(equalitiesExpr)),
                    std::move(resultExpr));
            }

            return resultExpr;
        };

        generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode, hasNull);
    }

    void translateExprComparison(const ComparisonMatchExpressionBase* expr) {
        SbExprBuilder b(_context->state);

        ExpressionCompare::CmpOp cmpOp = [&]() {
            switch (expr->matchType()) {
                case MatchExpression::MatchType::INTERNAL_EXPR_EQ:
                    return ExpressionCompare::CmpOp::EQ;
                case MatchExpression::MatchType::INTERNAL_EXPR_GT:
                    return ExpressionCompare::CmpOp::GT;
                case MatchExpression::MatchType::INTERNAL_EXPR_GTE:
                    return ExpressionCompare::CmpOp::GTE;
                case MatchExpression::MatchType::INTERNAL_EXPR_LT:
                    return ExpressionCompare::CmpOp::LT;
                case MatchExpression::MatchType::INTERNAL_EXPR_LTE:
                    return ExpressionCompare::CmpOp::LTE;
                default:
                    // Only $expr expressions supported.
                    MONGO_UNREACHABLE_TASSERT(6205800);
            }
        }();

        // We want to translate this into an SBE expression that returns true if LHS is
        // an array, and otherwise returns the result of the comparison.
        //
        // We accomplish by generating the following expression:
        //   let [l1.0 = <lhs>] in
        //     (isArray(l1.0) ?: false) || <<expr for {$cmpOp: [l1.0, <rhs>]}>>
        auto& state = _context->state;

        const auto frameId = state.frameIdGenerator->generate();
        auto lhsVar = SbExpr{SbLocalVar{frameId, 0}};

        auto [rhsTag, rhsVal] = sbe::value::makeValue(Value(expr->getData()));

        // Generate the comparison expression and coerce the result to boolean.
        auto translatedCmpExpr =
            generateExpressionCompare(state, cmpOp, lhsVar.clone(), b.makeConstant(rhsTag, rhsVal));

        auto isArrayExpr =
            b.makeBooleanOpTree(abt::Operations::Or,
                                b.makeFillEmptyFalse(b.makeFunction("isArray", lhsVar.clone())),
                                std::move(translatedCmpExpr));

        // Now generate the actual field path expression for the LHS.
        FieldPath fp("CURRENT." + std::string{expr->fieldRef()->dottedField()});

        auto translatedFieldPathExpr = generateExpressionFieldPath(
            _context->state, fp, boost::none, _context->rootSlot, *_context->slots);

        auto cmpWArrayCheckExpr = b.makeLet(
            frameId, SbExpr::makeSeq(std::move(translatedFieldPathExpr)), std::move(isArrayExpr));

        _context->topFrame().pushExpr(std::move(cmpWArrayCheckExpr));
    }

    void visit(const InternalExprEqMatchExpression* expr) final {
        translateExprComparison(expr);
    }
    void visit(const InternalExprGTMatchExpression* expr) final {
        translateExprComparison(expr);
    }
    void visit(const InternalExprGTEMatchExpression* expr) final {
        translateExprComparison(expr);
    }
    void visit(const InternalExprLTMatchExpression* expr) final {
        translateExprComparison(expr);
    }
    void visit(const InternalExprLTEMatchExpression* expr) final {
        translateExprComparison(expr);
    }

    void visit(const InternalEqHashedKey* expr) final {}
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
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
        generateComparison(_context, expr, abt::Operations::Lte);
    }

    void visit(const LTMatchExpression* expr) final {
        generateComparison(_context, expr, abt::Operations::Lt);
    }

    void visit(const ModMatchExpression* expr) final {
        // The mod function returns the result of the mod operation between the operand and
        // given divisor, so construct an expression to then compare the result of the operation
        // to the given remainder.
        auto makePredicate = [context = _context, expr](SbExpr inputExpr) {
            return generateModExpr(context->state, expr, std::move(inputExpr));
        };

        const auto traversalMode = LeafTraversalMode::kArrayElementsOnly;
        generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode);
    }

    void visit(const NorMatchExpression* expr) final {
        SbExprBuilder b(_context->state);

        // $nor is implemented as a negation of $or. First step is to build $or expression from
        // stack.
        buildLogicalExpression(abt::Operations::Or, expr->numChildren(), _context);

        // Second step is to negate the result of $or expression.
        // Here we discard the index value of the state even if it was set by expressions below NOR.
        // This matches the behaviour of classic engine, which does not pass 'MatchDetails' object
        // to children of NOR and thus does not get any information on 'elemMatchKey' from them.
        auto& frame = _context->topFrame();
        frame.pushExpr(b.makeNot(frame.popExpr()));
    }

    void visit(const NotMatchExpression* expr) final {
        SbExprBuilder b(_context->state);
        auto& frame = _context->topFrame();

        // Negate the result of $not's child.
        // Here we discard the index value of the state even if it was set by expressions below NOT.
        // This matches the behaviour of classic engine, which does not pass 'MatchDetails' object
        // to children of NOT and thus does not get any information on 'elemMatchKey' from them.
        frame.pushExpr(b.makeNot(frame.popExpr()));
    }

    void visit(const OrMatchExpression* expr) final {
        buildLogicalExpression(abt::Operations::Or, expr->numChildren(), _context);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makePredicate = [context = _context, expr](SbExpr inputExpr) {
            return generateRegexExpr(context->state, expr, std::move(inputExpr));
        };

        const auto traversalMode = LeafTraversalMode::kArrayElementsOnly;
        generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode);
    }

    void visit(const SizeMatchExpression* expr) final {
        generateArraySize(_context, expr);
    }

    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}

    void visit(const TypeMatchExpression* expr) final {
        SbExprBuilder b(_context->state);

        // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
        // register a SlotId for it and use the slot directly. Note that we don't auto-parameterize
        // if the type set contains 'BSONType::array'.
        if (auto typeMaskParam = expr->getInputParamId()) {
            auto typeMaskSlot = SbSlot{_context->state.registerInputParamSlot(*typeMaskParam)};
            auto makePredicate = [&](SbExpr inputExpr) {
                return b.makeFillEmptyFalse(
                    b.makeFunction("typeMatch", std::move(inputExpr), typeMaskSlot));
            };

            const auto traversalMode = LeafTraversalMode::kArrayElementsOnly;
            generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode);
            return;
        }

        const auto traversalMode = expr->typeSet().hasType(BSONType::array)
            ? LeafTraversalMode::kDoNotTraverseLeaf
            : LeafTraversalMode::kArrayElementsOnly;

        auto makePredicate = [&](SbExpr inputExpr) {
            const MatcherTypeSet& ts = expr->typeSet();
            return b.makeFillEmptyFalse(b.makeFunction(
                "typeMatch", std::move(inputExpr), b.makeInt32Constant(ts.getBSONTypeMask())));
        };

        generatePredicate(_context, *expr->fieldRef(), makePredicate, traversalMode);
    }

    void visit(const WhereMatchExpression* expr) final {
        auto& frame = _context->topFrame();
        frame.pushExpr(generateWhereExpr(_context->state, expr, frame.inputExpr.clone()));
    }

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
    MatchExpressionInVisitor(MatchExpressionVisitorContext*) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(const ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalEqHashedKey* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
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
    void visit(const OrMatchExpression* expr) final {}
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}
};
}  // namespace

SbExpr generateFilter(StageBuilderState& state,
                      const MatchExpression* root,
                      boost::optional<SbSlot> rootSlot,
                      const PlanStageSlots& slots,
                      bool isFilterOverIxscan) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return SbExpr{};
    }

    MatchExpressionVisitorContext context{state, rootSlot, root, &slots, isFilterOverIxscan};

    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};

    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);

    return context.done();
}

std::pair<sbe::value::TypeTags, sbe::value::Value> convertBitTestBitPositions(
    const BitTestMatchExpression* expr) {
    const auto& bitPositions = expr->getBitPositions();

    // Build an array set of bit positions for the bitmask, and remove duplicates in the
    // bitPositions vector since duplicates aren't handled in the match expression parser by
    // checking if an item has already been seen.
    auto [bitPosTag, bitPosVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard arrGuard{bitPosTag, bitPosVal};

    auto arr = sbe::value::getArrayView(bitPosVal);
    if (bitPositions.size()) {
        arr->reserve(bitPositions.size());

        std::set<uint32_t> seenBits;
        for (size_t index = 0; index < bitPositions.size(); ++index) {
            auto currentBit = bitPositions[index];
            if (auto result = seenBits.insert(currentBit); result.second) {
                arr->push_back(sbe::value::TypeTags::NumberInt64,
                               sbe::value::bitcastFrom<int64_t>(currentBit));
            }
        }
    }

    arrGuard.reset();
    return {bitPosTag, bitPosVal};
}

namespace {
template <typename ExpressionType>
using ValueExpressionFn = std::function<ExpressionType(sbe::value::TypeTags, sbe::value::Value)>;

/*
 * Converts SBE comparison operation and value to an expression.
 */
template <typename BuilderType, typename ExpressionType>
ExpressionType generateComparisonExpr(BuilderType& b,
                                      sbe::value::TypeTags tag,
                                      sbe::value::Value val,
                                      abt::Operations binaryOp,
                                      ExpressionType inputExpr,
                                      ValueExpressionFn<ExpressionType> makeValExpr) {
    // Most commonly the comparison does not do any kind of type conversions (i.e. 12 > "10" does
    // not evaluate to true as we do not try to convert a string to a number). Internally, SBE
    // returns Nothing for mismatched types. However, there is a wrinkle with MQL (and there always
    // is one). We can compare any type to MinKey or MaxKey type and expect a true/false answer.
    if (tag == sbe::value::TypeTags::MinKey) {
        switch (binaryOp) {
            case abt::Operations::Eq:
            case abt::Operations::Neq:
                break;
            case abt::Operations::Gt:
                return b.makeNot(
                    b.makeFunction("isMinKey", b.makeFillEmptyNull(std::move(inputExpr))));
            case abt::Operations::Gte:
                return b.makeBoolConstant(true);
            case abt::Operations::Lt:
                return b.makeBoolConstant(false);
            case abt::Operations::Lte:
                return b.makeFunction("isMinKey", b.makeFillEmptyNull(std::move(inputExpr)));
            default:
                MONGO_UNREACHABLE_TASSERT(8217105);
        }
    } else if (tag == sbe::value::TypeTags::MaxKey) {
        switch (binaryOp) {
            case abt::Operations::Eq:
            case abt::Operations::Neq:
                break;
            case abt::Operations::Gt:
                return b.makeBoolConstant(false);
            case abt::Operations::Gte:
                return b.makeFunction("isMaxKey", b.makeFillEmptyNull(std::move(inputExpr)));
            case abt::Operations::Lt:
                return b.makeNot(
                    b.makeFunction("isMaxKey", b.makeFillEmptyNull(std::move(inputExpr))));
            case abt::Operations::Lte:
                return b.makeBoolConstant(true);
            default:
                MONGO_UNREACHABLE_TASSERT(8217101);
        }
    } else if (tag == sbe::value::TypeTags::Null) {
        // When comparing to null we have to consider missing.
        inputExpr = b.buildMultiBranchConditional(
            typename BuilderType::CaseValuePair{b.generateNullOrMissing(b.cloneExpr(inputExpr)),
                                                b.makeNullConstant()},
            b.cloneExpr(inputExpr));

        return b.makeFillEmptyFalse(
            b.makeBinaryOp(binaryOp, std::move(inputExpr), b.makeNullConstant()));
    } else if (sbe::value::isNaN(tag, val)) {
        // Construct an expression to perform a NaN check.
        switch (binaryOp) {
            case abt::Operations::Eq:
            case abt::Operations::Gte:
            case abt::Operations::Lte:
                // If 'rhs' is NaN, then return whether the lhs is NaN.
                return b.makeFillEmptyFalse(b.makeFunction("isNaN", std::move(inputExpr)));
            case abt::Operations::Lt:
            case abt::Operations::Gt:
                // Always return false for non-equality operators.
                return b.makeBoolConstant(false);
            default:
                tasserted(5449400,
                          str::stream() << "Could not construct expression for comparison op "
                                        << abt::toStringData(binaryOp));
        }
    }

    return b.makeFillEmptyFalse(
        b.makeBinaryOp(binaryOp, std::move(inputExpr), makeValExpr(tag, val)));
}
}  // namespace

SbExpr generateComparisonExpr(StageBuilderState& state,
                              const ComparisonMatchExpression* expr,
                              abt::Operations binaryOp,
                              SbExpr inputExpr) {
    SbExprBuilder b(state);

    const auto& rhs = expr->getData();
    auto [tagView, valView] = sbe::bson::convertFrom<true>(
        rhs.rawdata(), rhs.rawdata() + rhs.size(), rhs.fieldNameSize() - 1);

    ValueExpressionFn<SbExpr> makeValExpr = [&](sbe::value::TypeTags tag, sbe::value::Value val) {
        if (auto inputParam = expr->getInputParamId()) {
            return SbExpr{SbSlot{state.registerInputParamSlot(*inputParam)}};
        }
        auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
        return b.makeConstant(copyTag, copyVal);
    };

    return generateComparisonExpr(
        b, tagView, valView, binaryOp, std::move(inputExpr), std::move(makeValExpr));
}

SbExpr generateInExpr(StageBuilderState& state, const InMatchExpression* expr, SbExpr inputExpr) {
    SbExprBuilder b(state);
    tassert(6988283,
            "'generateInExpr' supports only parameterized queries or the ones without regexes.",
            static_cast<bool>(expr->getInputParamId()) || !expr->hasRegex());

    auto [equalities, hasArray, hasObject, hasNull] = _generateInExprInternal(state, expr);

    return b.makeFunction("isMember", std::move(inputExpr), std::move(equalities));
}

SbExpr generateBitTestExpr(StageBuilderState& state,
                           const BitTestMatchExpression* expr,
                           const sbe::BitTestBehavior& bitOp,
                           SbExpr inputExpr) {
    SbExprBuilder b(state);

    // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
    // register a SlotId for it and use the slot directly.
    SbExpr bitPosExpr = [&]() -> SbExpr {
        if (auto bitPosParamId = expr->getBitPositionsParamId()) {
            auto bitPosSlot = SbSlot{state.registerInputParamSlot(*bitPosParamId)};
            return bitPosSlot;
        } else {
            auto [bitPosTag, bitPosVal] = convertBitTestBitPositions(expr);
            return b.makeConstant(bitPosTag, bitPosVal);
        }
    }();

    // An EExpression for the BinData and position list for the binary case of
    // BitTestMatchExpressions. This function will be applied to values carrying BinData
    // elements.
    auto binaryBitTestExpr = b.makeFunction("bitTestPosition"_sd,
                                            std::move(bitPosExpr),
                                            inputExpr.clone(),
                                            b.makeInt32Constant(static_cast<int32_t>(bitOp)));

    // Build An EExpression for the numeric bitmask case. The AllSet case tests if (mask &
    // value) == mask, and AllClear case tests if (mask & value) == 0. The AnyClear and
    // AnySet cases are the negation of the AllSet and AllClear cases, respectively.
    auto numericBitTestFnName = [&]() {
        if (bitOp == sbe::BitTestBehavior::AllSet || bitOp == sbe::BitTestBehavior::AnyClear) {
            return "bitTestMask"_sd;
        }
        if (bitOp == sbe::BitTestBehavior::AllClear || bitOp == sbe::BitTestBehavior::AnySet) {
            return "bitTestZero"_sd;
        }
        MONGO_UNREACHABLE_TASSERT(5610200);
    }();

    SbExpr bitMaskExpr = [&]() -> SbExpr {
        if (auto bitMaskParamId = expr->getBitMaskParamId()) {
            auto bitMaskSlot = SbSlot{state.registerInputParamSlot(*bitMaskParamId)};
            return bitMaskSlot;
        } else {
            return b.makeInt64Constant(expr->getBitMask());
        }
    }();
    // Convert the value to a 64-bit integer, and then pass the converted value along with the mask
    // to the appropriate bit-test function. If the value cannot be losslessly converted to a 64-bit
    // integer, this expression will return Nothing.
    auto numericBitTestExpr =
        b.makeFunction(numericBitTestFnName,
                       std::move(bitMaskExpr),
                       b.makeNumericConvert(inputExpr.clone(), sbe::value::TypeTags::NumberInt64));

    // For the AnyClear and AnySet cases, negate the output of the bit-test function.
    if (bitOp == sbe::BitTestBehavior::AnyClear || bitOp == sbe::BitTestBehavior::AnySet) {
        numericBitTestExpr = b.makeNot(std::move(numericBitTestExpr));
    }

    // isBinData and numericBitTestExpr might produce Nothing, so we wrap everything with
    // makeFillEmptyFalse().
    return b.makeFillEmptyFalse(b.makeIf(b.makeFunction("isBinData"_sd, std::move(inputExpr)),
                                         std::move(binaryBitTestExpr),
                                         std::move(numericBitTestExpr)));
}

/**
 * Generates the following plan for $mod:
 * (mod(convert ( trunc(input), int64), divisor) == remainder) ?: false
 *
 * When 'inputExpr' is NaN or inf, trunc() remains unmodified and lossless conversion will return
 * Nothing, so the final result becomes false.
 */
SbExpr generateModExpr(StageBuilderState& state, const ModMatchExpression* expr, SbExpr inputExpr) {
    SbExprBuilder b(state);

    auto& dividend = inputExpr;
    auto truncatedArgument = b.makeNumericConvert(b.makeFunction("trunc"_sd, dividend.clone()),
                                                  sbe::value::TypeTags::NumberInt64);
    tassert(6142202,
            "Either both divisor and remainer are parameterized or none",
            (expr->getDivisorInputParamId() && expr->getRemainderInputParamId()) ||
                (!expr->getDivisorInputParamId() && !expr->getRemainderInputParamId()));
    // If there's related input param ids in this expr, we can register SlotIds for them, and use
    // generated slots directly.
    SbExpr divisorExpr = [&]() -> SbExpr {
        if (auto divisorParam = expr->getDivisorInputParamId()) {
            auto divisorSlot = SbSlot{state.registerInputParamSlot(*divisorParam)};
            return divisorSlot;
        } else {
            return b.makeInt64Constant(expr->getDivisor());
        }
    }();
    SbExpr remainderExpr = [&]() -> SbExpr {
        if (auto remainderParam = expr->getRemainderInputParamId()) {
            auto remainderSlot = SbSlot{state.registerInputParamSlot(*remainderParam)};
            return remainderSlot;
        } else {
            return b.makeInt64Constant(expr->getRemainder());
        }
    }();
    return b.makeFillEmptyFalse(b.makeBinaryOp(
        abt::Operations::Eq,
        b.makeFunction("mod"_sd, std::move(truncatedArgument), std::move(divisorExpr)),
        std::move(remainderExpr)));
}

SbExpr generateRegexExpr(StageBuilderState& state,
                         const RegexMatchExpression* expr,
                         SbExpr inputExpr) {
    SbExprBuilder b(state);

    tassert(6142203,
            "Either both sourceRegex and compiledRegex are parameterized or none",
            (expr->getSourceRegexInputParamId() && expr->getCompiledRegexInputParamId()) ||
                (!expr->getSourceRegexInputParamId() && !expr->getCompiledRegexInputParamId()));
    SbExpr bsonRegexExpr = [&]() -> SbExpr {
        if (auto sourceRegexParam = expr->getSourceRegexInputParamId()) {
            auto sourceRegexSlot = SbSlot{state.registerInputParamSlot(*sourceRegexParam)};
            return sourceRegexSlot;
        } else {
            auto [bsonRegexTag, bsonRegexVal] =
                sbe::value::makeNewBsonRegex(expr->getString(), expr->getFlags());
            return b.makeConstant(bsonRegexTag, bsonRegexVal);
        }
    }();

    SbExpr compiledRegexExpr = [&]() -> SbExpr {
        if (auto compiledRegexParam = expr->getCompiledRegexInputParamId()) {
            auto compiledRegexSlot = SbSlot{state.registerInputParamSlot(*compiledRegexParam)};
            return compiledRegexSlot;
        } else {
            auto [compiledRegexTag, compiledRegexVal] =
                sbe::makeNewPcreRegex(expr->getString(), expr->getFlags());
            return b.makeConstant(compiledRegexTag, compiledRegexVal);
        }
    }();

    auto resultExpr = b.makeBooleanOpTree(
        abt::Operations::Or,
        b.makeFillEmptyFalse(
            b.makeBinaryOp(abt::Operations::Eq, inputExpr.clone(), std::move(bsonRegexExpr))),
        b.makeFillEmptyFalse(
            b.makeFunction("regexMatch", std::move(compiledRegexExpr), inputExpr.clone())));

    return resultExpr;
}

SbExpr generateWhereExpr(StageBuilderState& state,
                         const WhereMatchExpression* expr,
                         SbExpr inputExpr) {
    SbExprBuilder b(state);

    // Generally speaking, this visitor is non-destructive and does not mutate the MatchExpression
    // tree. However, in order to apply an optimization to avoid making a copy of the 'JsFunction'
    // object stored within 'WhereMatchExpression', we can transfer its ownership from the match
    // expression node into the SBE plan. Hence, we need to drop the const qualifier. This should be
    // a safe operation, given that the match expression tree is allocated on the heap, and this
    // visitor has exclusive access to this tree (after it has been translated into an SBE tree,
    // it's no longer used).
    auto predicate =
        b.makeConstant(sbe::value::TypeTags::jsFunction,
                       sbe::value::bitcastFrom<JsFunction*>(
                           const_cast<WhereMatchExpression*>(expr)->extractPredicate().release()));

    // If there's an "inputParamId" in this expr meaning this expr got parameterized, we can
    // register a SlotId for it and use the slot directly.
    if (auto inputParam = expr->getInputParamId()) {
        auto inputParamSlot = SbSlot{state.registerInputParamSlot(*inputParam)};
        return b.makeFunction("runJsPredicate", inputParamSlot, std::move(inputExpr));
    } else {
        return b.makeFunction("runJsPredicate", std::move(predicate), std::move(inputExpr));
    }
}
}  // namespace mongo::stage_builder
