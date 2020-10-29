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
                 const sbe::PlanStageStats* stats,
                 ExplainOptions::Verbosity verbosity,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob) {
    invariant(bob);
    invariant(topLevelBob);
    invariant(verbosity < ExplainOptions::Verbosity::kExecStats || stats);

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

    // Some top-level exec stats get pulled out of the root stage.
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        // TODO SERVER-51409.
    }

    // Stage-specific stats.
    switch (node->getType()) {
        case STAGE_AND_HASH: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_AND_SORTED: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_COLLSCAN: {
            auto csn = static_cast<const CollectionScanNode*>(node);
            bob->append("direction", csn->direction > 0 ? "forward" : "backward");
            if (csn->minTs) {
                bob->append("minTs", *csn->minTs);
            }
            if (csn->maxTs) {
                bob->append("maxTs", *csn->maxTs);
            }
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }

        case STAGE_ENSURE_SORTED: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_FETCH: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }

        case STAGE_GEO_NEAR_2D: {
            auto geo2d = static_cast<const GeoNear2DNode*>(node);

            bob->append("keyPattern", geo2d->index.keyPattern);
            bob->append("indexName", geo2d->index.identifier.catalogName);
            bob->append("indexVersion", geo2d->index.version);

            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_GEO_NEAR_2DSPHERE: {
            auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);

            bob->append("keyPattern", geo2dsphere->index.keyPattern);
            bob->append("indexName", geo2dsphere->index.identifier.catalogName);
            bob->append("indexVersion", geo2dsphere->index.version);

            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
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

            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_OR: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
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
        case STAGE_SHARDING_FILTER: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_SKIP: {
            auto sn = static_cast<const SkipNode*>(node);
            bob->appendNumber("skipAmount", sn->skip);
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

            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_SORT_MERGE: {
            auto smn = static_cast<const MergeSortNode*>(node);
            bob->append("sortPattern", smn->sort);

            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
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
        }
        case STAGE_TEXT_MATCH: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
            }
            break;
        }
        case STAGE_TEXT_OR: {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                // TODO SERVER-51409.
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
        BSONObjBuilder childBob;
        statsToBSON(node->children[0], stats, verbosity, &childBob, topLevelBob);
        bob->append("inputStage", childBob.obj());
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.
    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
    for (auto&& child : node->children) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSON(child, stats, verbosity, &childBob, topLevelBob);
    }
    childrenBob.doneFast();
}
}  // namespace

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

    auto&& [stats, summary] = [&]()
        -> std::pair<std::unique_ptr<sbe::PlanStageStats>, boost::optional<PlanSummaryStats>> {
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            auto stats = _root->getStats();
            // TODO: SERVER-51409 add support for PlanSummaryStats.
            return {std::move(stats), PlanSummaryStats{}};
        }
        return {};
    }();

    BSONObjBuilder bob;
    statsToBSON(_solution->root(), stats.get(), verbosity, &bob, &bob);
    return {bob.obj(), std::move(summary)};
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

        auto&& [stats, summary] = [&]()
            -> std::pair<std::unique_ptr<sbe::PlanStageStats>, boost::optional<PlanSummaryStats>> {
            if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                auto stats = candidate.root->getStats();
                // TODO: SERVER-51409 add support for PlanSummaryStats.
                return {std::move(stats), PlanSummaryStats{}};
            }
            return {};
        }();

        BSONObjBuilder bob;
        statsToBSON(candidate.solution->root(), stats.get(), verbosity, &bob, &bob);
        res.push_back({bob.obj(), std::move(summary)});
    }
    return res;
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerSBE::getCachedPlanStats(
    const PlanCacheEntry::DebugInfo&, ExplainOptions::Verbosity) const {
    // TODO: SERVER-50728
    return {};
}
}  // namespace mongo
