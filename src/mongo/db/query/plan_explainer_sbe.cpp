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

#include "mongo/db/query/plan_explainer_sbe.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"

#include <cstddef>
#include <cstdint>
#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
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
    invariant(replace.nFields() == fieldNames.nFields());

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
    if (stats->common.executionTime.precision == QueryExecTimerPrecision::kMillis) {
        bob->appendNumber(
            "executionTimeMillisEstimate",
            durationCount<Milliseconds>(stats->common.executionTime.executionTimeEstimate));
    } else if (stats->common.executionTime.precision == QueryExecTimerPrecision::kNanos) {
        bob->appendNumber(
            "executionTimeMillisEstimate",
            durationCount<Milliseconds>(stats->common.executionTime.executionTimeEstimate));
        bob->appendNumber(
            "executionTimeMicros",
            durationCount<Microseconds>(stats->common.executionTime.executionTimeEstimate));
        bob->appendNumber(
            "executionTimeNanos",
            durationCount<Nanoseconds>(stats->common.executionTime.executionTimeEstimate));
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
    const sbe::PlanStage* sbePlanStageRoot,
    const stage_builder::PlanStageData* sbePlanStageData,
    const boost::optional<std::string>& planSummary,
    const boost::optional<BSONObj>& queryParams,
    const boost::optional<BSONArray>& remotePlanInfo,
    ExplainOptions::Verbosity verbosity,
    bool isCached) {
    BSONObjBuilder bob;

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        auto summary = sbe::collectExecutionStatsSummary(stats);
        if (solution != nullptr && verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
            summary.score = solution->score;
            if (internalQueryAllowForcedPlanByHash.load()) {
                bob.append("solutionHashUnstable", (long long)solution->hash());
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
        statsToBSON(solution->root(), &bob, &bob);
        if (internalQueryAllowForcedPlanByHash.load()) {
            bob.append("solutionHashUnstable", (long long)solution->hash());
        }
    }

    BSONObjBuilder plan;
    if (planSummary) {
        plan.append("planSummary", *planSummary);
    }

    plan.append("isCached", isCached);
    plan.append("queryPlan", bob.obj());

    int explainThresholdBytes = internalQueryExplainSizeThresholdBytes.loadRelaxed();

    if (plan.len() > explainThresholdBytes) {
        plan.append("warning", "slotBasedPlan exceeded BSON size limit for explain");
    } else {
        BSONObj execPlanDebugInfo = PlanExplainerSBEBase::buildExecPlanDebugInfo(
            sbePlanStageRoot, sbePlanStageData, explainThresholdBytes - plan.len() /*lengthCap*/
        );
        if (plan.len() + execPlanDebugInfo.objsize() > explainThresholdBytes) {
            plan.append("warning", "slotBasedPlan exceeded BSON size limit for explain");
        } else {
            plan.append("slotBasedPlan", execPlanDebugInfo);
        }
    }
    if (remotePlanInfo && !remotePlanInfo->isEmpty()) {
        plan.append("remotePlans", *remotePlanInfo);
    }
    return {plan.obj(), boost::none};
}
}  // namespace

void statsToBSON(const QuerySolutionNode* node,
                 BSONObjBuilder* bob,
                 const BSONObjBuilder* topLevelBob) {
    tassert(9378604, "encountered unexpected nullptr for BSONObjBuilder", bob);
    tassert(9378605, "encountered unexpected nullptr for BSONObjBuilder", topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > internalQueryExplainSizeThresholdBytes.load()) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    bob->append("stage", nodeStageTypeToString(node));
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
        case STAGE_COUNT_SCAN: {
            auto csn = static_cast<const CountScanNode*>(node);

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

            bob->append("foreignCollection",
                        NamespaceStringUtil::serialize(eln->foreignCollection,
                                                       SerializationContext::stateDefault()));
            bob->append("localField", eln->joinFieldLocal.fullPath());
            bob->append("foreignField", eln->joinFieldForeign.fullPath());
            bob->append("asField", eln->joinField.fullPath());
            bob->append("strategy", EqLookupNode::serializeLookupStrategy(eln->lookupStrategy));
            if (eln->idxEntry) {
                bob->append("indexName", eln->idxEntry->identifier.catalogName);
                bob->append("indexKeyPattern", eln->idxEntry->keyPattern);
            }
            bob->append("scanDirection", toString(eln->scanDirection));
            break;
        }
        case STAGE_EQ_LOOKUP_UNWIND: {
            auto eln = static_cast<const EqLookupUnwindNode*>(node);

            bob->append("foreignCollection",
                        NamespaceStringUtil::serialize(eln->foreignCollection,
                                                       SerializationContext::stateDefault()));
            bob->append("localField", eln->joinFieldLocal.fullPath());
            bob->append("foreignField", eln->joinFieldForeign.fullPath());
            bob->append("asField", eln->joinField.fullPath());
            bob->append("strategy", EqLookupNode::serializeLookupStrategy(eln->lookupStrategy));
            if (eln->idxEntry) {
                bob->append("indexName", eln->idxEntry->identifier.catalogName);
                bob->append("indexKeyPattern", eln->idxEntry->keyPattern);
            }
            bob->append("scanDirection", toString(eln->scanDirection));
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

PlanExplainerSBEBase::PlanExplainerSBEBase(
    const sbe::PlanStage* root,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isCachedPlan,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfo,
    RemoteExplainVector* remoteExplains)
    : PlanExplainer{solution},
      _root{root},
      _rootData{data},
      _isMultiPlan{isMultiPlan},
      _isFromPlanCache{isCachedPlan},
      _cachedPlanHash{cachedPlanHash},
      _debugInfo{debugInfo},
      _remoteExplains{remoteExplains} {
    tassert(5968203, "_debugInfo should not be null", _debugInfo);
};

const PlanExplainer::ExplainVersion& PlanExplainerSBEBase::getVersion() const {
    static const ExplainVersion kExplainVersionForStageBuilders = "2";
    return kExplainVersionForStageBuilders;
}

bool PlanExplainerSBEBase::matchesCachedPlan() const {
    return _cachedPlanHash && (*_cachedPlanHash == _solution->hash());
};

std::string PlanExplainerSBEBase::getPlanSummary() const {
    return _debugInfo->planSummary;
}

void PlanExplainerSBEBase::getSummaryStats(PlanSummaryStats* statsOut) const {
    tassert(6466201, "statsOut should be a valid pointer", statsOut);

    if (!_root) {
        return;
    }

    // If the exec tree _root was provided, so must be _rootData holding auxiliary data.
    tassert(5323806, "exec tree data is not provided", _rootData);

    auto common = _root->getCommonStats();
    statsOut->nReturned = common->advances;
    statsOut->fromMultiPlanner = isMultiPlan();
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
                                 matchesCachedPlan());
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
    RemoteExplainVector* remoteExplains)
    : PlanExplainerSBEBase{root,
                           data,
                           solution,
                           isMultiPlan,
                           isCachedPlan,
                           cachedPlanHash,
                           std::move(debugInfo),
                           remoteExplains},
      _classicRuntimePlannerStage{std::move(classicRuntimePlannerStage)},
      _classicRuntimePlannerExplainer{
          _classicRuntimePlannerStage  // If there were no multi-planning, this will be nullptr.
              ? plan_explainer_factory::make(_classicRuntimePlannerStage.get(), cachedPlanHash)
              : nullptr} {
    if (_classicRuntimePlannerExplainer) {
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
    return _classicRuntimePlannerExplainer
        ? _classicRuntimePlannerExplainer->getRejectedPlansStats(verbosity)
        : std::vector<PlanExplainer::PlanStatsDetails>{};
}
}  // namespace mongo
