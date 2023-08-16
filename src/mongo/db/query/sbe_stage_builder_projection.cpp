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

#include <cstddef>
#include <list>
#include <memory>
#include <stack>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/makeobj_enums.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_ast_visitor.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo::stage_builder {
namespace {
/**
 * Enum desribing mode in which projection for the field must be evaluated.
 */
enum class EvalMode {
    // Field should be included in the resulting object with no modification.
    KeepField,
    // Field should be excluded from the resulting object.
    DropField,
    // Set field value with an EvalExpr.
    EvaluateField,
};

/**
 * A 'ProjectEval' contains an EvalMode ('mode') and an EvalExpr ('expr'). If 'mode' is equal
 * to 'EvalMode::EvaluateField' then 'expr' will be non-null, otherwise 'expr' will be null.
 */
struct ProjectEval {
    ProjectEval(EvalMode mode, EvalExpr expr) : mode(mode), expr(std::move(expr)) {}

    EvalMode mode;
    EvalExpr expr;
};

/**
 * Stores the necessary context needed while visiting each node in the projection tree.
 */
struct ProjectionVisitorContext {
    /**
     * Represents current projection level. Created each time visitor encounters path projection.
     */
    struct NestedLevel {
        NestedLevel(StageBuilderState& state,
                    EvalExpr inputExpr,
                    boost::optional<sbe::FrameId> lambdaFrame)
            : state(state), inputExpr(std::move(inputExpr)), lambdaFrame(std::move(lambdaFrame)) {}

        EvalExpr getInputEvalExpr() const {
            return inputExpr.clone();
        }
        std::unique_ptr<sbe::EExpression> getInputExpr() const {
            return inputExpr.getExpr(state);
        }

        EvalExpr extractInputEvalExpr() {
            return std::move(inputExpr);
        }
        std::unique_ptr<sbe::EExpression> extractInputExpr() {
            auto evalExpr = extractInputEvalExpr();
            return evalExpr.extractExpr(state);
        }

        StageBuilderState& state;
        // The input expression for the current level. This is the parent sub-document for each of
        // the projected fields at the current level. 'inputExpr' can be a slot or a local variable.
        EvalExpr inputExpr;
        // The lambda frame associated with the current level.
        boost::optional<sbe::FrameId> lambdaFrame;
        // Vector containing operations for the current level. There are three types of operations
        // (see EvalMode enum for details).
        std::vector<ProjectEval> evals;
        // Whether or not any subtree of this level has a computed field.
        bool subtreeContainsComputedField = false;
    };

    ProjectionVisitorContext(StageBuilderState& state,
                             projection_ast::ProjectType projectType,
                             EvalExpr inputExpr,
                             const PlanStageSlots* slots)
        : state(state), projectType(projectType), inputExpr(std::move(inputExpr)), slots(slots) {}

    size_t numLevels() const {
        return levels.size();
    }

    bool levelsEmpty() const {
        return levels.empty();
    }

    auto& topLevel() {
        tassert(7580707, "Expected 'levels' to not be empty", !levels.empty());
        return levels.top();
    }

    bool getSubtreeContainsComputedField() {
        return !levels.empty() ? topLevel().subtreeContainsComputedField : containsComputedField;
    }

    void setSubtreeContainsComputedField(bool val) {
        if (!levels.empty()) {
            topLevel().subtreeContainsComputedField = val;
        } else {
            containsComputedField = val;
        }
    }

    auto& evals() {
        return topLevel().evals;
    }

    void pushKeepOrDrop(bool keep) {
        evals().emplace_back(keep ? EvalMode::KeepField : EvalMode::DropField, EvalExpr{});
    }

    void pushEvaluate(EvalExpr expr) {
        evals().emplace_back(EvalMode::EvaluateField, std::move(expr));
    }

    void setResult(EvalExpr expr) {
        resultExpr = std::move(expr);
    }

    void pushLevel(EvalExpr expr, boost::optional<sbe::FrameId> lambdaFrame = boost::none) {
        levels.push({state, std::move(expr), lambdaFrame});
    }

    void popLevel() {
        tassert(7580708, "Expected 'levels' to not be empty", !levels.empty());
        levels.pop();
    }

    EvalExpr done() {
        tassert(7580709, "Expected 'levels' to be empty", levels.empty());

        if (resultExpr) {
            return std::move(resultExpr);
        } else {
            return std::move(inputExpr);
        }
    }

    StageBuilderState& state;
    projection_ast::ProjectType projectType{};
    EvalExpr inputExpr;
    EvalExpr resultExpr;
    const PlanStageSlots* slots;
    bool containsComputedField{false};

    std::stack<NestedLevel> levels;
};

using FieldVector = std::vector<std::string>;

/**
 * This function takes a flag indicating if we're dealing with an inclusion or exclusion projection
 * ('isInclusion') and two parallel vectors: a vector of field names ('fieldNames') and a vector of
 * ProjectEvals ('evals').
 *
 * This function processes its inputs and returns a tuple containing a vector of "KeepOrDrop"
 * field names and two parallel vectors: a vector of "Evaluate" field names and a vector of the
 * corresponding EvalExprs.
 *
 * The output tuple is intended for to be used with MakeObjSpec and the makeBsonObj() VM function.
 */
auto prepareFieldEvals(const FieldVector& fieldNames,
                       std::vector<ProjectEval>& evals,
                       bool isInclusion) {
    tassert(7580712,
            "Expected 'evals' and 'fieldNames' to be the same size",
            evals.size() == fieldNames.size());

    FieldVector fields;
    FieldVector projects;
    std::vector<EvalExpr> exprs;

    for (size_t i = 0; i < fieldNames.size(); i++) {
        auto& fieldName = fieldNames[i];
        auto mode = evals[i].mode;
        auto& expr = evals[i].expr;

        switch (mode) {
            case EvalMode::KeepField:
                if (isInclusion) {
                    fields.push_back(fieldName);
                }
                break;
            case EvalMode::DropField:
                if (!isInclusion) {
                    fields.push_back(fieldName);
                }
                break;
            case EvalMode::EvaluateField: {
                projects.push_back(fieldName);
                exprs.emplace_back(std::move(expr));
                break;
            }
        }
    }

    return std::make_tuple(std::move(fields), std::move(projects), std::move(exprs));
}

void preVisitCommon(PathTreeNode<boost::optional<ProjectionNode>>* node,
                    ProjectionVisitorContext& ctx) {
    if (node->value) {
        if (node->value->isExpr()) {
            ctx.setSubtreeContainsComputedField(true);
        }
        return;
    }

    if (!ctx.levelsEmpty()) {
        auto frame = ctx.state.frameId();
        ctx.pushLevel(EvalExpr{makeVariable(frame, 0)}, frame);
    } else {
        ctx.pushLevel(ctx.inputExpr.clone());
    }
}

void postVisitCommon(PathTreeNode<boost::optional<ProjectionNode>>* node,
                     ProjectionVisitorContext& ctx,
                     boost::optional<int32_t> traversalDepth = boost::none) {
    if (node->value) {
        return;
    }

    std::vector<std::string> childNames;
    for (auto&& child : node->children) {
        childNames.emplace_back(child->name);
    }

    const bool isInclusion = ctx.projectType == projection_ast::ProjectType::kInclusion;
    const bool isInclusionOrAddFields = ctx.projectType != projection_ast::ProjectType::kExclusion;
    auto fieldBehavior =
        isInclusion ? sbe::MakeObjSpec::FieldBehavior::keep : sbe::MakeObjSpec::FieldBehavior::drop;

    auto [fields, projects, exprs] = prepareFieldEvals(childNames, ctx.evals(), isInclusion);

    auto lambdaFrame = ctx.topLevel().lambdaFrame;
    auto childInputExpr = ctx.topLevel().extractInputExpr();
    const bool containsComputedField = ctx.getSubtreeContainsComputedField();

    // We've finished extracting what we need from the child level, so pop if off the stack.
    ctx.popLevel();

    // If the child's 'subtreeContainsComputedField' flag was set, then propagate it to
    // the parent level.
    ctx.setSubtreeContainsComputedField(ctx.getSubtreeContainsComputedField() ||
                                        containsComputedField);

    // If the current sub-tree does not contain any work that needs to be done, then there is
    // no need to change the object. Push 'EvalMode::KeepField' for this sub-tree (if levels
    // is non-empty) and then return.
    if (!isInclusion && fields.empty() && exprs.empty()) {
        if (!ctx.levelsEmpty()) {
            ctx.pushKeepOrDrop(true);
        }
        return;
    }

    // Create a 'makeBsonObj(..)' expression to generate the document for the current nested level.
    auto makeObjSpecExpr =
        makeConstant(sbe::value::TypeTags::makeObjSpec,
                     sbe::value::bitcastFrom<sbe::MakeObjSpec*>(new sbe::MakeObjSpec(
                         fieldBehavior, std::move(fields), std::move(projects))));

    auto args = sbe::makeEs(std::move(makeObjSpecExpr), childInputExpr->clone());
    for (auto& expr : exprs) {
        args.push_back(expr.extractExpr(ctx.state));
    }

    auto makeObjFn = "makeBsonObj"_sd;
    auto innerExpr = sbe::makeE<sbe::EFunction>(makeObjFn, std::move(args));

    // If there are no more levels left, we just need to store the result by calling setResult()
    // and then we can return.
    if (ctx.levelsEmpty()) {
        ctx.setResult(std::move(innerExpr));
        return;
    }

    tassert(7580713, "Expected lambda frame to be set", lambdaFrame);

    // If this is an inclusion projection and with no computed fields, then anything that's
    // not an object should get filtered out. Example:
    // projection: {a: {b: 1}}
    // document: {a: [1, {b: 2}, 3]}
    // result: {a: [{b: 2}]}
    //
    // If this is an exclusion projection, then anything that is not an object should be
    // preserved as-is.
    //
    // If this is an inclusion projection with 1 or more computed fields, then projections
    // of computed fields should always be applied even if the values aren't objects.
    // Example:
    // projection: {a: {b: "x"}}
    // document: {a: [1,2,3]}
    // result: {a: [{b: "x"}, {b: "x"}, {b: "x"}]}
    if (isInclusionOrAddFields && !containsComputedField) {
        innerExpr = sbe::makeE<sbe::EIf>(makeFunction("isObject", childInputExpr->clone()),
                                         std::move(innerExpr),
                                         makeNothingConstant());
    } else if (!isInclusionOrAddFields) {
        innerExpr = sbe::makeE<sbe::EIf>(makeFunction("isObject", childInputExpr->clone()),
                                         std::move(innerExpr),
                                         childInputExpr->clone());
    }

    auto nodeName = StringData(node->name);
    auto topLevelFieldSlot = (ctx.slots != nullptr && ctx.numLevels() == 1)
        ? ctx.slots->getIfExists(std::make_pair(PlanStageSlots::kField, nodeName))
        : boost::none;

    auto fromExpr = topLevelFieldSlot
        ? makeVariable(*topLevelFieldSlot)
        : makeFunction("getField"_sd, ctx.topLevel().getInputExpr(), makeStrConstant(node->name));

    auto depthExpr = traversalDepth ? makeInt32Constant(*traversalDepth) : makeNothingConstant();

    // Create the call to traverseP().
    ctx.pushEvaluate(makeFunction("traverseP",
                                  std::move(fromExpr),
                                  sbe::makeE<sbe::ELocalLambda>(*lambdaFrame, std::move(innerExpr)),
                                  std::move(depthExpr)));
}

EvalExpr evaluateProjection(StageBuilderState& state,
                            projection_ast::ProjectType type,
                            std::vector<std::string> paths,
                            std::vector<ProjectionNode> nodes,
                            sbe::value::SlotId rootSlot,
                            const PlanStageSlots* slots) {
    using Node = PathTreeNode<boost::optional<ProjectionNode>>;

    auto tree = buildPathTree<boost::optional<ProjectionNode>>(
        std::move(paths), std::move(nodes), BuildPathTreeMode::AssertNoConflictingPaths);

    ProjectionVisitorContext context{state, type, rootSlot, slots};

    auto preVisit = [&](Node* node) {
        preVisitCommon(node, context);
    };

    auto postVisit = [&](Node* node) {
        if (node->value) {
            if (node->value->isBool()) {
                context.pushKeepOrDrop(node->value->getBool());
            } else if (node->value->isExpr()) {
                context.pushEvaluate(
                    generateExpression(state, node->value->getExpr(), rootSlot, context.slots));
            } else if (node->value->isSlice()) {
                // We should not encounter 'Slice' here. If the original projection contained
                // one or more $slice ops, the caller should have detected this and replaced
                // each 'Slice' node with a 'Keep' node before calling this function.
                tasserted(7580714, "Encountered unexpected node type 'kSlice'");
            } else {
                MONGO_UNREACHABLE;
            }
        }

        postVisitCommon(node, context);
    };

    const bool invokeCallbacksForRootNode = true;
    visitPathTreeNodes(tree.get(), preVisit, postVisit, invokeCallbacksForRootNode);

    return context.done();
}

// When a projection contains $slice ops, this function is called after evaluateProjection()
// to deal with evaluating the $slice ops.
EvalExpr evaluateSliceOps(StageBuilderState& state,
                          std::vector<std::string> paths,
                          std::vector<ProjectionNode> nodes,
                          EvalExpr inputExpr,
                          const PlanStageSlots* slots) {
    using Node = PathTreeNode<boost::optional<ProjectionNode>>;

    auto tree = buildPathTree<boost::optional<ProjectionNode>>(
        std::move(paths), std::move(nodes), BuildPathTreeMode::AssertNoConflictingPaths);

    // Store 'inputExpr' into a local variable.
    auto outerFrameId = state.frameId();
    auto outerBinds = sbe::makeEs(inputExpr.extractExpr(state));
    sbe::EVariable inputRef{outerFrameId, 0};

    // We want to keep the entire input document as-is except for applying the $slice ops, so
    // we use the 'kExclusion' projection type.
    ProjectionVisitorContext context{
        state, projection_ast::ProjectType::kExclusion, inputRef.clone(), slots};

    auto preVisit = [&](Node* node) {
        preVisitCommon(node, context);
    };

    // For each field path that does not have a $slice operator, we mark the field path using
    // 'EvalMode::KeepField'. This causes prepareFieldEvals() function to populate 'projectFields'
    // and 'projectExprs' only with evals for $slice operators.
    auto postVisit = [&context, &state](Node* node) {
        if (node->value) {
            // Get the SliceInfo from 'node->value'. getSlice() will tassert for us if 'node->value'
            // is not a 'Slice'.
            auto [limit, skip] = node->value->getSlice();
            auto arrayFromField = makeFunction(
                "getField"_sd, context.topLevel().getInputExpr(), makeStrConstant(node->name));
            auto innerBinds = sbe::makeEs(std::move(arrayFromField));
            auto innerFrameId = state.frameId();
            sbe::EVariable arrayVariable{innerFrameId, 0};

            auto arguments = sbe::makeEs(arrayVariable.clone(), makeInt32Constant(limit));
            if (skip) {
                arguments.push_back(makeInt32Constant(*skip));
            }

            auto extractSubArrayExpr = sbe::makeE<sbe::EIf>(
                makeFunction("isArray"_sd, arrayVariable.clone()),
                sbe::makeE<sbe::EFunction>("extractSubArray", std::move(arguments)),
                arrayVariable.clone());

            context.pushEvaluate(sbe::makeE<sbe::ELocalBind>(
                innerFrameId, std::move(innerBinds), std::move(extractSubArrayExpr)));
        }

        // When handling $slice, we only go 1 level in depth (unlike other projection operators
        // which have unlimited depth for the traversal).
        const int32_t traversalDepth = 1;
        postVisitCommon(node, context, traversalDepth);
    };

    const bool invokeCallbacksForRootNode = true;
    visitPathTreeNodes(tree.get(), preVisit, postVisit, invokeCallbacksForRootNode);

    auto resultExpr = context.done();

    return sbe::makeE<sbe::ELocalBind>(
        outerFrameId, std::move(outerBinds), resultExpr.extractExpr(state));
}
}  // namespace

EvalExpr generateProjection(StageBuilderState& state,
                            const projection_ast::Projection* projection,
                            sbe::value::SlotId inputSlot,
                            const PlanStageSlots* slots) {
    const bool isInclusion = projection->type() == projection_ast::ProjectType::kInclusion;

    // Do a DFS on the projection AST and populate 'allPaths' and 'allNodes'.
    auto [allPaths, allNodes] = getProjectionNodes(*projection);

    // Check for 'Slice' operators. If 'allNodes' doesn't have any $slice operators, we just
    // return the expression generated by evaluateProjection(). If 'tree' contains one or
    // more $slice operators, then after evaluateProjection() returns we need to apply a
    // "post-projection transform" to evaluate the $slice ops. (This mirrors the classic
    // engine's implementation of $slice, see the 'ExpressionInternalFindSlice' class for
    // details.)
    std::vector<std::string> paths;
    std::vector<ProjectionNode> nodes;
    std::vector<std::string> slicePaths;
    std::vector<ProjectionNode> sliceNodes;
    if (!std::any_of(allNodes.begin(), allNodes.end(), [&](auto&& n) { return n.isSlice(); })) {
        // If 'allNodes' doesn't contain any Slices, then we just move 'allNodes' to 'nodes' and
        // move 'allPaths' to 'paths'.
        paths = std::move(allPaths);
        nodes = std::move(allNodes);
    } else {
        for (size_t i = 0; i < allNodes.size(); ++i) {
            auto& path = allPaths[i];
            auto& node = allNodes[i];
            if (!node.isSlice()) {
                // If 'node' is not a Slice, move it to the 'nodes' vector.
                paths.emplace_back(std::move(path));
                nodes.emplace_back(std::move(node));
            } else {
                // If 'node' is a Slice, move it to the 'sliceNodes' vector. If this is an
                // inclusion projection, then we also need to add a 'Keep' node to 'nodes' so
                // that the first pass doesn't drop 'path'.
                if (isInclusion) {
                    paths.emplace_back(path);
                    nodes.emplace_back(ProjectionNode::Keep{});
                }
                slicePaths.emplace_back(std::move(path));
                sliceNodes.emplace_back(std::move(node));
            }
        }
    }

    EvalExpr projExpr;

    // If this is an inclusion projection or if 'nodes' is not empty, call evaluateProjection().
    if (isInclusion || !nodes.empty()) {
        projExpr = evaluateProjection(
            state, projection->type(), std::move(paths), std::move(nodes), inputSlot, slots);
    } else {
        projExpr = EvalExpr(inputSlot);
    }

    // If 'sliceNodes' is not empty, then we need to call evaluateSliceOps() to evaluate the
    // $slice ops.
    if (!sliceNodes.empty()) {
        projExpr = evaluateSliceOps(
            state, std::move(slicePaths), std::move(sliceNodes), std::move(projExpr), slots);
    }

    return projExpr;
}
}  // namespace mongo::stage_builder
