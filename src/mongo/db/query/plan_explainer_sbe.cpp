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

#include "mongo/db/query/plan_explainer_sbe.h"

#include <queue>

#include "mongo/db/exec/plan_stats_walker.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/record_id_helpers.h"

namespace mongo {
namespace {
void statsToBSON(const QuerySolutionNode* node,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob) {
    invariant(bob);
    invariant(topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > internalQueryExplainSizeThresholdBytes.load()) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    bob->append("stage", stageTypeToString(node->getType()));
    bob->appendNumber("planNodeId", static_cast<long long>(node->nodeId()));

    // Display the BSON representation of the filter, if there is one.
    if (node->filter) {
        bob->append("filter", node->filter->serialize());
    }

    // Stage-specific stats.
    switch (node->getType()) {
        case STAGE_COLLSCAN: {
            auto csn = static_cast<const CollectionScanNode*>(node);
            bob->append("direction", csn->direction > 0 ? "forward" : "backward");
            if (csn->minRecord) {
                csn->minRecord->appendToBSONAs(bob, "minRecord");
            }
            if (csn->maxRecord) {
                csn->maxRecord->appendToBSONAs(bob, "maxRecord");
            }
            break;
        }
        case STAGE_GEO_NEAR_2D: {
            auto geo2d = static_cast<const GeoNear2DNode*>(node);
            bob->append("keyPattern", geo2d->index.keyPattern);
            bob->append("indexName", geo2d->index.identifier.catalogName);
            bob->append("indexVersion", geo2d->index.version);
            break;
        }
        case STAGE_GEO_NEAR_2DSPHERE: {
            auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
            bob->append("keyPattern", geo2dsphere->index.keyPattern);
            bob->append("indexName", geo2dsphere->index.identifier.catalogName);
            bob->append("indexVersion", geo2dsphere->index.version);
            break;
        }
        case STAGE_IXSCAN: {
            auto ixn = static_cast<const IndexScanNode*>(node);

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

            bob->append("indexPrefix", tn->indexPrefix);
            bob->append("indexName", tn->index.identifier.catalogName);
            auto ftsQuery = dynamic_cast<fts::FTSQueryImpl*>(tn->ftsQuery.get());
            invariant(ftsQuery);
            bob->append("parsedTextQuery", ftsQuery->toBSON());
            bob->append("textIndexVersion", tn->index.version);
            break;
        }
        case STAGE_EQ_LOOKUP: {
            auto eln = static_cast<const EqLookupNode*>(node);

            bob->append("foreignCollection", eln->foreignCollection.toString());
            bob->append("localField", eln->joinFieldLocal.fullPath());
            bob->append("foreignField", eln->joinFieldForeign.fullPath());
            bob->append("asField", eln->joinField.fullPath());
            bob->append("strategy", EqLookupNode::serializeLookupStrategy(eln->lookupStrategy));
            if (eln->idxEntry) {
                bob->append("indexName", eln->idxEntry->identifier.catalogName);
                bob->append("indexKeyPattern", eln->idxEntry->keyPattern);
            }
            break;
        }
        default:
            break;
    }

    // We're done if there are no children.
    if (node->children.empty()) {
        return;
    }

    // If there's just one child (a common scenario), avoid making an array. This makes
    // the output more readable by saving a level of nesting. Name the field 'inputStage'
    // rather than 'inputStages'.
    if (node->children.size() == 1) {
        BSONObjBuilder childBob(bob->subobjStart("inputStage"));
        statsToBSON(node->children[0].get(), &childBob, topLevelBob);
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.
    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
    for (auto&& child : node->children) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSON(child.get(), &childBob, topLevelBob);
    }
    childrenBob.doneFast();
}
void statsToBSONHelper(const sbe::PlanStageStats* stats,
                       BSONObjBuilder* bob,
                       const BSONObjBuilder* topLevelBob,
                       std::uint32_t currentDepth) {
    invariant(stats);
    invariant(bob);
    invariant(topLevelBob);

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
    // Include executionTimeMillis if it was recorded.
    if (stats->common.executionTimeMillis) {
        bob->appendNumber("executionTimeMillisEstimate", *stats->common.executionTimeMillis);
    }
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
    auto overridenNames = [stageType]() -> std::vector<StringData> {
        if (stageType == "branch"_sd) {
            return {"thenStage"_sd, "elseStage"_sd};
        } else if (stageType == "nlj"_sd || stageType == "traverse"_sd || stageType == "mj"_sd ||
                   stageType == "hj"_sd) {
            return {"outerStage"_sd, "innerStage"_sd};
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
    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"_sd));
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

PlanExplainer::PlanStatsDetails buildPlanStatsDetails(
    const QuerySolution* solution,
    const sbe::PlanStageStats& stats,
    const boost::optional<BSONObj>& execPlanDebugInfo,
    const boost::optional<BSONObj>& optimizerExplain,
    ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder bob;

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        auto summary = sbe::collectExecutionStatsSummary(stats);
        if (solution != nullptr && verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
            summary.score = solution->score;
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
        statsToBSON(solution->root(), &bob, &bob);
    }

    invariant(execPlanDebugInfo);
    if (optimizerExplain) {
        return {BSON("optimizerPlan" << *optimizerExplain << "slotBasedPlan" << *execPlanDebugInfo),
                boost::none};
    } else {
        return {BSON("queryPlan" << bob.obj() << "slotBasedPlan" << *execPlanDebugInfo),
                boost::none};
    }
}
}  // namespace

const PlanExplainer::ExplainVersion& PlanExplainerSBE::getVersion() const {
    static const ExplainVersion kExplainVersion = "2";
    return kExplainVersion;
}

std::string PlanExplainerSBE::getPlanSummary() const {
    return _debugInfo->planSummary;
}

void PlanExplainerSBE::getSummaryStats(PlanSummaryStats* statsOut) const {
    tassert(6466201, "statsOut should be a valid pointer", statsOut);

    if (!_root) {
        return;
    }

    // If the exec tree _root was provided, so must be _rootData holding auxiliary data.
    tassert(5323806, "exec tree data is not provided", _rootData);

    auto common = _root->getCommonStats();
    statsOut->nReturned = common->advances;
    statsOut->fromMultiPlanner = isMultiPlan();
    statsOut->totalKeysExamined = 0;
    statsOut->totalDocsExamined = 0;
    statsOut->replanReason = _rootData->replanReason;

    // Collect cumulative execution stats for the plan.
    auto visitor = PlanSummaryStatsVisitor(*statsOut);
    _root->accumulate(kEmptyPlanNodeId, &visitor);

    // Use the pre-computed summary stats instead of traversing the QuerySolution tree.
    const auto& indexesUsed = _debugInfo->mainStats.indexesUsed;
    statsOut->indexesUsed.insert(indexesUsed.begin(), indexesUsed.end());
    statsOut->collectionScans += _debugInfo->mainStats.collectionScans;
    statsOut->collectionScansNonTailable += _debugInfo->mainStats.collectionScansNonTailable;
}

void PlanExplainerSBE::getSecondarySummaryStats(std::string secondaryColl,
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

PlanExplainer::PlanStatsDetails PlanExplainerSBE::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    invariant(_root);
    auto stats = _root->getStats(true /* includeDebugInfo  */);
    invariant(stats);
    return buildPlanStatsDetails(_solution,
                                 *stats,
                                 buildExecPlanDebugInfo(_root, _rootData),
                                 buildCascadesPlan(),
                                 verbosity);
}

PlanExplainer::PlanStatsDetails PlanExplainerSBE::getWinningPlanTrialStats() const {
    invariant(_rootData);
    if (_rootData->savedStatsOnEarlyExit) {
        invariant(_solution);
        return buildPlanStatsDetails(
            _solution,
            *_rootData->savedStatsOnEarlyExit,
            // This parameter is not used in `buildPlanStatsDetails` if the last parameter is
            // `ExplainOptions::Verbosity::kExecAllPlans`, as is the case here.
            boost::none,
            boost::none,
            ExplainOptions::Verbosity::kExecAllPlans);
    }
    return getWinningPlanStats(ExplainOptions::Verbosity::kExecAllPlans);
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerSBE::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    if (_rejectedCandidates.empty()) {
        return {};
    }

    std::vector<PlanStatsDetails> res;
    res.reserve(_rejectedCandidates.size());
    for (auto&& candidate : _rejectedCandidates) {
        invariant(candidate.root);
        invariant(candidate.solution);

        auto stats = candidate.root->getStats(true /* includeDebugInfo  */);
        invariant(stats);
        auto execPlanDebugInfo = buildExecPlanDebugInfo(candidate.root.get(), &candidate.data);
        res.push_back(buildPlanStatsDetails(
            candidate.solution.get(), *stats, execPlanDebugInfo, boost::none, verbosity));
    }
    return res;
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerSBE::getCachedPlanStats(
    const plan_cache_debug_info::DebugInfo& debugInfo, ExplainOptions::Verbosity verbosity) const {
    const auto& decision = *debugInfo.decision;
    std::vector<PlanStatsDetails> res;

    auto&& stats = decision.getStats<mongo::sbe::PlanStageStats>();
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        for (auto&& planStats : stats.candidatePlanStats) {
            invariant(planStats);
            res.push_back(
                buildPlanStatsDetails(nullptr, *planStats, boost::none, boost::none, verbosity));
        }
    } else {
        // At the "queryPlanner" verbosity we only need to provide details about the winning plan
        // when explaining from the plan cache.
        invariant(verbosity == ExplainOptions::Verbosity::kQueryPlanner);
        res.push_back({stats.serializedWinningPlan, boost::none});
    }

    return res;
}

boost::optional<BSONObj> PlanExplainerSBE::buildCascadesPlan() const {
    if (_optimizerData) {
        return _optimizerData->explainBSON();
    }
    return {};
}

}  // namespace mongo
