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

#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/projection_ast_util.h"

namespace mongo {
namespace {
void statsToBSON(const QuerySolutionNode* node,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob) {
    invariant(bob);
    invariant(topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > kMaxExplainStatsBSONSizeMB) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    bob->append("stage", stageTypeToString(node->getType()));
    bob->appendNumber("planNodeId", static_cast<size_t>(node->nodeId()));

    // Display the BSON representation of the filter, if there is one.
    if (node->filter) {
        bob->append("filter", node->filter->serialize());
    }

    // Stage-specific stats.
    switch (node->getType()) {
        case STAGE_COLLSCAN: {
            auto csn = static_cast<const CollectionScanNode*>(node);
            bob->append("direction", csn->direction > 0 ? "forward" : "backward");
            if (csn->minTs) {
                bob->append("minTs", *csn->minTs);
            }
            if (csn->maxTs) {
                bob->append("maxTs", *csn->maxTs);
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

            auto bounds = ixn->bounds.toBSON();
            if (topLevelBob->len() + bounds.objsize() > kMaxExplainStatsBSONSizeMB) {
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
            bob->appendIntOrLL("memLimit", sn->maxMemoryUsageBytes);

            if (sn->limit > 0) {
                bob->appendIntOrLL("limitAmount", sn->limit);
            }

            bob->append("type", node->getType() == STAGE_SORT_SIMPLE ? "simple" : "default");
            break;
        }
        case STAGE_SORT_MERGE: {
            auto smn = static_cast<const MergeSortNode*>(node);
            bob->append("sortPattern", smn->sort);
            break;
        }
        case STAGE_TEXT: {
            auto tn = static_cast<const TextNode*>(node);

            bob->append("indexPrefix", tn->indexPrefix);
            bob->append("indexName", tn->index.identifier.catalogName);
            auto ftsQuery = dynamic_cast<fts::FTSQueryImpl*>(tn->ftsQuery.get());
            invariant(ftsQuery);
            bob->append("parsedTextQuery", ftsQuery->toBSON());
            bob->append("textIndexVersion", tn->index.version);
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
        BSONObjBuilder childBob;
        statsToBSON(node->children[0], &childBob, topLevelBob);
        bob->append("inputStage", childBob.obj());
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.
    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
    for (auto&& child : node->children) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSON(child, &childBob, topLevelBob);
    }
    childrenBob.doneFast();
}

void statsToBSON(const sbe::PlanStageStats* stats,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob) {
    invariant(stats);
    invariant(bob);
    invariant(topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > kMaxExplainStatsBSONSizeMB) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    auto stageType = stats->common.stageType;
    bob->append("stage", stageType);
    bob->appendNumber("planNodeId", static_cast<size_t>(stats->common.nodeId));

    // Some top-level exec stats get pulled out of the root stage.
    bob->appendNumber("nReturned", stats->common.advances);
    // Include executionTimeMillis if it was recorded.
    if (stats->common.executionTimeMillis) {
        bob->appendNumber("executionTimeMillisEstimate", *stats->common.executionTimeMillis);
    }
    bob->appendNumber("advances", stats->common.advances);
    bob->appendNumber("opens", stats->common.opens);
    bob->appendNumber("closes", stats->common.closes);
    bob->appendNumber("saveState", stats->common.yields);
    bob->appendNumber("restoreState", stats->common.unyields);
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
        BSONObjBuilder childBob;
        statsToBSON(stats->children[0].get(), &childBob, topLevelBob);
        bob->append("inputStage"_sd, childBob.obj());
        return;
    }

    // For some stages we may want to output its children under different field names.
    auto overridenNames = [stageType]() -> std::vector<StringData> {
        if (stageType == "branch"_sd) {
            return {"thenStage"_sd, "elseStage"_sd};
        } else if (stageType == "nlj"_sd || stageType == "traverse"_sd) {
            return {"outerStage"_sd, "innerStage"_sd};
        }
        return {};
    }();
    if (!overridenNames.empty()) {
        invariant(overridenNames.size() == stats->children.size());

        for (size_t idx = 0; idx < stats->children.size(); ++idx) {
            BSONObjBuilder childBob;
            statsToBSON(stats->children[idx].get(), &childBob, topLevelBob);
            bob->append(overridenNames[idx], childBob.obj());
        }
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.
    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"_sd));
    for (auto&& child : stats->children) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSON(child.get(), &childBob, topLevelBob);
    }
    childrenBob.doneFast();
}

PlanSummaryStats collectExecutionStatsSummary(const sbe::PlanStageStats* stats) {
    invariant(stats);

    PlanSummaryStats summary;
    summary.nReturned = stats->common.advances;

    if (stats->common.executionTimeMillis) {
        summary.executionTimeMillisEstimate = *stats->common.executionTimeMillis;
    }

    // Collect cumulative execution stats for the plan.
    std::queue<const sbe::PlanStageStats*> queue;
    queue.push(stats);
    while (!queue.empty()) {
        stats = queue.front();
        invariant(stats);
        queue.pop();

        if (stats->specific) {
            stats->specific->accumulate(summary);
        }

        for (auto&& child : stats->children) {
            queue.push(child.get());
        }
    }

    return summary;
}

PlanExplainer::PlanStatsDetails buildPlanStatsDetails(
    const QuerySolutionNode* node,
    const sbe::PlanStageStats* stats,
    const boost::optional<BSONObj>& execPlanDebugInfo,
    ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder bob;

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        auto summary = collectExecutionStatsSummary(stats);
        statsToBSON(stats, &bob, &bob);
        // At the 'kQueryPlanner' verbosity level we use the QSN-derived format for the given plan,
        // and thus the winning plan and rejected plans at this verbosity should display the
        // stringified SBE plan, which is added below. However, at the 'kExecStats' the execution
        // stats use the PlanStage-derived format for the SBE tree, so there is no need to repeat
        // the stringified SBE plan and we only included what's been generated from the
        // PlanStageStats.
        return {bob.obj(), std::move(summary)};
    }

    statsToBSON(node, &bob, &bob);
    invariant(execPlanDebugInfo);
    return {BSON("queryPlan" << bob.obj() << "slotBasedPlan" << *execPlanDebugInfo), boost::none};
}
}  // namespace

const PlanExplainer::ExplainVersion& PlanExplainerSBE::getVersion() const {
    static const ExplainVersion kExplainVersion = "2";
    return kExplainVersion;
}

std::string PlanExplainerSBE::getPlanSummary() const {
    if (!_solution) {
        return {};
    }

    StringBuilder sb;
    std::queue<const QuerySolutionNode*> queue;
    queue.push(_solution->root());

    while (!queue.empty()) {
        auto node = queue.front();
        queue.pop();

        sb << stageTypeToString(node->getType());

        switch (node->getType()) {
            case STAGE_COUNT_SCAN: {
                auto csn = static_cast<const CountScanNode*>(node);
                const KeyPattern keyPattern{csn->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_DISTINCT_SCAN: {
                auto dn = static_cast<const DistinctNode*>(node);
                const KeyPattern keyPattern{dn->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_GEO_NEAR_2D: {
                auto geo2d = static_cast<const GeoNear2DNode*>(node);
                const KeyPattern keyPattern{geo2d->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_GEO_NEAR_2DSPHERE: {
                auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
                const KeyPattern keyPattern{geo2dsphere->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_IXSCAN: {
                auto ixn = static_cast<const IndexScanNode*>(node);
                const KeyPattern keyPattern{ixn->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_TEXT: {
                auto tn = static_cast<const TextNode*>(node);
                const KeyPattern keyPattern{tn->indexPrefix};
                sb << " " << keyPattern;
                break;
            }
            default:
                break;
        }

        for (auto&& child : node->children) {
            queue.push(child);
        }

        if (!queue.empty()) {
            sb << ", ";
        }
    }

    return sb.str();
}

void PlanExplainerSBE::getSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);

    if (!_solution || !_root) {
        return;
    }

    auto common = _root->getCommonStats();
    statsOut->nReturned = common->advances;
    statsOut->fromMultiPlanner = isMultiPlan();
    statsOut->totalKeysExamined = 0;
    statsOut->totalDocsExamined = 0;

    // Collect cumulative execution stats for the plan.
    _root->accumulate(kEmptyPlanNodeId, *statsOut);

    std::queue<const QuerySolutionNode*> queue;
    queue.push(_solution->root());

    // Look through the QuerySolution to collect some static stat details.
    //
    // TODO SERVER-51138: handle replan reason for cached plan.
    while (!queue.empty()) {
        auto node = queue.front();
        queue.pop();
        invariant(node);

        switch (node->getType()) {
            case STAGE_COUNT_SCAN: {
                auto csn = static_cast<const CountScanNode*>(node);
                statsOut->indexesUsed.insert(csn->index.identifier.catalogName);
                break;
            }
            case STAGE_DISTINCT_SCAN: {
                auto dn = static_cast<const DistinctNode*>(node);
                statsOut->indexesUsed.insert(dn->index.identifier.catalogName);
                break;
            }
            case STAGE_GEO_NEAR_2D: {
                auto geo2d = static_cast<const GeoNear2DNode*>(node);
                statsOut->indexesUsed.insert(geo2d->index.identifier.catalogName);
                break;
            }
            case STAGE_GEO_NEAR_2DSPHERE: {
                auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
                statsOut->indexesUsed.insert(geo2dsphere->index.identifier.catalogName);
                break;
            }
            case STAGE_IXSCAN: {
                auto ixn = static_cast<const IndexScanNode*>(node);
                statsOut->indexesUsed.insert(ixn->index.identifier.catalogName);
                break;
            }
            case STAGE_TEXT: {
                auto tn = static_cast<const TextNode*>(node);
                statsOut->indexesUsed.insert(tn->index.identifier.catalogName);
                break;
            }
            case STAGE_COLLSCAN: {
                statsOut->collectionScans++;
                auto csn = static_cast<const CollectionScanNode*>(node);
                if (!csn->tailable) {
                    statsOut->collectionScansNonTailable++;
                }
            }
            default:
                break;
        }

        for (auto&& child : node->children) {
            queue.push(child);
        }
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerSBE::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    invariant(_root);
    invariant(_solution);
    auto stats = _root->getStats(true /* includeDebugInfo  */);
    return buildPlanStatsDetails(_solution->root(), stats.get(), _execPlanDebugInfo, verbosity);
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
        auto execPlanDebugInfo = buildExecPlanDebugInfo(candidate.root.get(), &candidate.data);
        res.push_back(buildPlanStatsDetails(
            candidate.solution->root(), stats.get(), execPlanDebugInfo, verbosity));
    }
    return res;
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerSBE::getCachedPlanStats(
    const PlanCacheEntry::DebugInfo& debugInfo, ExplainOptions::Verbosity verbosity) const {
    const auto& decision = *debugInfo.decision;
    std::vector<PlanStatsDetails> res;

    auto&& stats = decision.getStats<mongo::sbe::PlanStageStats>();
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        for (auto&& planStats : stats.candidatePlanStats) {
            res.push_back(buildPlanStatsDetails(nullptr, planStats.get(), boost::none, verbosity));
        }
    } else {
        // At the "queryPlanner" verbosity we only need to provide details about the winning plan
        // when explaining from the plan cache.
        invariant(verbosity == ExplainOptions::Verbosity::kQueryPlanner);
        res.push_back({stats.serializedWinningPlan, boost::none});
    }

    return res;
}
}  // namespace mongo
