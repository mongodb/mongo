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

#include "mongo/db/query/stage_builder/sbe/gen_projection.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_expression.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <cstddef>
#include <memory>
#include <stack>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::stage_builder {
ProjectActionType ProjectAction::type() const {
    return visit(OverloadedVisitor{[](const Keep&) { return ProjectActionType::kKeep; },
                                   [](const Drop&) { return ProjectActionType::kDrop; },
                                   [](const SetArg&) { return ProjectActionType::kSetArg; },
                                   [](const AddArg&) { return ProjectActionType::kAddArg; },
                                   [](const LambdaArg&) { return ProjectActionType::kLambdaArg; },
                                   [](const MakeObj&) {
                                       return ProjectActionType::kMakeObj;
                                   }},
                 _data);
}

ProjectAction ProjectAction::clone() const {
    return visit([](const auto& data) { return ProjectAction{data.clone()}; }, _data);
}

ProjectAction::MakeObj ProjectAction::MakeObj::clone() const {
    return MakeObj{std::make_unique<ProjectActions>(projActions->clone())};
}

namespace {
struct ProjectionVisitorContext {
    /**
     * Represents current projection level. Created each time visitor encounters path projection.
     */
    struct NestedLevel {
        NestedLevel() = default;

        // Vector containing operations for the current level. There are 6 types of operations
        // (see ProjectActionType enum for details).
        std::vector<ProjectAction> evals;

        // Whether or not any subtree of this level has expressions.
        bool hasExpressions = false;
    };

    ProjectionVisitorContext(projection_ast::ProjectType projectType) : projectType(projectType) {}

    bool levelsEmpty() const {
        return levels.empty();
    }

    NestedLevel& topLevel() {
        tassert(7580707, "Expected 'levels' to not be empty", !levels.empty());
        return levels.top();
    }

    bool getHasExpressions() {
        return !levels.empty() ? topLevel().hasExpressions : hasExpressions;
    }

    void setHasExpressions(bool val) {
        if (!levels.empty()) {
            topLevel().hasExpressions = val;
        } else {
            hasExpressions = val;
        }
    }

    std::vector<ProjectAction>& evals() {
        return topLevel().evals;
    }

    std::vector<ProjectAction> extractEvals() {
        auto evals = std::move(topLevel().evals);
        topLevel().evals = std::vector<ProjectAction>{};
        return evals;
    }

    void pushKeepOrDrop(bool keep) {
        if (keep) {
            evals().emplace_back(ProjectAction::Keep{});
        } else {
            evals().emplace_back(ProjectAction::Drop{});
        }
    }
    void pushSetArg(SbExpr expr) {
        evals().emplace_back(ProjectAction::SetArg{std::move(expr)});
    }
    void pushAddArg(SbExpr expr) {
        evals().emplace_back(ProjectAction::AddArg{std::move(expr)});
    }
    void pushLambdaArg(SbExpr bodyExpr,
                       sbe::FrameId frameId,
                       bool returnsNothingOnMissingInput = true) {
        evals().emplace_back(
            ProjectAction::LambdaArg{std::move(bodyExpr), frameId, returnsNothingOnMissingInput});
    }
    void pushMakeObj(ProjectActions pa) {
        evals().emplace_back(
            ProjectAction::MakeObj{std::make_unique<ProjectActions>(std::move(pa))});
    }

    void setResult(ProjectActions pa) {
        result = std::move(pa);
    }

    void pushLevel() {
        levels.push({});
    }
    void popLevel() {
        tassert(7580708, "Expected 'levels' to not be empty", !levels.empty());
        levels.pop();
    }

    ProjectActions done() {
        tassert(7580709, "Expected 'levels' to be empty", levels.empty());
        tassert(9295201, "Expected 'result' to be set", result.has_value());

        return std::move(*result);
    }

    projection_ast::ProjectType projectType{};
    boost::optional<ProjectActions> result;
    bool hasExpressions{false};
    std::stack<NestedLevel> levels;
};

std::unique_ptr<sbe::MakeObjSpec> buildMakeObjSpecImpl(StageBuilderState& state,
                                                       ProjectActions& pa,
                                                       SbExpr::Vector& args) {
    using FieldAction = sbe::MakeObjSpec::FieldAction;

    SbExprBuilder b(state);

    std::vector<std::unique_ptr<sbe::MakeObjSpec>> childSpecs;
    childSpecs.resize(pa.actions.size());

    for (size_t i = 0; i < pa.actions.size(); ++i) {
        auto& action = pa.actions[i];
        if (action.isMakeObj()) {
            childSpecs[i] = buildMakeObjSpecImpl(state, *action.getMakeObj().projActions, args);
        }
    }

    std::vector<FieldAction> actions;

    for (size_t i = 0; i < pa.actions.size(); i++) {
        size_t nextArgIdx = args.size();

        actions.emplace_back(visit(
            OverloadedVisitor{
                [&](ProjectAction::Keep&) -> FieldAction { return sbe::MakeObjSpec::Keep{}; },
                [&](ProjectAction::Drop&) -> FieldAction { return sbe::MakeObjSpec::Drop{}; },
                [&](ProjectAction::SetArg& sa) -> FieldAction {
                    args.emplace_back(std::move(sa.arg));
                    return sbe::MakeObjSpec::SetArg{nextArgIdx};
                },
                [&](ProjectAction::AddArg& aa) -> FieldAction {
                    args.emplace_back(std::move(aa.arg));
                    return sbe::MakeObjSpec::AddArg{nextArgIdx};
                },
                [&](ProjectAction::LambdaArg& la) -> FieldAction {
                    args.emplace_back(b.makeLocalLambda(la.frameId, std::move(la.bodyExpr)));
                    return sbe::MakeObjSpec::LambdaArg{nextArgIdx, la.returnsNothingOnMissingInput};
                },
                [&](ProjectAction::MakeObj& mo) -> FieldAction {
                    return sbe::MakeObjSpec::MakeObj{std::move(childSpecs[i])};
                }},
            pa.actions[i].getData()));
    }

    return std::make_unique<sbe::MakeObjSpec>(pa.fieldsScope,
                                              std::move(pa.fields),
                                              std::move(actions),
                                              pa.nonObjInputBehavior,
                                              pa.traversalDepth);
}

std::pair<std::unique_ptr<sbe::MakeObjSpec>, SbExpr::Vector> buildMakeObjSpec(
    StageBuilderState& state, ProjectActions pa) {
    SbExpr::Vector args;

    auto spec = buildMakeObjSpecImpl(state, pa, args);

    return {std::move(spec), std::move(args)};
}

void preVisitNode(PathTreeNode<boost::optional<ProjectNode>>* node, ProjectionVisitorContext& ctx) {
    if (node->value) {
        if (node->value->isExpr() || node->value->isSbExpr()) {
            ctx.setHasExpressions(true);
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
ProjectActions::NonObjInputBehavior getNonObjInputBehavior(bool hasExpressions, bool isInclusion) {
    using NonObjInputBehavior = ProjectActions::NonObjInputBehavior;

    return hasExpressions
        ? NonObjInputBehavior::kNewObj
        : (isInclusion ? NonObjInputBehavior::kReturnNothing : NonObjInputBehavior::kReturnInput);
}

void postVisitNonLeafNode(PathTreeNode<boost::optional<ProjectNode>>* node,
                          ProjectionVisitorContext& ctx,
                          boost::optional<int32_t> travDepth = boost::none) {
    if (node->value) {
        return;
    }

    std::vector<std::string> fields;
    for (auto&& child : node->children) {
        fields.emplace_back(child->name);
    }

    const bool isInclusion = ctx.projectType == projection_ast::ProjectType::kInclusion;
    const bool hasExpressions = ctx.getHasExpressions();
    std::vector<ProjectAction> actions = ctx.extractEvals();

    // We've finished extracting what we need from the child level, so pop if off the stack. Also,
    // if 'hasExpressions' was set to true on the child level, make sure we propagate it to the
    // parent level.
    ctx.popLevel();
    ctx.setHasExpressions(ctx.getHasExpressions() || hasExpressions);

    auto defaultActionType = isInclusion ? ProjectActionType::kDrop : ProjectActionType::kKeep;

    // Remove actions that are the same as the default action from 'fields' / 'actions'.
    size_t outIdx = 0;
    for (size_t idx = 0; idx < actions.size(); idx++) {
        if (actions[idx].type() != defaultActionType) {
            if (idx != outIdx) {
                fields[outIdx] = std::move(fields[idx]);
                actions[outIdx] = std::move(actions[idx]);
            }
            ++outIdx;
        }
    }
    if (outIdx != actions.size()) {
        fields.resize(outIdx);
        actions.resize(outIdx);
    }

    bool isTopLevel = ctx.levelsEmpty();

    auto fieldsScope = isInclusion ? FieldListScope::kClosed : FieldListScope::kOpen;
    auto noiBehavior = getNonObjInputBehavior(hasExpressions, isInclusion);
    travDepth = !isTopLevel ? travDepth : boost::optional<int32_t>{0};

    ProjectActions pa{fieldsScope, std::move(fields), std::move(actions), noiBehavior, travDepth};

    if (isTopLevel) {
        // If this is the top level, return 'pa' as the result.
        ctx.setResult(std::move(pa));
    } else if (pa.isNoop()) {
        // If this isn't the top level and 'pa' is a no-op, then we can push a "Keep" instead
        // of "MakeObj".
        ctx.evals().emplace_back(ProjectAction::Keep{});
    } else {
        // Push a "MakeObj" action.
        ctx.evals().emplace_back(
            ProjectAction::MakeObj{std::make_unique<ProjectActions>(std::move(pa))});
    }
}
}  // namespace

SbExpr generateObjectExpr(StageBuilderState& state,
                          ProjectActions pa,
                          SbExpr expr,
                          bool shouldProduceBson) {
    SbExprBuilder b(state);

    expr = expr ? std::move(expr) : b.makeNullConstant();

    // If 'pa' is a no-op, then we can return 'expr' as-is.
    if (pa.isNoop()) {
        return expr;
    }

    // Build a MakeObjSpec ('spec') from 'pa'.
    auto [spec, args] = buildMakeObjSpec(state, std::move(pa));

    // Generate a makeBsonObj() expression using 'spec' and 'args'.
    auto specExpr = b.makeConstant(sbe::value::TypeTags::makeObjSpec,
                                   sbe::value::bitcastFrom<sbe::MakeObjSpec*>(spec.release()));

    const StringData makeObjFn = shouldProduceBson ? "makeBsonObj"_sd : "makeObj"_sd;
    auto funcArgs = SbExpr::makeSeq(std::move(specExpr), std::move(expr));

    std::move(args.begin(), args.end(), std::back_inserter(funcArgs));

    return b.makeFunction(makeObjFn, std::move(funcArgs));
}

SbExpr generateSingleFieldExpr(StageBuilderState& state,
                               ProjectActions pa,
                               SbExpr expr,
                               const std::string& singleField,
                               bool shouldProduceBson) {
    SbExprBuilder b(state);

    const StringData makeObjFn = shouldProduceBson ? "makeBsonObj"_sd : "makeObj"_sd;
    const auto defActionType = pa.fieldsScope == FieldListScope::kClosed ? ProjectActionType::kDrop
                                                                         : ProjectActionType::kKeep;

    ProjectAction* eval = !pa.actions.empty() ? &pa.actions[0] : nullptr;
    ProjectActionType type = eval ? eval->type() : defActionType;

    expr = expr ? std::move(expr) : b.makeNullConstant();

    switch (type) {
        case ProjectActionType::kKeep:
            // Return the input.
            return expr;
        case ProjectActionType::kDrop:
            // Return Nothing.
            return SbExpr{SbSlot{state.getNothingSlot()}};
        case ProjectActionType::kSetArg:
            // Return the arg.
            return std::move(eval->getSetArg().arg);
        case ProjectActionType::kAddArg:
            // Return the arg.
            return std::move(eval->getAddArg().arg);
        case ProjectActionType::kLambdaArg: {
            // Apply the lambda to the input and return the resulting expression.
            auto& la = eval->getLambdaArg();

            return b.makeLet(la.frameId, SbExpr::makeSeq(std::move(expr)), std::move(la.bodyExpr));
        }
        case ProjectActionType::kMakeObj: {
            // Return a 'makeBsonObj()' expression that generates the output field.
            auto& childPa = *eval->getMakeObj().projActions;

            auto [spec, args] = buildMakeObjSpec(state, std::move(childPa));
            auto specExpr =
                b.makeConstant(sbe::value::TypeTags::makeObjSpec,
                               sbe::value::bitcastFrom<sbe::MakeObjSpec*>(spec.release()));

            auto funcArgs = SbExpr::makeSeq(std::move(specExpr), std::move(expr));
            std::move(args.begin(), args.end(), std::back_inserter(funcArgs));

            return b.makeFunction(makeObjFn, std::move(funcArgs));
        }
        default:
            MONGO_UNREACHABLE_TASSERT(9295200);
    };
}

namespace {
ProjectActions evaluateProjectOps(
    StageBuilderState& state,
    projection_ast::ProjectType projType,
    std::vector<std::string> paths,  // (possibly dotted) paths to project to
    std::vector<ProjectNode> nodes,  // SlotIds w/ values for 'paths'
    const PlanStageSlots* slots,
    boost::optional<int32_t> traversalDepth) {
    using Node = PathTreeNode<boost::optional<ProjectNode>>;

    boost::optional<SbSlot> rootSlot =
        slots ? slots->getIfExists(PlanStageSlots::kResult) : boost::none;

    auto tree = buildPathTree<boost::optional<ProjectNode>>(
        paths, std::move(nodes), BuildPathTreeMode::AssertNoConflictingPaths);

    ProjectionVisitorContext context{projType};

    auto preVisit = [&](Node* node) {
        preVisitNode(node, context);
    };

    auto postVisit = [&](Node* node) {
        if (!node->value) {
            postVisitNonLeafNode(node, context, traversalDepth);
        } else {
            const bool isInclusion = projType == projection_ast::ProjectType::kInclusion;

            if (node->value->isBool()) {
                context.pushKeepOrDrop(node->value->getBool());
            } else if (node->value->isExpr() || node->value->isSbExpr()) {
                // Get the expression.
                auto e = node->value->isExpr()
                    ? generateExpression(state, node->value->getExpr(), rootSlot, *slots)
                    : node->value->extractSbExpr();
                // For expressions in inclusion projections, we use AddArg. For expressions in
                // $addFields operations, we use SetArg. We assume that exclusion projections do
                // not have expressions.
                if (isInclusion) {
                    context.pushAddArg(std::move(e));
                } else {
                    context.pushSetArg(std::move(e));
                }
            } else if (node->value->isSlice()) {
                // We should not encounter 'Slice' here. If the original projection contained
                // one or more $slice ops, the caller should have detected this and replaced
                // each 'Slice' node with a 'Keep' node before calling this function.
                tasserted(7580714, "Encountered unexpected node type 'kSlice'");
            } else {
                MONGO_UNREACHABLE_TASSERT(7103504);
            }
        }
    };

    const bool invokeCallbacksForRootNode = true;
    visitPathTreeNodes(tree.get(), preVisit, postVisit, invokeCallbacksForRootNode);

    return context.done();
}

ProjectActions evaluateSliceOps(StageBuilderState& state,
                                std::vector<std::string> paths,
                                std::vector<ProjectNode> nodes) {
    using Node = PathTreeNode<boost::optional<ProjectNode>>;

    auto tree = buildPathTree<boost::optional<ProjectNode>>(
        paths, std::move(nodes), BuildPathTreeMode::AssertNoConflictingPaths);

    // We want to keep the entire input document as-is except for applying the $slice ops, so
    // we use the 'kExclusion' projection type.
    ProjectionVisitorContext context{projection_ast::ProjectType::kExclusion};

    auto preVisit = [&](Node* node) {
        preVisitNode(node, context);
    };

    auto postVisit = [&](Node* node) {
        if (!node->value) {
            // When handling $slice, we only go 1 level in depth (unlike other projection operators
            // which have unlimited depth for the traversal).
            const int32_t travDepth = 1;
            postVisitNonLeafNode(node, context, travDepth);
        } else {
            SbExprBuilder b(state);

            // 'nodes' should only contain $slice operators.
            tassert(7103505, "Expected node type to be 'kSlice'", node->value->isSlice());

            auto [limit, skip] = node->value->getSlice();
            auto frameId = state.frameId();
            auto var = SbLocalVar{frameId, 0};

            auto args = SbExpr::makeSeq(var, b.makeInt32Constant(limit));
            if (skip) {
                args.emplace_back(b.makeInt32Constant(*skip));
            }

            auto extractSubArrayExpr = b.makeIf(b.makeFunction("isArray"_sd, var),
                                                b.makeFunction("extractSubArray", std::move(args)),
                                                var);

            context.pushLambdaArg(std::move(extractSubArrayExpr), frameId);
        }
    };

    const bool invokeCallbacksForRootNode = true;
    visitPathTreeNodes(tree.get(), preVisit, postVisit, invokeCallbacksForRootNode);

    return context.done();
}
}  // namespace

std::pair<ProjectActions, boost::optional<ProjectActions>> evaluateProjection(
    StageBuilderState& state,
    projection_ast::ProjectType projType,
    std::vector<std::string> paths,
    std::vector<ProjectNode> nodes,
    const PlanStageSlots* slots,
    boost::optional<int32_t> traversalDepth) {
    const bool isInclusion = projType == projection_ast::ProjectType::kInclusion;

    std::vector<std::string> projPaths;
    std::vector<ProjectNode> projNodes;
    std::vector<std::string> slicePaths;
    std::vector<ProjectNode> sliceNodes;

    // Check for 'Slice' operators. If 'nodes' doesn't have any $slice operators, we just
    // return the expression generated by evaluateProjection(). If 'tree' contains one or
    // more $slice operators, then after evaluateProjection() returns we need to apply a
    // "post-projection transform" to evaluate the $slice ops. (This mirrors the classic
    // engine's implementation of $slice, see the 'ExpressionInternalFindSlice' class for
    // details.)
    if (std::any_of(nodes.begin(), nodes.end(), [&](auto&& n) { return n.isSlice(); })) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& path = paths[i];
            auto& node = nodes[i];
            if (!node.isSlice()) {
                projPaths.emplace_back(std::move(path));
                projNodes.emplace_back(std::move(node));
            } else {
                // If 'node' is a Slice, move it to the 'sliceNodes' vector. If this is an
                // inclusion projection, then we also need to add a 'Keep' node to 'projNodes'
                // so that the first pass doesn't drop 'path'.
                if (isInclusion) {
                    projPaths.emplace_back(path);
                    projNodes.emplace_back(ProjectNode::Keep{});
                }
                slicePaths.emplace_back(std::move(path));
                sliceNodes.emplace_back(std::move(node));
            }
        }
    } else {
        projPaths = std::move(paths);
        projNodes = std::move(nodes);
    }

    // Call evaluateProjectOps() to evaluate the standard project ops.
    ProjectActions pa = evaluateProjectOps(
        state, projType, std::move(projPaths), std::move(projNodes), slots, traversalDepth);

    // If 'sliceNodes' is not empty, then we need to call evaluateSliceOps() as well to
    // evaluate the $slice ops.
    boost::optional<ProjectActions> slicePa;
    if (!sliceNodes.empty()) {
        slicePa.emplace(evaluateSliceOps(state, std::move(slicePaths), std::move(sliceNodes)));
    }

    return {std::move(pa), std::move(slicePa)};
}

SbExpr generateProjection(StageBuilderState& state,
                          const projection_ast::Projection* projection,
                          SbExpr inputExpr,
                          const PlanStageSlots* slots,
                          boost::optional<int32_t> traversalDepth,
                          bool shouldProduceBson) {
    const auto projType = projection->type();

    // Do a DFS on the projection AST to populate 'paths' and 'nodes'.
    auto [paths, nodes] = getProjectNodes(*projection);

    return generateProjection(state,
                              projType,
                              std::move(paths),
                              std::move(nodes),
                              std::move(inputExpr),
                              slots,
                              traversalDepth,
                              shouldProduceBson);
}

SbExpr generateProjection(StageBuilderState& state,
                          projection_ast::ProjectType projType,
                          std::vector<std::string> paths,
                          std::vector<ProjectNode> nodes,
                          SbExpr inputExpr,
                          const PlanStageSlots* slots,
                          boost::optional<int32_t> traversalDepth,
                          bool shouldProduceBson) {
    auto [pa, slicePa] = evaluateProjection(
        state, projType, std::move(paths), std::move(nodes), slots, traversalDepth);

    // Apply 'pa', and then apply 'slicePa' if it's set.
    auto expr = generateObjectExpr(state, std::move(pa), std::move(inputExpr), shouldProduceBson);

    if (slicePa) {
        expr = generateObjectExpr(state, std::move(*slicePa), std::move(expr), shouldProduceBson);
    }

    return expr;
}

SbExpr generateSingleFieldProjection(StageBuilderState& state,
                                     const projection_ast::Projection* projection,
                                     SbExpr inputExpr,
                                     const PlanStageSlots* slots,
                                     const std::string& singleField,
                                     boost::optional<int32_t> traversalDepth,
                                     bool shouldProduceBson) {
    const auto projType = projection->type();

    // Do a DFS on the projection AST to populate 'paths' and 'nodes'.
    auto [paths, nodes] = getProjectNodes(*projection);

    return generateSingleFieldProjection(state,
                                         projType,
                                         paths,
                                         nodes,
                                         std::move(inputExpr),
                                         slots,
                                         singleField,
                                         traversalDepth,
                                         shouldProduceBson);
}

SbExpr generateSingleFieldProjection(StageBuilderState& state,
                                     projection_ast::ProjectType projType,
                                     const std::vector<std::string>& pathsIn,
                                     const std::vector<ProjectNode>& nodesIn,
                                     SbExpr inputExpr,
                                     const PlanStageSlots* slots,
                                     const std::string& singleField,
                                     boost::optional<int32_t> traversalDepth,
                                     bool shouldProduceBson) {
    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;
    for (size_t i = 0; i < pathsIn.size(); ++i) {
        if (getTopLevelField(pathsIn[i]) == singleField) {
            paths.emplace_back(pathsIn[i]);
            nodes.emplace_back(nodesIn[i].clone());
        }
    }

    auto [pa, slicePa] = evaluateProjection(
        state, projType, std::move(paths), std::move(nodes), slots, traversalDepth);

    // Apply 'pa', and then apply 'slicePa' if it's set.
    auto expr = generateSingleFieldExpr(
        state, std::move(pa), std::move(inputExpr), singleField, shouldProduceBson);

    if (slicePa) {
        expr = generateSingleFieldExpr(
            state, std::move(*slicePa), std::move(expr), singleField, shouldProduceBson);
    }

    return expr;
}

ProjectActions evaluateFieldEffects(StageBuilderState& state,
                                    const FieldEffects& effects,
                                    const PlanStageSlots& slots) {
    const FieldEffect defaultEffect = effects.getDefaultEffect();

    // Populate 'fields' and 'actions' based on the effects in 'effects'.
    std::vector<std::string> fields;
    std::vector<ProjectAction> actions;

    for (const auto& field : effects.getFieldList()) {
        auto effect = effects.get(field);

        // Add a ProjectAction for this field if its effect is not equal to the default effect.
        if (effect != defaultEffect) {
            fields.emplace_back(field);

            switch (effect) {
                case FieldEffect::kKeep:
                    actions.emplace_back(ProjectAction::Keep{});
                    break;
                case FieldEffect::kDrop:
                    actions.emplace_back(ProjectAction::Drop{});
                    break;
                case FieldEffect::kAdd: {
                    auto expr = SbExpr{slots.get(std::pair(PlanStageSlots::kField, field))};
                    actions.emplace_back(ProjectAction::AddArg{std::move(expr)});
                    break;
                }
                case FieldEffect::kModify:
                case FieldEffect::kSet:
                case FieldEffect::kGeneric: {
                    auto expr = SbExpr{slots.get(std::pair(PlanStageSlots::kField, field))};
                    actions.emplace_back(ProjectAction::SetArg{std::move(expr)});
                    break;
                }
                default:
                    MONGO_UNREACHABLE_TASSERT(8323514);
            }
        }
    }

    auto fieldsScope =
        defaultEffect == FieldEffect::kDrop ? FieldListScope::kClosed : FieldListScope::kOpen;
    auto noiBehavior = ProjectActions::NonObjInputBehavior::kNewObj;
    auto travDepth = boost::optional<int32_t>{0};

    // Build a ProjectActions struct ('pa') from 'fields' and 'actions' and return it.
    return ProjectActions{
        fieldsScope, std::move(fields), std::move(actions), noiBehavior, travDepth};
}

SbExpr generateProjectionFromEffects(StageBuilderState& state,
                                     const FieldEffects& effects,
                                     SbExpr inputExpr,
                                     const PlanStageSlots& slots,
                                     bool shouldProduceBson) {
    ProjectActions pa = evaluateFieldEffects(state, effects, slots);
    return generateObjectExpr(state, std::move(pa), std::move(inputExpr), shouldProduceBson);
}
}  // namespace mongo::stage_builder
