/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/stage_dependencies.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/logv2/log.h"

#include <queue>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {
namespace {

TransformHoistPolicyEnum getTransformHoistPolicy() {
    return ServerParameterSet::getNodeParameterSet()
        ->get<TransformHoistPolicy>("internalQueryTransformHoistPolicy")
        ->_data.get();
}

/**
 * Returns true if 'deps' has only path dependencies.
 */
bool hasStrictPathDependencies(const DepsTracker& deps) {
    if (deps.needWholeDocument || deps.needRandomGenerator) {
        return false;
    }
    return true;
}

/**
 * Helper to get the dependencies for expression.
 * Precondition: 'expr' has only path dependencies
 */
OrderedPathSet extractStrictPathDependencies(const Expression& expr) {
    DepsTracker deps = expression::getDependencies(&expr);
    tassert(10544901,
            "Expressions should not have a whole-document dependency or use RNG",
            !deps.needWholeDocument && !deps.needRandomGenerator);
    return std::move(deps.fields);
}

/**
 * Contains details about the paths modified or preserved by a transformation.
 */
struct PathTransformationDetails {
    // All new paths introduced by the transformation, with their dependencies.
    // {modifiedPath -> dependencies of modifiedPath}
    StringMap<OrderedPathSet> modifiedPathOperands;
    // True if inclusion projection.
    bool isInclusion{true};
    // All paths preserved by inclusion projection.
    OrderedPathSet preservedPaths;
};

/**
 * Produces details from the given transformation stage.
 */
PathTransformationDetails getPathTransformationDetails(
    const DocumentSourceSingleDocumentTransformation& transform) {
    using namespace mongo::document_transformation;

    PathTransformationDetails details;
    details.isInclusion = transform.getTransformerType() ==
        TransformerInterface::TransformerType::kInclusionProjection;
    describeTransformation(
        OverloadedVisitor{
            [](const ReplaceRoot&) {},
            [&](const ModifyPath& op) {
                auto expr = op.getExpression();
                tassert(10544902,
                        fmt::format("Expected to have an expression for {}", op.getPath()),
                        expr);
                details.modifiedPathOperands.emplace(op.getPath(),
                                                     extractStrictPathDependencies(*expr));
            },
            [&](const PreservePath& op) { details.preservedPaths.emplace(op.getPath()); },
            [&](const RenamePath& op) {
                details.modifiedPathOperands.emplace(op.getNewPath(),
                                                     OrderedPathSet{std::string{op.getOldPath()}});
            },
        },
        transform);
    return details;
}

/**
 * Returns all paths updated by the previous stage, or boost::none if the set is not finite.
 */
boost::optional<OrderedPathSet> getOutputPaths(const DocumentSource& stage) {
    using namespace mongo::document_transformation;
    bool affectsAllPaths = false;
    OrderedPathSet outputPaths;
    describeTransformation(OverloadedVisitor{
                               [&](const ReplaceRoot&) { affectsAllPaths = true; },
                               [&](const ModifyPath& op) { outputPaths.emplace(op.getPath()); },
                               [](const PreservePath& op) {},
                               [&](const RenamePath& op) { outputPaths.emplace(op.getNewPath()); },
                           },
                           stage);
    if (affectsAllPaths) {
        return boost::none;
    }
    return outputPaths;
}

/**
 * Defines which computations of a transformation can be hoisted.
 */
struct HoistPlan {
    /// Computations which can be hoisted to an earlier transformation.
    OrderedPathSet hoistablePaths;
    /// Paths which must remain in the existing transformation.
    OrderedPathSet residualPaths;
};

/**
 * Removes from 'hoistablePaths' any path that is depended on by a path in the remainder
 * (i.e. a path in modifiedPathOperands that is NOT in hoistablePaths).
 *
 * Uses BFS (queue): each residual path is processed once, and any hoistable path it reads is also
 * moved to the remainder. Linear when cross-dependencies are few, quadratic in the worst case where
 * a long dependency chain causes one path to be removed per step.
 */
void pruneHoistableByPinnedDependences(OrderedPathSet& hoistablePaths,
                                       const StringMap<OrderedPathSet>& modifiedPathOperands) {
    // BFS queue, starts with the initial remainder and extended as paths are pruned.
    std::queue<std::string> queue;
    // Used to ensure each path is processed at most once.
    StringSet visited;

    for (auto& [path, deps] : modifiedPathOperands) {
        if (!hoistablePaths.contains(path)) {
            queue.push(path);
        }
    }

    while (!queue.empty()) {
        std::string path = std::move(queue.front());
        queue.pop();

        if (!visited.insert(std::string(path)).second) {
            continue;
        }

        auto pathIt = modifiedPathOperands.find(path);
        dassert(pathIt != modifiedPathOperands.end());
        const OrderedPathSet& pathOperands = pathIt->second;

        for (auto it = hoistablePaths.begin(); it != hoistablePaths.end();) {
            if (!expression::areIndependent(pathOperands, {*it})) {
                queue.push(*it);
                it = hoistablePaths.erase(it);
            } else {
                ++it;
            }
        }
    }
}

/**
 * Returns a HoistPlan which can be used to hoist independent computations before the previous
 * stage. Returns boost::none if no such plan exists.
 *
 * 'previousModifiedPaths' are the fields the previous stage modifies.
 * 'previousDependencies' are the fields the previous stage depends on.
 *
 * A computation is ineligible to be hoisted if:
 * - its expression reads any field the previous stage modifies (true dependency)
 * - the previous stage reads this path (anti-dependency)
 * - the previous stage writes this path (output dependency)
 * - the remaining computations reads the path it modifies (true dependency after split)
 */
boost::optional<HoistPlan> computeHoistPlan(const PathTransformationDetails& transform,
                                            const OrderedPathSet& previousOutputPaths,
                                            const OrderedPathSet& previousDependencies) {
    OrderedPathSet hoistablePaths;
    for (auto&& [path, exprDeps] : transform.modifiedPathOperands) {
        // Skip if the previous stage writes this path
        if (!expression::areIndependent({path}, previousOutputPaths)) {
            continue;
        }

        // Skip if the previous stage depends on this computed path.
        if (!expression::areIndependent(previousDependencies, {path})) {
            continue;
        }

        // Skip if the expression reads a field that overlaps with what the previous stage writes,
        // including ancestor/descendant relationships.
        if (!expression::areIndependent(exprDeps, previousOutputPaths)) {
            continue;
        }

        hoistablePaths.emplace(path);
    }

    // Hoisting can change existing dependencies. We need to ensure that the paths we've identified
    // are not be depended on by the residual part.
    pruneHoistableByPinnedDependences(hoistablePaths, transform.modifiedPathOperands);

    if (hoistablePaths.empty()) {
        return boost::none;
    }

    HoistPlan plan;
    for (auto&& [path, deps] : transform.modifiedPathOperands) {
        if (hoistablePaths.contains(path)) {
            plan.hoistablePaths.emplace(path);
        } else {
            plan.residualPaths.emplace(path);
        }
    }
    return plan;
}

/**
 * Returns true if the the transform supports hoisting.
 */
bool isHoistableTransform(const DocumentSourceSingleDocumentTransformation& transform) {
    switch (transform.getTransformerType()) {
        case TransformerInterface::TransformerType::kExclusionProjection:
        case TransformerInterface::TransformerType::kReplaceRoot:
        case TransformerInterface::TransformerType::kGroupFromFirstDocument:
        case TransformerInterface::TransformerType::kSetMetadata:
            return false;
        case TransformerInterface::TransformerType::kInclusionProjection:
        case TransformerInterface::TransformerType::kComputedProjection:
            break;
    }

    if (!hasStrictPathDependencies(getDependencies(transform))) {
        // We cannot determine which fields to split in this case or we cannot do so without
        // changing semantics.
        // Example:
        // {a: '$$ROOT', b: 1} -> {b: 1} {a: '$$ROOT'}
        // This is invalid because 'a' will see the new value of 'b'.
        return false;
    }

    return true;
}

/**
 * Checks if the transformation is hoistable and if we should attempt the rewrite.
 */
bool shouldAttemptToHoistTransform(const PipelineRewriteContext& ctx) {
    const auto policy = getTransformHoistPolicy();
    if (policy == TransformHoistPolicyEnum::kNever) {
        LOGV2_DEBUG(10544911, 4, "shouldAttemptToHoistTransform: blocked - knob disabled");
        return false;
    }

    if (ctx.atFirstStage()) {
        LOGV2_DEBUG(10544903, 4, "shouldAttemptToHoistTransform: blocked - at first stage");
        return false;
    }

    if (ctx.prevStage()->constraints().requiredPosition !=
        StageConstraints::PositionRequirement::kNone) {
        LOGV2_DEBUG(
            10544914,
            4,
            "shouldAttemptToHoistTransform: blocked - previous stage has position requirement");
        return false;
    }

    if (dynamic_cast<const DocumentSourceMatch*>(ctx.prevStage().get())) {
        // Block hoisting immediately before a $match.
        // Such a rewrite could be beneficial, but it might trigger a rewrite loop.
        // Example:
        // {$replaceRoot: ...} {$match: {a: 1}} {$set: {b: <expr>]}}
        // {$replaceRoot: ...} {$set: {b: <expr>]}} {$match: {a: 1}}
        // {$replaceRoot: ...} {$match: {a: 1}} {$set: {b: <expr>]}} (same as initial)
        // ...
        // Example where this could be USEFUL:
        // {$lookup: {as: 'x'}}
        // {$match: {x: ...}}
        // {$set: {a: {$add: ['$a', 2]}}}
        // {$match: {a: {$lt: 50}}}
        // With this condition, we PREVENT the rewrite to:
        // {$set: {a: {$add: ['$a', 2]}}}
        // {$match: {a: {$lt: 50}}}
        // {$lookup: {as: 'x'}}
        // {$match: {x: ...}}
        LOGV2_DEBUG(
            10544913, 4, "shouldAttemptToHoistTransform: blocked - previous stage is $match");
        return false;
    }

    if (policy == TransformHoistPolicyEnum::kForMatchPushdown) {
        // If the next stage is not a $match, we should not attempt the rewrite, even through it
        // might be legal. This is just because when this rewrite was added, it was intended to
        // specifically enable $match pushdown.
        if (ctx.atLastStage()) {
            LOGV2_DEBUG(10544904, 4, "shouldAttemptToHoistTransform: blocked - at last stage");
            return false;
        }
        if (!dynamic_cast<const DocumentSourceMatch*>(ctx.nextStage().get())) {
            LOGV2_DEBUG(10544905,
                        4,
                        "shouldAttemptToHoistTransform: blocked - following stage is not $match",
                        "nextStageName"_attr = ctx.nextStage()->getSourceName());
            return false;
        }
        if (!ctx.prevStage()->constraints().canSwapWithMatch) {
            LOGV2_DEBUG(
                10544915,
                4,
                "shouldAttemptToHoistTransform: blocked - previous stage does not swap with $match",
                "nextStageName"_attr = ctx.nextStage()->getSourceName());
            return false;
        }
    }

    auto& transform =
        checked_cast<const DocumentSourceSingleDocumentTransformation&>(ctx.current());
    bool isHoistable = isHoistableTransform(transform);
    if (!isHoistable) {
        LOGV2_DEBUG(10544906,
                    4,
                    "shouldAttemptToHoistTransform: blocked - not hoistable",
                    "transform"_attr = redact(transform.serializeToBSONForDebug()));
        return false;
    }

    if (auto prevTransform =
            dynamic_cast<const DocumentSourceSingleDocumentTransformation*>(ctx.prevStage().get());
        prevTransform && isHoistableTransform(*prevTransform)) {
        // If the previous transform is also hoistable, we should not hoist this one. This could
        // create a loop as we optimize the pipeline where they both swap with each other. It also
        // prevents the relative order of the transforms from changing in indeterminate ways. The
        // "correct" behaviour would be to merge them, but we do not do this.
        LOGV2_DEBUG(10544910,
                    4,
                    "shouldAttemptToHoistTransform: blocked - previous transform is also hoistable",
                    "transform"_attr = redact(transform.serializeToBSONForDebug()));
        return false;
    }

    return true;
}

/**
 * Create the pair [hoistableStage, residualStage] from the given transformation stage,
 * according to 'plan'.
 */
std::pair<boost::intrusive_ptr<DocumentSource>, boost::intrusive_ptr<DocumentSource>>
splitTransformation(const DocumentSourceSingleDocumentTransformation& transform,
                    const PathTransformationDetails& details,
                    const HoistPlan& plan,
                    ExpressionContext& expCtx) {
    // Use the serialized spec for splitting expressions.
    Document originalSpec =
        transform.getTransformer().serializeTransformation({.serializeForCloning = true});
    // The two new specs.
    MutableDocument hoistedComputationSpec;
    MutableDocument residualTransformationSpec;

    for (auto&& path : plan.hoistablePaths) {
        FieldPath fieldPath(path);
        hoistedComputationSpec.setNestedField(fieldPath, originalSpec.getNestedField(fieldPath));
        if (details.isInclusion) {
            residualTransformationSpec.setNestedField(fieldPath, Value(true));
        }
    }

    for (auto&& path : plan.residualPaths) {
        FieldPath fieldPath(path);
        residualTransformationSpec.setNestedField(fieldPath,
                                                  originalSpec.getNestedField(fieldPath));
    }

    tassert(10544907,
            "Expected inclusion transformation",
            details.isInclusion || details.preservedPaths.empty());
    for (auto&& path : details.preservedPaths) {
        FieldPath fieldPath(path);
        residualTransformationSpec.setNestedField(fieldPath,
                                                  originalSpec.getNestedField(fieldPath));
    }

    // Preserve the _id: false exclusion special case for inclusion projections.
    if (details.isInclusion) {
        auto idProjection = originalSpec.getField("_id");
        if (!idProjection.missing() && !plan.hoistablePaths.contains("_id")) {
            residualTransformationSpec.setField("_id", idProjection);
        }
    }

    // If the original stage is set/addFields, use the same name for the split component.
    // If it is a $project, use the canonical name for DocumentSourceAddFields.
    auto hoistedStageName =
        details.isInclusion ? DocumentSourceAddFields::kStageName : transform.getSourceName();

    auto hoistedStage = DocumentSourceAddFields::create(
        hoistedComputationSpec.freeze().toBson(), &expCtx, hoistedStageName);

    auto residualStage = details.isInclusion
        ? DocumentSourceProject::create(
              residualTransformationSpec.freeze().toBson(), &expCtx, transform.getSourceName())
        : DocumentSourceAddFields::create(
              residualTransformationSpec.freeze().toBson(), &expCtx, transform.getSourceName());

    return {hoistedStage, residualStage};
}

/**
 * Returns true if at least one AND-level predicate in 'match' is on a hoisted path and is only
 * dependent on the hoisted set. Such predicates can be pushed past the residual stage after
 * hoisting.
 */
bool canHoistEnableMatchPushdown(const DocumentSourceMatch& match,
                                 const OrderedPathSet& hoistedPaths) {
    auto canPushdownPastResidualPaths = [&](const MatchExpression& pred) -> bool {
        return expression::isOnlyDependentOnConst(pred, hoistedPaths);
    };

    const auto& expr = *match.getMatchExpression();
    if (expr.matchType() == MatchExpression::MatchType::AND) {
        for (size_t i = 0; i < expr.numChildren(); ++i) {
            if (canPushdownPastResidualPaths(*expr.getChild(i))) {
                return true;
            }
        }
        return false;
    }

    return canPushdownPastResidualPaths(expr);
}

/**
 * Hoist some or all computations of the transformation before the previous stage.
 * Precondition: canHoistSingleDocTransformBeforeStage(ctx) returns true.
 *
 * Field order is not preserved, but this is explicitly allowed in MongoDB and documented.
 *
 * This rewrite is beneficial when a subset of the transformation's computations are independent of
 * the previous stage - their expressions don't read fields the previous stage writes, and the
 * previous stage doesn't read the fields they produce.
 *
 * This in turn can allow a predicate to be split across the remainder transformation and pushed
 * down as well with the MATCH_PUSHDOWN rule, which is the key optimization we want to enable.
 *
 * Examples:
 * The previous stage is {$lookup: {as: 'x', ...}} (adds field 'x', does not depend on 'a' or 'b').
 *
 *     {$lookup: {as: 'x'}} {$project: {a: <expr_a>, b: <expr_b(x)>}}
 * ->  {$set: {a: <expr_a>}} {$lookup: {as: 'x'}} {$project: {a: 1, b: <expr_b(x)>}}
 *     (if the expression for 'a' does not depend on 'x', and {$lookup} does not depend on 'a')
 *
 *     {$lookup: {as: 'x'}} {$set: {a: <expr_a>, b: <expr_b(x)>}}
 * ->  {$set: {a: <expr_a>}}  {$lookup: {as: 'x'}} {$set: {b: <expr_b(x)>}}
 *     (if the expression for 'a' does not depend on 'x', and {$lookup} does not depend on 'a')
 *
 *     {$lookup: {as: 'x'}} {$set: {a: <expr_a>, b: <expr_b(a)>}}
 * ->  FAIL: Cannot split this since 'expr_b' would see the new 'a'.
 */
bool hoistSingleDocTransformBeforeStage(PipelineRewriteContext& ctx) {
    dassert(shouldAttemptToHoistTransform(ctx));
    auto& transform =
        checked_cast<const DocumentSourceSingleDocumentTransformation&>(ctx.current());
    dassert(isHoistableTransform(transform));

    const auto& prev = *ctx.prevStage().get();

    auto prevOutputPaths = getOutputPaths(prev);
    if (!prevOutputPaths) {
        return false;
    }

    DepsTracker prevDeps = getDependencies(prev);
    if (!hasStrictPathDependencies(prevDeps)) {
        // If the previous stage needs the whole document or uses RNG, we cannot enumerate which
        // of its dependencies conflict with the computed paths.
        LOGV2_DEBUG(10544908,
                    4,
                    "hoistSingleDocTransformBeforeStage: blocked - previous stage does not have "
                    "strict path dependencies");
        return false;
    }

    auto details = getPathTransformationDetails(transform);
    const size_t numPaths = details.modifiedPathOperands.size();
    const size_t maxPaths = static_cast<size_t>(internalQueryTransformHoistMaximumPaths.load());
    if (numPaths > maxPaths) {
        LOGV2_DEBUG(10622200,
                    4,
                    "hoistSingleDocTransformBeforeStage: blocked - too many projection paths",
                    "numPaths"_attr = numPaths,
                    "maxPaths"_attr = maxPaths);
        return false;
    }

    auto plan = computeHoistPlan(details, *prevOutputPaths, prevDeps.fields);
    if (!plan) {
        LOGV2_DEBUG(
            10544916, 4, "hoistSingleDocTransformBeforeStage: blocked - no valid hoist plan");
        return false;
    }

    if (getTransformHoistPolicy() == TransformHoistPolicyEnum::kForMatchPushdown) {
        // At least one predicate in the following $match must be on a hoisted path and the
        // predicate cannot have a dependency on a residual path. When residualPaths is empty the
        // whole transform is being moved (swapStageWithPrev below), so the check is moot. The check
        // also only applies when a $match follows.
        if (!plan->residualPaths.empty() && !ctx.atLastStage()) {
            const auto* nextMatch = dynamic_cast<const DocumentSourceMatch*>(ctx.nextStage().get());
            if (nextMatch && !canHoistEnableMatchPushdown(*nextMatch, plan->hoistablePaths)) {
                LOGV2_DEBUG(10544912,
                            4,
                            "hoistSingleDocTransformBeforeStage: blocked - no $match predicate "
                            "benefits from hoist");
                return false;
            }
        }
    }

    if (!details.isInclusion && plan->residualPaths.empty()) {
        // We can just push down the existing computation stage and exit.
        LOGV2_DEBUG(10544917,
                    4,
                    "hoistSingleDocTransformBeforeStage: swap with previous",
                    "prevStage"_attr = redact(prev.serializeToBSONForDebug()),
                    "currStage"_attr = redact(transform.serializeToBSONForDebug()));
        return Transforms::swapStageWithPrev(ctx);
    }

    // Use the serialized spec for moving expressions.
    Document originalSpec =
        transform.getTransformer().serializeTransformation({.serializeForCloning = true});

    auto [hoistedStage, residualStage] =
        splitTransformation(transform, details, *plan, ctx.getExpCtx());

    LOGV2_DEBUG(10544909,
                4,
                "hoistSingleDocTransformBeforeStage: split",
                "hoistedStage"_attr = redact(hoistedStage->serializeToBSONForDebug()),
                "residualStage"_attr = redact(residualStage->serializeToBSONForDebug()));

    // prev -> transform
    //         ^^^^^^^  cursor
    Transforms::replaceCurrentStage(ctx, residualStage);
    // prev -> residualStage
    // ^^^^  cursor
    Transforms::insertAfter(ctx, *hoistedStage);
    // prev -> hoistedStage -> residualStage
    // ^^^^  cursor
    Transforms::swapStageWithNext(ctx);
    // hoistedStage -> prev -> residualStage
    // ^^^^^^^^^^^^  cursor (or the stage before, if any)
    dassert(&ctx.current() == hoistedStage.get() || ctx.nextStage().get() == hoistedStage.get());
    return true;
}

}  // namespace

// HOIST_SINGLE_DOC_TRANSFORMATION moves some (or all) of the computations in a
// $set/$addFields/$project to before the preceding stage, so they execute earlier in the
// pipeline. This can enable other rewrites, such as MATCH_PUSHDOWN.
REGISTER_RULES_WITH_FEATURE_FLAG(DocumentSourceSingleDocumentTransformation,
                                 &feature_flags::gFeatureFlagImprovedDepsAnalysis,
                                 {
                                     .name = "HOIST_SINGLE_DOC_TRANSFORMATION",
                                     .precondition = shouldAttemptToHoistTransform,
                                     .transform = hoistSingleDocTransformBeforeStage,
                                     .priority = kDefaultHoistPriority,
                                     .tags = PipelineRewriteContext::Tags::Reordering,
                                 });
}  // namespace mongo::rule_based_rewrites::pipeline
