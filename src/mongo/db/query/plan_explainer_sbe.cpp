// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_explainer_sbe.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
/**
 * This function replaces field names in *replace* with those from the object
 * *fieldNames*, preserving field ordering.  Both objects must have the same
 * number of fields.
 *
 * Example:
 *
 *     replaceBSONKeyNames({ 'a': 1, 'b' : 1 }, { '': 'foo', '', 'bar' }) =>
 *
 *         { 'a' : 'foo' }, { 'b' : 'bar' }
 */
BSONObj replaceBSONFieldNames(const BSONObj& replace, const BSONObj& fieldNames) {
    tassert(
        11321411,
        fmt::format("'replace' and 'fieldNames' must have the same number of fields, but {} != {}",
                    replace.nFields(),
                    fieldNames.nFields()),
        replace.nFields() == fieldNames.nFields());

    BSONObjBuilder bob;
    auto iter = fieldNames.begin();

    for (const BSONElement& el : replace) {
        bob.appendAs(el, (*iter++).fieldNameStringData());
    }

    return bob.obj();
}

void statsToBSONHelper(const sbe::PlanStageStats* stats,
                       BSONObjBuilder* bob,
                       const BSONObjBuilder* topLevelBob,
                       std::uint32_t currentDepth) {
    tassert(9378607, "encountered unexpected nullptr for PlanStageStats", stats);
    tassert(9378608, "encountered unexpected nullptr for BSONObjBuilder", bob);
    tassert(9258809, "encountered unexpected nullptr for BSONObjBuilder", topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > internalQueryExplainSizeThresholdBytes.load()) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    // Stop as soon as the BSON object we're building becomes too deep. Note that we go 2 less
    // than the max depth to account for when this stage has multiple children.
    if (currentDepth >= BSONDepth::getMaxDepthForUserStorage() - 2) {
        bob->append("warning",
                    "stats tree exceeded BSON depth limit; omitting the rest of the tree");
        return;
    }

    auto stageType = stats->common.stageType;
    bob->append("stage", stageType);
    bob->appendNumber("planNodeId", static_cast<long long>(stats->common.nodeId));

    // Some top-level exec stats get pulled out of the root stage.
    bob->appendNumber("nReturned", static_cast<long long>(stats->common.advances));
    // Include the execution time if it was recorded.
    appendExecutionTimeFields(*bob, stats->common.executionTime);
    bob->appendNumber("opens", static_cast<long long>(stats->common.opens));
    bob->appendNumber("closes", static_cast<long long>(stats->common.closes));
    bob->appendNumber("saveState", static_cast<long long>(stats->common.yields));
    bob->appendNumber("restoreState", static_cast<long long>(stats->common.unyields));
    bob->appendNumber("isEOF", stats->common.isEOF);

    // Include any extra debug info if present.
    bob->appendElements(stats->debugInfo);

    // We're done if there are no children.
    if (stats->children.empty()) {
        return;
    }

    // If there's just one child (a common scenario), avoid making an array. This makes
    // the output more readable by saving a level of nesting. Name the field 'inputStage'
    // rather than 'inputStages'.
    if (stats->children.size() == 1) {
        BSONObjBuilder childBob(bob->subobjStart("inputStage"));
        statsToBSONHelper(stats->children[0].get(), &childBob, topLevelBob, currentDepth + 1);
        return;
    }

    // For some stages we may want to output its children under different field names.
    auto overridenNames = [stageType]() -> std::vector<std::string_view> {
        if (stageType == "branch"sv) {
            return {"thenStage"sv, "elseStage"sv};
        } else if (stageType == "nlj"sv || stageType == "traverse"sv || stageType == "mj"sv ||
                   stageType == "hj"sv) {
            return {"outerStage"sv, "innerStage"sv};
        }
        return {};
    }();
    if (!overridenNames.empty()) {
        invariant(overridenNames.size() == stats->children.size());

        for (size_t idx = 0; idx < stats->children.size(); ++idx) {
            BSONObjBuilder childBob(bob->subobjStart(overridenNames[idx]));
            statsToBSONHelper(stats->children[idx].get(), &childBob, topLevelBob, currentDepth + 1);
        }
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.
    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"sv));
    for (auto&& child : stats->children) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSONHelper(child.get(), &childBob, topLevelBob, currentDepth + 2);
    }
    childrenBob.doneFast();
}

void statsToBSON(const sbe::PlanStageStats* stats,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob) {
    statsToBSONHelper(stats, bob, topLevelBob, 0);
}

/**
 * Whether 'solution' still carries the pushed-down pipeline that extended it upward after its
 * find part was ranked. When it does, the tree that executes is not the tree that was ranked.
 */
bool hasSurvivingExtension(const QuerySolution& solution) {
    return solution.hasExtension() && solution.root()->nodeId() != solution.unextendedRootId();
}

/**
 * Returns the root of the ranked part of 'solution'. When the QSN is extended (e.g. via $group
 * pushdown to SBE), this will be different than the current root.
 */
const QuerySolutionNode* rankedRootOf(const QuerySolution& solution) {
    if (!hasSurvivingExtension(solution)) {
        return solution.root();
    }
    for (const QuerySolutionNode* node = solution.root(); node;) {
        if (node->nodeId() <= solution.unextendedRootId()) {
            return node;
        }
        if (node->children.empty()) {
            break;
        }
        node = node->children[0].get();
    }
    return solution.root();
}

/**
 * Builds the stringified SBE plan for 'root' within 'lengthCap'. Returns boost::none when the
 * tree does not fit even after being capped at that budget.
 */
boost::optional<BSONObj> buildSbeDebugInfoWithinBudget(const sbe::PlanStage* root,
                                                       const stage_builder::PlanStageData* data,
                                                       int lengthCap,
                                                       bool printBytecode) {
    if (lengthCap < 0) {
        return boost::none;
    }
    BSONObj debugInfo = PlanExplainerSBEBase::buildExecPlanDebugInfo(
        root, data, static_cast<size_t>(lengthCap), printBytecode);
    // buildExecPlanDebugInfo() caps the pieces it appends, but the object it returns can still
    // exceed the budget, in which case there is no room for it at all.
    if (debugInfo.objsize() > lengthCap) {
        return boost::none;
    }
    return debugInfo;
}

PlanExplainer::PlanStatsDetails buildPlanStatsDetails(
    const QuerySolution* solution,
    const sbe::PlanStageStats& stats,
    const sbe::PlanStage* sbePlanStageRoot,
    const stage_builder::PlanStageData* sbePlanStageData,
    const boost::optional<std::string>& planSummary,
    const boost::optional<BSONObj>& queryParams,
    const boost::optional<BSONArray>& remotePlanInfo,
    ExplainOptions::Verbosity verbosity,
    bool isCached,
    bool printBytecode,
    bool usedJoinOpt = false,
    const cost_based_ranker::EstimateMap& _estimates = {},
    boost::optional<size_t> preExtensionSolutionHash = boost::none) {
    BSONObjBuilder bob;

    // The winning solution may have been extended with a pushed-down pipeline, which changes its
    // hash(). Report the pre-extension hash if stored, so a later command using this hash as
    // forcedPlanSolutionHash still matches this plan.
    const auto forceableHash = [&] {
        return (long long)preExtensionSolutionHash.value_or(solution->hash());
    };

    const ExplainPolicy explainPolicy = explainPolicyFor(verbosity);
    if (explainPolicy.hasExecStats()) {
        auto summary = sbe::collectExecutionStatsSummary(stats);
        if (solution != nullptr && explainPolicy.hasAllPlansStats()) {
            summary.score = solution->score;
            if (internalQueryAllowForcedPlanByHash.load()) {
                bob.append("solutionHashUnstable", forceableHash());
            }
        }
        statsToBSON(&stats, &bob, &bob);
        // At the 'kQueryPlanner' verbosity level we use the QSN-derived format for the given plan,
        // and thus the winning plan and rejected plans at this verbosity should display the
        // stringified SBE plan, which is added below. However, at the 'kExecStats' the execution
        // stats use the PlanStage-derived format for the SBE tree, so there is no need to repeat
        // the stringified SBE plan and we only included what's been generated from the
        // PlanStageStats.
        return {bob.obj(), std::move(summary)};
    }

    if (solution != nullptr) {
        statsToBSON(solution->root(), &bob, &bob, _estimates);
        if (internalQueryAllowForcedPlanByHash.load()) {
            bob.append("solutionHashUnstable", forceableHash());
        }
    }

    BSONObjBuilder plan;
    if (planSummary) {
        plan.append("planSummary", *planSummary);
    }

    plan.append("isCached", isCached);
    // Only include information about join optimization in explain output if the relevant knob is
    // enabled.
    if (internalEnableJoinOptimization.load()) {
        plan.append("usedJoinOptimization", usedJoinOpt);
    }

    plan.append("queryPlan", bob.obj());

    int explainThresholdBytes = internalQueryExplainSizeThresholdBytes.loadRelaxed();
    if (auto execPlanDebugInfo =
            buildSbeDebugInfoWithinBudget(sbePlanStageRoot,
                                          sbePlanStageData,
                                          explainThresholdBytes - plan.len() /* lengthCap */,
                                          printBytecode)) {
        plan.append("slotBasedPlan", *execPlanDebugInfo);
    } else {
        plan.append("warning", "slotBasedPlan exceeded BSON size limit for explain");
    }
    if (remotePlanInfo && !remotePlanInfo->isEmpty()) {
        plan.append("remotePlans", *remotePlanInfo);
    }
    return {plan.obj(), boost::none};
}
/**
 * Serializes 'node' into 'bob' with the output dependent on 'format' and 'explainPolicy':
 * - How the estimates are grouped: fused flat onto the node (kLegacy) versus under a sparse
 *   "statistics.costBased" subobject (kV3). Whether they appear at all is 'explainPolicy' alone,
 * the same predicate for both shapes.
 * - Child nesting: a lone child collapses into an "inputStage" object (kLegacy) versus children
 *   always forming the "inputStages" array (kV3).
 *
 * 'topLevelBob' is only read to size-guard against the overall explain object.
 */
void qsnToBson(const QuerySolutionNode* node,
               PlanStatsFormat format,
               const ExplainPolicy& explainPolicy,
               const cost_based_ranker::EstimateMap& estimates,
               BSONObjBuilder* bob,
               const BSONObjBuilder* topLevelBob,
               std::uint32_t currentDepth) {
    tassert(9378604, "encountered unexpected nullptr for BSONObjBuilder", bob);
    tassert(9378605, "encountered unexpected nullptr for BSONObjBuilder", topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > internalQueryExplainSizeThresholdBytes.load()) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    // Stop as soon as the BSON object we're building becomes too deep. Use the worst-case (+2)
    // depth increment to account for stages which emit children under an array.
    if (currentDepth + 2 >= BSONDepth::getMaxDepthForUserStorage()) {
        bob->append("warning",
                    "stats tree exceeded BSON depth limit; omitting the rest of the tree");
        return;
    }

    const bool isV3 = format == PlanStatsFormat::kV3;
    const bool hasCostEstimates = explainPolicy.hasCostBasedStats() && estimates.contains(node);

    bob->append("stage", nodeStageTypeToString(node));
    bob->appendNumber("planNodeId", static_cast<long long>(node->nodeId()));

    // Stage-specific stats. Structural fields stay flat on the node in both shapes.
    switch (node->getType()) {
        case STAGE_COLLSCAN: {
            auto csn = static_cast<const CollectionScanNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(csn->nss, SerializationContext::stateDefault()));
            bob->append("direction", csn->direction > 0 ? "forward" : "backward");

            const auto outerBounds = csn->rangeList.outerBounds();
            if (outerBounds.getMin()) {
                outerBounds.getMin()->appendToBSONAs(bob, "minRecord");
            }
            if (outerBounds.getMax()) {
                outerBounds.getMax()->appendToBSONAs(bob, "maxRecord");
            }

            // No need to emit an additional list of ranges if there is a single range.
            if (csn->rangeList.getRanges().size() != 1) {
                bob->append("recordIdRanges", csn->rangeList.toBSONArray());
            }
            break;
        }
        case STAGE_COUNT_SCAN: {
            auto csn = static_cast<const CountScanNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(csn->nss, SerializationContext::stateDefault()));
            bob->append("keyPattern", csn->index.keyPattern);
            bob->append("indexName", csn->index.identifier.catalogName);
            auto collation =
                csn->index.infoObj.getObjectField(IndexDescriptor::kCollationFieldName);
            if (!collation.isEmpty()) {
                bob->append("collation", collation);
            }
            bob->appendBool("isMultiKey", csn->index.multikey);
            if (!csn->index.multikeyPaths.empty()) {
                appendMultikeyPaths(csn->index.keyPattern, csn->index.multikeyPaths, bob);
            }
            bob->appendBool("isUnique", csn->index.unique);
            bob->appendBool("isSparse", csn->index.sparse);
            bob->appendBool("isPartial", csn->index.filterExpr != nullptr);
            bob->append("indexVersion", static_cast<int>(csn->index.version));

            BSONObjBuilder indexBoundsBob(bob->subobjStart("indexBounds"));
            indexBoundsBob.append("startKey",
                                  replaceBSONFieldNames(csn->startKey, csn->index.keyPattern));
            indexBoundsBob.append("startKeyInclusive", csn->startKeyInclusive);
            indexBoundsBob.append("endKey",
                                  replaceBSONFieldNames(csn->endKey, csn->index.keyPattern));
            indexBoundsBob.append("endKeyInclusive", csn->endKeyInclusive);
            break;
        }
        case STAGE_GEO_NEAR_2D: {
            auto geo2d = static_cast<const GeoNear2DNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(geo2d->nss, SerializationContext::stateDefault()));
            bob->append("keyPattern", geo2d->index.keyPattern);
            bob->append("indexName", geo2d->index.identifier.catalogName);
            bob->append("indexVersion", geo2d->index.version);
            break;
        }
        case STAGE_GEO_NEAR_2DSPHERE: {
            auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
            bob->append("nss",
                        NamespaceStringUtil::serialize(geo2dsphere->nss,
                                                       SerializationContext::stateDefault()));
            bob->append("keyPattern", geo2dsphere->index.keyPattern);
            bob->append("indexName", geo2dsphere->index.identifier.catalogName);
            bob->append("indexVersion", geo2dsphere->index.version);
            break;
        }
        case STAGE_IXSCAN: {
            auto ixn = static_cast<const IndexScanNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(ixn->nss, SerializationContext::stateDefault()));
            bob->append("keyPattern", ixn->index.keyPattern);
            bob->append("indexName", ixn->index.identifier.catalogName);
            auto collation =
                ixn->index.infoObj.getObjectField(IndexDescriptor::kCollationFieldName);
            if (!collation.isEmpty()) {
                bob->append("collation", collation);
            }
            bob->appendBool("isMultiKey", ixn->index.multikey);
            if (!ixn->index.multikeyPaths.empty()) {
                appendMultikeyPaths(ixn->index.keyPattern, ixn->index.multikeyPaths, bob);
            }
            bob->appendBool("isUnique", ixn->index.unique);
            bob->appendBool("isSparse", ixn->index.sparse);
            bob->appendBool("isPartial", ixn->index.filterExpr != nullptr);
            bob->append("indexVersion", static_cast<int>(ixn->index.version));
            bob->append("direction", ixn->direction > 0 ? "forward" : "backward");

            auto bounds = ixn->bounds.toBSON(!collation.isEmpty());
            if (topLevelBob->len() + bounds.objsize() >
                internalQueryExplainSizeThresholdBytes.load()) {
                bob->append("warning", "index bounds omitted due to BSON size limit for explain");
            } else {
                bob->append("indexBounds", bounds);
            }
            break;
        }
        case STAGE_LIMIT: {
            auto ln = static_cast<const LimitNode*>(node);
            bob->appendNumber("limitAmount", ln->limit);
            break;
        }
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_SIMPLE:
        case STAGE_PROJECTION_COVERED: {
            auto pn = static_cast<const ProjectionNode*>(node);
            bob->append("transformBy", projection_ast::astToDebugBSON(pn->proj.root()));
            break;
        }
        case STAGE_SKIP: {
            auto sn = static_cast<const SkipNode*>(node);
            bob->appendNumber("skipAmount", sn->skip);
            break;
        }
        case STAGE_SORT_SIMPLE:
        case STAGE_SORT_DEFAULT: {
            auto sn = static_cast<const SortNode*>(node);
            bob->append("sortPattern", sn->pattern);
            bob->appendNumber("memLimit", static_cast<long long>(sn->maxMemoryUsageBytes));

            if (sn->limit > 0) {
                bob->appendNumber("limitAmount", static_cast<long long>(sn->limit));
            }

            bob->append("type", node->getType() == STAGE_SORT_SIMPLE ? "simple" : "default");
            break;
        }
        case STAGE_SORT_MERGE: {
            auto smn = static_cast<const MergeSortNode*>(node);
            bob->append("sortPattern", smn->sort);
            break;
        }
        case STAGE_TEXT_MATCH: {
            auto tn = static_cast<const TextMatchNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(tn->nss, SerializationContext::stateDefault()));
            bob->append("indexPrefix", tn->indexPrefix);
            bob->append("indexName", tn->index.identifier.catalogName);
            auto ftsQuery = dynamic_cast<fts::FTSQueryImpl*>(tn->ftsQuery.get());
            tassert(11321412, "Invalid dynamic cast to fts::FTSQueryImpl", ftsQuery);
            bob->append("parsedTextQuery", ftsQuery->toBSON());
            bob->append("textIndexVersion", tn->index.version);
            break;
        }
        case STAGE_EQ_LOOKUP:
        case STAGE_EQ_LOOKUP_UNWIND: {
            auto eln = static_cast<const EqLookupNode*>(node);

            bob->append("foreignCollection",
                        NamespaceStringUtil::serialize(eln->foreignCollection,
                                                       SerializationContext::stateDefault()));
            bob->append("localField", eln->joinFieldLocal.fullPath());
            bob->append("foreignField", eln->joinFieldForeign.fullPath());
            bob->append("asField", eln->joinField.fullPath());
            bob->append("strategy", EqLookupNode::serializeLookupStrategy(eln->lookupStrategy));
            if (eln->unwindSpec) {
                BSONObjBuilder unwindBob(bob->subobjStart("unwinding"));
                unwindBob.appendBool("preserveNullAndEmptyArrays",
                                     eln->unwindSpec->preserveNullAndEmptyArrays);
                unwindBob.append("includeArrayIndex",
                                 eln->unwindSpec->indexPath ? eln->unwindSpec->indexPath->fullPath()
                                                            : "");
            }
            break;
        }
        case STAGE_UNPACK_TS_BUCKET: {
            auto utsbn = static_cast<const UnpackTsBucketNode*>(node);
            {
                const auto behaviorField =
                    utsbn->bucketSpec.behavior() == timeseries::BucketSpec::Behavior::kInclude
                    ? "include"
                    : "exclude";
                BSONArrayBuilder fieldsBab{bob->subarrayStart(behaviorField)};
                for (const auto& field : utsbn->bucketSpec.fieldSet()) {
                    fieldsBab.append(field);
                }
                if (utsbn->bucketSpec.behavior() == timeseries::BucketSpec::Behavior::kInclude &&
                    utsbn->includeMeta) {
                    fieldsBab.append(*utsbn->bucketSpec.metaField());
                }
            }
            {
                BSONArrayBuilder fieldsBab{bob->subarrayStart("computedMetaProjFields")};
                for (const auto& computedMeta : utsbn->bucketSpec.computedMetaProjFields()) {
                    fieldsBab.append(computedMeta);
                }
            }
            bob->append("includeMeta", utsbn->includeMeta);
            bob->append("eventFilter",
                        utsbn->eventFilter ? utsbn->eventFilter->serialize() : BSONObj());
            bob->append("wholeBucketFilter",
                        utsbn->wholeBucketFilter ? utsbn->wholeBucketFilter->serialize()
                                                 : BSONObj());

            break;
        }
        case STAGE_SEARCH: {
            auto sn = static_cast<const SearchNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(sn->nss, SerializationContext::stateDefault()));
            bob->append("isSearchMeta", sn->isSearchMeta);
            bob->appendNumber("remoteCursorId", static_cast<long long>(sn->remoteCursorId));
            bob->append("searchQuery", sn->searchQuery);
            break;
        }
        case STAGE_EOF: {
            auto eofn = static_cast<const EofNode*>(node);
            bob->append("type", eof_node::typeStr(eofn->type));
            break;
        }
        case STAGE_FETCH: {
            auto fn = static_cast<const FetchNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(fn->nss, SerializationContext::stateDefault()));
            break;
        }
        case STAGE_HASH_JOIN_EMBEDDING_NODE:
        case STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE:
        case STAGE_INDEXED_NESTED_LOOP_JOIN_EMBEDDING_NODE: {
            // These are all BinaryJoinEmbeddingNodes.
            auto bjen = static_cast<const BinaryJoinEmbeddingNode*>(node);
            bob->append("leftEmbeddingField",
                        (bjen->leftEmbeddingField ? bjen->leftEmbeddingField->fullPath() : "none"));
            bob->append(
                "rightEmbeddingField",
                (bjen->rightEmbeddingField ? bjen->rightEmbeddingField->fullPath() : "none"));
            {
                // Append join predicates.
                BSONArrayBuilder bab;
                for (auto&& jp : bjen->joinPredicates) {
                    bab.append(jp.toString());
                }
                bob->append("joinPredicates", bab.arr());
            }
            break;
        }
        case STAGE_INDEX_PROBE_NODE: {
            auto ipn = static_cast<const IndexProbeNode*>(node);
            bob->append(
                "nss",
                NamespaceStringUtil::serialize(ipn->nss, SerializationContext::stateDefault()));
            bob->append("keyPattern", ipn->index.keyPattern);
            bob->append("indexName", ipn->index.identifier.catalogName);
            auto collation =
                ipn->index.infoObj.getObjectField(IndexDescriptor::kCollationFieldName);
            if (!collation.isEmpty()) {
                bob->append("collation", collation);
            }
            bob->appendBool("isMultiKey", ipn->index.multikey);
            if (!ipn->index.multikeyPaths.empty()) {
                appendMultikeyPaths(ipn->index.keyPattern, ipn->index.multikeyPaths, bob);
            }
            bob->appendBool("isUnique", ipn->index.unique);
            bob->appendBool("isSparse", ipn->index.sparse);
            bob->appendBool("isPartial", ipn->index.filterExpr != nullptr);
            bob->append("indexVersion", static_cast<int>(ipn->index.version));
            break;
        }
        default:
            break;
    }

    // Display the BSON representation of the filter, if there is one.
    if (node->filter) {
        bob->append("filter", node->filter->serialize());
    }

    // Append the cost and cardinality estimates for the stage if they exist.
    if (hasCostEstimates) {
        if (isV3) {
            BSONObjBuilder statisticsBob(bob->subobjStart("statistics"));
            appendCostBasedStatsV3(node->getType(), *estimates.at(node), statisticsBob);
        } else {
            estimates.at(node)->serialize(*bob);
        }
    }

    // We're done if there are no children.
    if (node->children.empty()) {
        return;
    }

    const auto appendChild =
        [&](const QuerySolutionNode* child, BSONObjBuilder* childBob, std::uint32_t childDepth) {
            qsnToBson(child, format, explainPolicy, estimates, childBob, topLevelBob, childDepth);
        };

    // V3 always emits 'inputStages' array even for a single child, while legacy format collapses a
    // single child into 'inputStage'.
    if (!isV3 && node->children.size() == 1) {
        BSONObjBuilder childBob(bob->subobjStart("inputStage"));
        appendChild(node->children[0].get(), &childBob, currentDepth + 1);
        return;
    }

    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
    for (auto&& child : node->children) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        appendChild(child.get(), &childBob, currentDepth + 2);
    }
    childrenBob.doneFast();
}
}  // namespace

void statsToBSON(const QuerySolutionNode* node,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob,
                 const cost_based_ranker::EstimateMap& estimates,
                 std::uint32_t currentDepth) {
    // Legacy format always includes cost-based stats.
    qsnToBson(node,
              PlanStatsFormat::kLegacy,
              ExplainPolicy{ExplainSettings::kCostBasedStats},
              estimates,
              bob,
              topLevelBob,
              currentDepth);
}

void statsToBsonV3(const QuerySolutionNode* node,
                   const ExplainPolicy& explainPolicy,
                   const cost_based_ranker::EstimateMap& estimates,
                   BSONObjBuilder* bob,
                   const BSONObjBuilder* topLevelBob,
                   std::uint32_t currentDepth) {
    qsnToBson(node, PlanStatsFormat::kV3, explainPolicy, estimates, bob, topLevelBob, currentDepth);
}

PlanExplainerSBEBase::PlanExplainerSBEBase(
    const sbe::PlanStage* root,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isCachedPlan,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfo,
    RemoteExplainVector* remoteExplains,
    bool usedJoinOpt,
    cost_based_ranker::EstimateMap estimates,
    std::vector<JoinOptPlan> rejectedPlans,
    boost::optional<PlanSelectionStrategy> planSelectionStrategy,
    boost::optional<size_t> preExtensionWinningPlanHash)
    : PlanExplainer{solution},
      _root{root},
      _rootData{data},
      _estimates{std::move(estimates)},
      _isMultiPlan{isMultiPlan},
      _isFromPlanCache{isCachedPlan},
      _usedJoinOpt{usedJoinOpt},
      _planSelectionStrategy{planSelectionStrategy},
      _cachedPlanHash{cachedPlanHash},
      _preExtensionWinningPlanHash{preExtensionWinningPlanHash},
      _debugInfo{debugInfo},
      _remoteExplains{remoteExplains},
      _rejectedPlansForJoinOpt{std::move(rejectedPlans)} {
    tassert(5968203, "_debugInfo should not be null", _debugInfo);
}

bool PlanExplainerSBEBase::matchesCachedPlan() const {
    // Compare against the pre-extension hash: a pushed-down winner's hash() reflects the extension,
    // but the plan cache stores the pre-extension hash.
    return _cachedPlanHash &&
        (*_cachedPlanHash == _preExtensionWinningPlanHash.value_or(_solution->hash()));
}

std::string PlanExplainerSBEBase::getPlanSummary() const {
    return _debugInfo->planSummary;
}

void PlanExplainerSBEBase::getSummaryStats(PlanSummaryStats* statsOut) const {
    tassert(6466201, "statsOut should be a valid pointer", statsOut);

    // Known at construction and independent of the exec tree, so report them even without a _root.
    statsOut->planSelectionStrategy = _planSelectionStrategy;
    statsOut->usedJoinOptimization = _usedJoinOpt;

    if (!_root) {
        return;
    }

    // If the exec tree _root was provided, so must be _rootData holding auxiliary data.
    tassert(5323806, "exec tree data is not provided", _rootData);

    auto common = _root->getCommonStats();
    statsOut->nReturned = common->advances;
    statsOut->fromMultiPlanner = areThereRejectedPlansToExplain();
    statsOut->fromPlanCache = isFromCache();
    statsOut->totalKeysExamined = 0;
    statsOut->totalDocsExamined = 0;
    statsOut->replanReason = _rootData->replanReason;

    // Collect cumulative execution stats for the plan.
    auto visitor = PlanSummaryStatsVisitor(*statsOut);
    _root->accumulate(kEmptyPlanNodeId, &visitor);

    // Use the pre-computed summary stats instead of traversing the QuerySolution tree.
    const auto& indexesUsed = _debugInfo->mainStats.indexesUsed;
    statsOut->indexesUsed.clear();
    statsOut->indexesUsed.insert(indexesUsed.begin(), indexesUsed.end());
    statsOut->collectionScans = _debugInfo->mainStats.collectionScans;
    statsOut->collectionScansNonTailable = _debugInfo->mainStats.collectionScansNonTailable;
}

void PlanExplainerSBEBase::getSecondarySummaryStats(const NamespaceString& secondaryColl,
                                                    PlanSummaryStats* statsOut) const {
    tassert(6466202, "statsOut should be a valid pointer", statsOut);

    // Use the pre-computed summary stats instead of traversing the QuerySolution tree.
    const auto& entry = _debugInfo->secondaryStats.find(secondaryColl);
    // The secondary collection stats may not be filled in debugInfo if the SBE engine is only
    // responsible for the subtree of the query.
    if (entry != _debugInfo->secondaryStats.end()) {
        const auto& secondaryStats = entry->second;
        const auto& indexesUsed = secondaryStats.indexesUsed;
        statsOut->indexesUsed.insert(indexesUsed.begin(), indexesUsed.end());
        statsOut->collectionScans += secondaryStats.collectionScans;
        statsOut->collectionScansNonTailable += secondaryStats.collectionScansNonTailable;
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerSBEBase::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    tassert(9378606, "encountered unexpected nullptr for root PlanStage", _root);
    auto stats = _root->getStats(true /* includeDebugInfo  */);
    tassert(9378611, "encountered unexpected nullptr for PlanStageStats", stats);

    return buildPlanStatsDetails(_solution,
                                 *stats,
                                 _root,
                                 _rootData,
                                 boost::none /* planSummary */,
                                 boost::none /* queryParams */,
                                 buildRemotePlanInfo(),
                                 verbosity,
                                 matchesCachedPlan(),
                                 false /*printBytecode*/,
                                 _usedJoinOpt,
                                 _estimates,
                                 _preExtensionWinningPlanHash);
}

PlanExplainer::PlanStatsDetails PlanExplainerSBEBase::getWinningPlanStatsQueryPlanner(
    bool printBytecode) const {
    tassert(10629901, "encountered unexpected nullptr for root PlanStage", _root);
    auto stats = _root->getStats(true /* includeDebugInfo  */);
    tassert(10629902, "encountered unexpected nullptr for PlanStageStats", stats);

    return buildPlanStatsDetails(_solution,
                                 *stats,
                                 _root,
                                 _rootData,
                                 boost::none /* planSummary */,
                                 boost::none /* queryParams */,
                                 buildRemotePlanInfo(),
                                 ExplainOptions::Verbosity::kQueryPlanner,
                                 matchesCachedPlan(),
                                 printBytecode,
                                 _usedJoinOpt,
                                 _estimates,
                                 _preExtensionWinningPlanHash);
}

boost::optional<BSONArray> PlanExplainerSBEBase::buildRemotePlanInfo() const {
    if (!_remoteExplains) {
        return boost::none;
    }
    BSONArrayBuilder arrBuilder;
    for (const auto& explain : *_remoteExplains) {
        arrBuilder << explain;
    }
    return arrBuilder.arr();
}

PlanExplainerClassicRuntimePlannerForSBE::PlanExplainerClassicRuntimePlannerForSBE(
    const sbe::PlanStage* root,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isCachedPlan,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfo,
    std::unique_ptr<PlanStage> classicRuntimePlannerStage,
    RemoteExplainVector* remoteExplains,
    bool usedJoinOpt,
    cost_based_ranker::EstimateMap estimates,
    std::vector<JoinOptPlan> rejectedPlans,
    boost::optional<PlanExplainerData> maybeExplainData,
    boost::optional<PlanSelectionStrategy> planSelectionStrategy)
    : PlanExplainerSBEBase{root,
                           data,
                           solution,
                           isMultiPlan,
                           isCachedPlan,
                           cachedPlanHash,
                           std::move(debugInfo),
                           remoteExplains,
                           usedJoinOpt,
                           std::move(estimates),
                           std::move(rejectedPlans),
                           planSelectionStrategy,
                           maybeExplainData ? maybeExplainData->preExtensionWinningPlanHash
                                            : boost::none},
      _classicRuntimePlannerStage{std::move(classicRuntimePlannerStage)},
      // TODO SERVER-129170: Refactor to avoid copying on the explain path.
      _ceSamplingMetadata{maybeExplainData
                              ? boost::make_optional(maybeExplainData->ceSamplingMetadata)
                              : boost::none},
      _fieldStatsMetadata{maybeExplainData && !maybeExplainData->fieldStatsMetadata.empty()
                              ? boost::make_optional(maybeExplainData->fieldStatsMetadata)
                              : boost::none},
      _joinPlanCacheKeyHash{maybeExplainData ? maybeExplainData->joinPlanCacheKeyHash
                                             : boost::none},
      _planRankerReason{maybeExplainData ? maybeExplainData->planRankerReason : boost::none},
      _classicRuntimePlannerExplainer{
          _classicRuntimePlannerStage  // If there were no multi-planning, this will be nullptr.
              ? plan_explainer_factory::make(_classicRuntimePlannerStage.get(),
                                             cachedPlanHash,
                                             boost::none /* replanReason */,
                                             std::move(maybeExplainData),
                                             // No construction-time snapshot of the trial
                                             // statistics is needed on this path, not even for the
                                             // V3 per-plan view.
                                             false /* isExplain */,
                                             planSelectionStrategy)
              : nullptr} {
    if (_classicRuntimePlannerExplainer) {
        // 'solution' is always non-null when 'classicRuntimePlannerStage' is non-null
        // (MultiPlanner::makeExecutor() invariant).
        tassert(11619100,
                "Expected non-null QuerySolution when classic runtime planner explainer exists",
                _solution);
        _classicRuntimePlannerExplainer->updateEnumeratorExplainInfo(
            _solution->_enumeratorExplainInfo);
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerClassicRuntimePlannerForSBE::getWinningPlanTrialStats()
    const {
    return _classicRuntimePlannerExplainer
        ? _classicRuntimePlannerExplainer->getWinningPlanTrialStats()
        : PlanExplainer::PlanStatsDetails{};
}

std::vector<PlanExplainer::PlanStatsDetails>
PlanExplainerClassicRuntimePlannerForSBE::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    if (_usedJoinOpt) {
        std::vector<PlanExplainer::PlanStatsDetails> out;
        for (auto&& soln : _rejectedPlansForJoinOpt) {
            auto stats = soln.stage->getStats(true /* includeDebugInfo  */);
            out.push_back(buildPlanStatsDetails(soln.soln.get(),
                                                *stats,
                                                soln.stage.get(),
                                                &soln.data,
                                                boost::none /* planSummary */,
                                                boost::none /* queryParams */,
                                                boost::none /* remotePlanInfo */,
                                                verbosity,
                                                false /* matchesCachedPlan */,
                                                false /* printBytecode */,
                                                true /* usedJoinOpt */,
                                                _estimates));
        }
        return out;
    }
    return _classicRuntimePlannerExplainer
        ? _classicRuntimePlannerExplainer->getRejectedPlansStats(verbosity)
        : std::vector<PlanExplainer::PlanStatsDetails>{};
}

ExplainPlanEntry PlanExplainerClassicRuntimePlannerForSBE::makeQsnPlanEntry(
    const QuerySolution& solution,
    const ExplainPolicy& policy,
    const cost_based_ranker::EstimateMap& estimates) const {
    ExplainPlanEntry entry;
    entry.solutionHash = solution.hash();
    entry.isCached = _cachedPlanHash && (*_cachedPlanHash == *entry.solutionHash);
    BSONObjBuilder bob;
    // Append the stats of the portion of the QSN which was ranked.
    statsToBsonV3(rankedRootOf(solution), policy, estimates, &bob, &bob);
    entry.planStatsTree = bob.obj();
    return entry;
}

void PlanExplainerClassicRuntimePlannerForSBE::attachWinnerSbeInfo(
    const ExplainPolicy& policy, std::vector<ExplainPlanEntry>& entries) const {
    if (entries.empty()) {
        return;
    }
    auto& winner = entries.front();

    // The tree that ran, when the pushed-down pipeline made it differ from the ranked tree that
    // 'planStatsTree' holds.
    if (_solution && rankedRootOf(*_solution) != _solution->root()) {
        BSONObjBuilder bob;
        statsToBsonV3(_solution->root(), policy, _estimates, &bob, &bob);
        winner.executedPlanStages = bob.obj();
    }

    if (_root && _rootData) {
        // Include the already allocated tree sizes in the budget and return a warning if the SBE
        // plan causes the explain to exceed the size limit.
        const int lengthCap = internalQueryExplainSizeThresholdBytes.loadRelaxed() -
            winner.planStatsTree.objsize() -
            (winner.executedPlanStages ? winner.executedPlanStages->objsize() : 0);
        if (auto debugInfo = buildSbeDebugInfoWithinBudget(
                _root, _rootData, lengthCap, policy.hasByteCode() /*printBytecode*/)) {
            winner.slotBasedPlan = std::move(*debugInfo);
        } else {
            winner.warning = "slotBasedPlan exceeded BSON size limit for explain";
        }
    }
}

std::vector<ExplainPlanEntry> PlanExplainerClassicRuntimePlannerForSBE::getPlanEntries(
    const ExplainPolicy& policy,
    PlanStatsFormat format,
    PlanSelectionStrategy decidingPlanRanker) const {
    tassert(13314403,
            "SBE explainers produce only V3-format plan entries",
            format == PlanStatsFormat::kV3);

    // With no runtime planner stage there are no trial trees to read, so the plan is described by
    // its QuerySolution alone: one entry, carrying the cost-based ranker's estimates but no trial
    // counters, since the plan never ran a trial.
    if (!_classicRuntimePlannerExplainer) {
        // Without a QuerySolution there is nothing to enumerate.
        if (!_solution) {
            return {};
        }
        std::vector<ExplainPlanEntry> entries;
        entries.push_back(makeQsnPlanEntry(*_solution, policy, _estimates));
        attachWinnerSbeInfo(policy, entries);
        return entries;
    }

    auto entries =
        _classicRuntimePlannerExplainer->getPlanEntries(policy, format, decidingPlanRanker);
    if (entries.empty()) {
        return entries;
    }

    // The embedded classic explainer describes the trial trees only; it was built from the runtime
    // planner stage and has no QuerySolution of its own, so the winner's identity - and hence
    // whether the winner is the cached plan - is supplied here.
    auto& winner = entries.front();
    if (_solution) {
        winner.solutionHash = _solution->hash();
        winner.isCached = matchesCachedPlan();
    }
    attachWinnerSbeInfo(policy, entries);
    return entries;
}

boost::optional<StringMap<cost_based_ranker::SamplingMetadata>>
PlanExplainerClassicRuntimePlannerForSBE::getCeSamplingMetadata() const {
    return _ceSamplingMetadata;
}

boost::optional<StringMap<std::vector<ce::PersistedNDVEntry>>>
PlanExplainerClassicRuntimePlannerForSBE::getFieldStatsMetadata() const {
    return _fieldStatsMetadata;
}

boost::optional<uint32_t> PlanExplainerClassicRuntimePlannerForSBE::getJoinPlanCacheKeyHash()
    const {
    return _joinPlanCacheKeyHash;
}
}  // namespace mongo
