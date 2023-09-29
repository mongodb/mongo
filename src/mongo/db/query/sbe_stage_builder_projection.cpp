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
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_ast_visitor.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo::stage_builder {
namespace {
/**
 * Enum desribing mode in which projection for the field must be evaluated.
 */
enum class EvalMode {
    // Include field in output object with no modification.
    kKeep,
    // Exclude field from output object.
    kDrop,
    // Set field value with an SbExpr.
    kValueArg,
    // Invoke a lambda passing in the field, then set field to the lambda's return value.
    kLambdaArg,
    // Call makeBsonObj() passing in the field, then set field to makeBsonObj()'s return value.
    kMakeObj,
};

/**
 * A 'ProjectEval' contains an EvalMode ('mode') and an SbExpr ('expr'). If 'mode' is equal
 * to 'EvalMode::EvaluateField' then 'expr' will be non-null, otherwise 'expr' will be null.
 */
struct ProjectEval {
    ProjectEval(EvalMode mode,
                std::unique_ptr<sbe::MakeObjSpec> spec = {},
                std::vector<SbExpr> exprs = {},
                bool returnsNothingOnMissingInput = true)
        : mode(mode),
          spec(std::move(spec)),
          exprs(std::move(exprs)),
          returnsNothingOnMissingInput(returnsNothingOnMissingInput) {}

    EvalMode mode;
    std::unique_ptr<sbe::MakeObjSpec> spec;
    std::vector<SbExpr> exprs;
    bool returnsNothingOnMissingInput = true;
};

/**
 * Stores the necessary context needed while visiting each node in the projection tree.
 */
struct ProjectionVisitorContext {
    /**
     * Represents current projection level. Created each time visitor encounters path projection.
     */
    struct NestedLevel {
        NestedLevel() = default;

        // Vector containing operations for the current level. There are 5 types of operations
        // (see EvalMode enum for details).
        std::vector<ProjectEval> evals;

        // Whether or not any subtree of this level has a computed field.
        bool hasValueArgs = false;
    };

    ProjectionVisitorContext(StageBuilderState& state,
                             projection_ast::ProjectType projectType,
                             SbExpr inputExpr,
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

    bool getHasValueArgs() {
        return !levels.empty() ? topLevel().hasValueArgs : hasValueArgs;
    }

    void setHasValueArgs(bool val) {
        if (!levels.empty()) {
            topLevel().hasValueArgs = val;
        } else {
            hasValueArgs = val;
        }
    }

    auto& evals() {
        return topLevel().evals;
    }

    void pushKeepOrDrop(bool keep) {
        evals().emplace_back(keep ? EvalMode::kKeep : EvalMode::kDrop);
    }
    void pushValueArg(SbExpr expr) {
        std::vector<SbExpr> exprs;
        exprs.emplace_back(std::move(expr));
        evals().emplace_back(ProjectEval(EvalMode::kValueArg, {}, std::move(exprs)));
    }
    void pushLambdaArg(SbExpr lambdaExpr, bool returnsNothingOnMissingInput = true) {
        std::vector<SbExpr> exprs;
        exprs.emplace_back(std::move(lambdaExpr));
        evals().emplace_back(
            ProjectEval(EvalMode::kLambdaArg, {}, std::move(exprs), returnsNothingOnMissingInput));
    }
    void pushMakeObj(std::unique_ptr<sbe::MakeObjSpec> spec, std::vector<SbExpr> args) {
        evals().emplace_back(ProjectEval(EvalMode::kMakeObj, std::move(spec), std::move(args)));
    }

    void setResult(SbExpr expr) {
        resultExpr = std::move(expr);
    }

    void pushLevel() {
        levels.push({});
    }
    void popLevel() {
        tassert(7580708, "Expected 'levels' to not be empty", !levels.empty());
        levels.pop();
    }

    SbExpr done() {
        tassert(7580709, "Expected 'levels' to be empty", levels.empty());

        if (resultExpr) {
            return std::move(resultExpr);
        } else {
            return std::move(inputExpr);
        }
    }

    StageBuilderState& state;
    projection_ast::ProjectType projectType{};
    SbExpr inputExpr;
    SbExpr resultExpr;
    const PlanStageSlots* slots;
    bool hasValueArgs{false};

    size_t nextArgIdx{0};

    std::stack<NestedLevel> levels;
};

/**
 * This function takes a flag indicating if we're dealing with an inclusion or exclusion projection
 * ('isInclusion') and two parallel vectors: a vector of field names ('fieldNames') and a vector of
 * ProjectEvals ('evals').
 *
 * This function processes its inputs and returns a tuple containing a vector field names, a vector
 * of FieldInfos, and a vector of SbExprs.
 *
 * The output tuple is intended for to be used with MakeObjSpec and the makeBsonObj() VM function.
 */
auto prepareFieldEvals(const std::vector<std::string>& fieldNames,
                       std::vector<ProjectEval>& evals,
                       bool isInclusion,
                       size_t* nextArgIdx) {
    // Ensure that there is eval for each of the field names.
    tassert(7580712,
            "Expected 'evals' and 'fieldNames' to be the same size",
            evals.size() == fieldNames.size());

    std::vector<std::string> fields;
    std::vector<sbe::MakeObjSpec::FieldInfo> fieldInfos;
    std::vector<SbExpr> valueAndLambdaArgs;
    std::vector<SbExpr> args;

    for (size_t i = 0; i < fieldNames.size(); i++) {
        auto& fieldName = fieldNames[i];
        auto mode = evals[i].mode;
        auto& exprs = evals[i].exprs;
        auto& spec = evals[i].spec;
        bool returnsNothingOnMissingInput = evals[i].returnsNothingOnMissingInput;

        switch (mode) {
            case EvalMode::kKeep:
                if (isInclusion) {
                    fields.emplace_back(fieldName);
                    fieldInfos.emplace_back();
                }
                break;
            case EvalMode::kDrop:
                if (!isInclusion) {
                    fields.emplace_back(fieldName);
                    fieldInfos.emplace_back();
                }
                break;
            case EvalMode::kValueArg:
                fields.emplace_back(fieldName);
                fieldInfos.emplace_back(*nextArgIdx);
                valueAndLambdaArgs.emplace_back(std::move(exprs[0]));
                ++(*nextArgIdx);
                break;
            case EvalMode::kLambdaArg:
                fields.emplace_back(fieldName);
                fieldInfos.emplace_back(
                    sbe::MakeObjSpec::LambdaArg{*nextArgIdx, returnsNothingOnMissingInput});
                valueAndLambdaArgs.emplace_back(std::move(exprs[0]));
                ++(*nextArgIdx);
                break;
            case EvalMode::kMakeObj:
                fields.emplace_back(fieldName);
                fieldInfos.emplace_back(std::move(spec));
                if (!exprs.empty()) {
                    std::move(exprs.begin(), exprs.end(), std::back_inserter(args));
                }
                break;
        }
    }

    std::move(valueAndLambdaArgs.begin(), valueAndLambdaArgs.end(), std::back_inserter(args));

    return std::make_tuple(std::move(fields), std::move(fieldInfos), std::move(args));
}

void preVisitCommon(PathTreeNode<boost::optional<ProjectionNode>>* node,
                    ProjectionVisitorContext& ctx) {
    if (node->value) {
        if (node->value->isExpr() || node->value->isSbExpr()) {
            ctx.setHasValueArgs(true);
        }
        return;
    }

    ctx.pushLevel();
}

/**
 * If a subtree of the projection contains 1 or more value args (i.e. computed fields), then
 * the projection should always be applied even if the values aren't objects. Example:
 *   projection: {a: {b: "x"}}
 *   document: {a: [1,2,3]}
 *   result: {a: [{b: "x"}, {b: "x"}, {b: "x"}]}
 *
 * If this subtree doesn't contain any value args and we're performing an inclusion projection,
 * then anything that's not an object should get filtered out. Example:
 *   projection: {a: {b: 1}}
 *   document: {a: [1, {b: 2}, 3]}
 *   result: {a: [{b: 2}]}
 *
 * If this subtree doesn't contain any value args and we're performing an exclusion projection,
 * then anything that's not an object should be preserved as-is.
 */
sbe::MakeObjSpec::NonObjInputBehavior getNonObjInputBehavior(bool hasValueArgs, bool isInclusion) {
    using NonObjInputBehavior = sbe::MakeObjSpec::NonObjInputBehavior;

    return hasValueArgs
        ? NonObjInputBehavior::kNewObj
        : (isInclusion ? NonObjInputBehavior::kReturnNothing : NonObjInputBehavior::kReturnInput);
}

void postVisitCommon(PathTreeNode<boost::optional<ProjectionNode>>* node,
                     ProjectionVisitorContext& ctx,
                     boost::optional<int32_t> traversalDepth = boost::none) {
    using FieldBehavior = sbe::MakeObjSpec::FieldBehavior;

    if (node->value) {
        return;
    }

    std::vector<std::string> childNames;
    for (auto&& child : node->children) {
        childNames.emplace_back(child->name);
    }

    const bool isInclusion = ctx.projectType == projection_ast::ProjectType::kInclusion;

    auto [fields, fieldInfos, args] =
        prepareFieldEvals(childNames, ctx.evals(), isInclusion, &ctx.nextArgIdx);

    const bool hasValueArgs = ctx.getHasValueArgs();

    // We've finished extracting what we need from the child level, so pop if off the stack.
    ctx.popLevel();

    // If the child's 'hasValueArgs' flag was set, then propagate it to the parent level.
    ctx.setHasValueArgs(ctx.getHasValueArgs() || hasValueArgs);

    // If the current sub-tree does not contain any work that needs to be done, then there is
    // no need to change the object. Push 'EvalMode::KeepField' for this sub-tree (if levels
    // is non-empty) and then return.
    if (!isInclusion && fields.empty()) {
        if (!ctx.levelsEmpty()) {
            ctx.pushKeepOrDrop(true);
        }
        return;
    }

    auto fieldBehavior = isInclusion ? FieldBehavior::kClosed : FieldBehavior::kOpen;
    auto noiBehavior = getNonObjInputBehavior(hasValueArgs, isInclusion);

    // Generate a MakeObjSpec for the current nested level.
    auto spec = std::make_unique<sbe::MakeObjSpec>(
        fieldBehavior, std::move(fields), std::move(fieldInfos), noiBehavior, traversalDepth);

    if (ctx.levelsEmpty()) {
        SbExprBuilder b(ctx.state);

        // For the last level, create a 'makeBsonObj(..)' expression to generate the output object.
        auto specExpr = b.makeConstant(sbe::value::TypeTags::makeObjSpec,
                                       sbe::value::bitcastFrom<sbe::MakeObjSpec*>(spec.release()));

        auto funcArgs = SbExpr::makeSeq(std::move(specExpr), std::move(ctx.inputExpr));
        std::move(args.begin(), args.end(), std::back_inserter(funcArgs));

        auto makeObjFn = "makeBsonObj"_sd;
        ctx.setResult(b.makeFunction(makeObjFn, std::move(funcArgs)));
    } else {
        ctx.pushMakeObj(std::move(spec), std::move(args));
    }
}

SbExpr evaluateProjection(StageBuilderState& state,
                          projection_ast::ProjectType type,
                          std::vector<std::string> paths,
                          std::vector<ProjectionNode> nodes,
                          SbExpr inputExpr,
                          boost::optional<TypedSlot> rootSlot,
                          const PlanStageSlots* slots) {
    using Node = PathTreeNode<boost::optional<ProjectionNode>>;

    auto tree = buildPathTree<boost::optional<ProjectionNode>>(
        paths, std::move(nodes), BuildPathTreeMode::AssertNoConflictingPaths);

    ProjectionVisitorContext context{state, type, std::move(inputExpr), slots};

    auto preVisit = [&](Node* node) {
        preVisitCommon(node, context);
    };

    auto postVisit = [&](Node* node) {
        if (node->value) {
            if (node->value->isBool()) {
                context.pushKeepOrDrop(node->value->getBool());
            } else if (node->value->isExpr()) {
                context.pushValueArg(
                    generateExpression(state, node->value->getExpr(), rootSlot, context.slots));
            } else if (node->value->isSbExpr()) {
                context.pushValueArg(node->value->extractSbExpr());
            } else if (node->value->isSlice()) {
                // We should not encounter 'Slice' here. If the original projection contained
                // one or more $slice ops, the caller should have detected this and replaced
                // each 'Slice' node with a 'Keep' node before calling this function.
                tasserted(7580714, "Encountered unexpected node type 'kSlice'");
            } else {
                MONGO_UNREACHABLE_TASSERT(7103504);
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
SbExpr evaluateSliceOps(StageBuilderState& state,
                        std::vector<std::string> paths,
                        std::vector<ProjectionNode> nodes,
                        SbExpr inputExpr,
                        const PlanStageSlots* slots) {
    using Node = PathTreeNode<boost::optional<ProjectionNode>>;

    auto tree = buildPathTree<boost::optional<ProjectionNode>>(
        paths, std::move(nodes), BuildPathTreeMode::AssertNoConflictingPaths);

    // We want to keep the entire input document as-is except for applying the $slice ops, so
    // we use the 'kExclusion' projection type.
    ProjectionVisitorContext context{
        state, projection_ast::ProjectType::kExclusion, std::move(inputExpr), slots};

    auto preVisit = [&](Node* node) {
        preVisitCommon(node, context);
    };

    auto postVisit = [&](Node* node) {
        if (node->value) {
            SbExprBuilder b(state);

            // 'nodes' should only contain $slice operators.
            tassert(7103505, "Expected node type to be 'kSlice'", node->value->isSlice());

            auto [limit, skip] = node->value->getSlice();
            auto lambdaFrameId = state.frameId();
            auto lambdaParam = b.makeVariable(lambdaFrameId, 0);

            auto args = SbExpr::makeSeq(lambdaParam.clone(), b.makeInt32Constant(limit));
            if (skip) {
                args.emplace_back(b.makeInt32Constant(*skip));
            }

            auto extractSubArrayExpr = b.makeIf(b.makeFunction("isArray"_sd, lambdaParam.clone()),
                                                b.makeFunction("extractSubArray", std::move(args)),
                                                lambdaParam.clone());

            context.pushLambdaArg(b.makeLocalLambda(lambdaFrameId, std::move(extractSubArrayExpr)));
        }

        // When handling $slice, we only go 1 level in depth (unlike other projection operators
        // which have unlimited depth for the traversal).
        const int32_t traversalDepth = 1;
        postVisitCommon(node, context, traversalDepth);
    };

    const bool invokeCallbacksForRootNode = true;
    visitPathTreeNodes(tree.get(), preVisit, postVisit, invokeCallbacksForRootNode);

    return context.done();
}
}  // namespace

SbExpr generateProjection(StageBuilderState& state,
                          const projection_ast::Projection* projection,
                          SbExpr inputExpr,
                          boost::optional<TypedSlot> rootSlot,
                          const PlanStageSlots* slots) {
    const auto projType = projection->type();

    // Do a DFS on the projection AST and populate 'paths' and 'nodes'.
    auto [paths, nodes] = getProjectionNodes(*projection);

    return generateProjection(
        state, projType, std::move(paths), std::move(nodes), std::move(inputExpr), rootSlot, slots);
}

SbExpr generateProjection(StageBuilderState& state,
                          projection_ast::ProjectType projType,
                          std::vector<std::string> paths,
                          std::vector<ProjectionNode> nodes,
                          SbExpr inputExpr,
                          boost::optional<TypedSlot> rootSlot,
                          const PlanStageSlots* slots) {
    const bool isInclusion = projType == projection_ast::ProjectType::kInclusion;

    // Check for 'Slice' operators. If 'nodes' doesn't have any $slice operators, we just
    // return the expression generated by evaluateProjection(). If 'tree' contains one or
    // more $slice operators, then after evaluateProjection() returns we need to apply a
    // "post-projection transform" to evaluate the $slice ops. (This mirrors the classic
    // engine's implementation of $slice, see the 'ExpressionInternalFindSlice' class for
    // details.)
    std::vector<std::string> slicePaths;
    std::vector<ProjectionNode> sliceNodes;

    if (std::any_of(nodes.begin(), nodes.end(), [&](auto&& n) { return n.isSlice(); })) {
        std::vector<std::string> newPaths;
        std::vector<ProjectionNode> newNodes;

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& path = paths[i];
            auto& node = nodes[i];
            if (!node.isSlice()) {
                // If 'node' is not a Slice, move it to the 'newNodes' vector.
                newPaths.emplace_back(std::move(path));
                newNodes.emplace_back(std::move(node));
            } else {
                // If 'node' is a Slice, move it to the 'sliceNodes' vector. If this is an
                // inclusion projection, then we also need to add a 'Keep' node to 'newNodes' so
                // that the first pass doesn't drop 'path'.
                if (isInclusion) {
                    newPaths.emplace_back(path);
                    newNodes.emplace_back(ProjectionNode::Keep{});
                }
                slicePaths.emplace_back(std::move(path));
                sliceNodes.emplace_back(std::move(node));
            }
        }

        paths = std::move(newPaths);
        nodes = std::move(newNodes);
    }

    auto expr = std::move(inputExpr);

    // If this is an inclusion projection or if 'nodes' is not empty, call evaluateProjection().
    if (isInclusion || !nodes.empty()) {
        expr = evaluateProjection(
            state, projType, std::move(paths), std::move(nodes), std::move(expr), rootSlot, slots);
    }

    // If 'sliceNodes' is not empty, then we need to call evaluateSliceOps() to evaluate the
    // $slice ops.
    if (!sliceNodes.empty()) {
        expr = evaluateSliceOps(
            state, std::move(slicePaths), std::move(sliceNodes), std::move(expr), slots);
    }

    return expr;
}
}  // namespace mongo::stage_builder
