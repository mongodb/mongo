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

#include "mongo/db/query/plan_explainer_impl.h"

#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count_scan.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/near.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/text_match.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {
/**
 * Adds to the plan summary string being built by 'sb' for the execution stage 'stage'.
 */
void addStageSummaryStr(const PlanStage* stage, StringBuilder& sb) {
    // First add the stage type string.
    const CommonStats* common = stage->getCommonStats();
    sb << common->stageTypeStr;

    // Some leaf nodes also provide info about the index they used.
    const SpecificStats* specific = stage->getSpecificStats();
    if (STAGE_COUNT_SCAN == stage->stageType()) {
        const CountScanStats* spec = static_cast<const CountScanStats*>(specific);
        const KeyPattern keyPattern{spec->keyPattern};
        sb << " " << keyPattern;
    } else if (STAGE_DISTINCT_SCAN == stage->stageType()) {
        const DistinctScanStats* spec = static_cast<const DistinctScanStats*>(specific);
        const KeyPattern keyPattern{spec->keyPattern};
        sb << " " << keyPattern;
    } else if (STAGE_GEO_NEAR_2D == stage->stageType()) {
        const NearStats* spec = static_cast<const NearStats*>(specific);
        const KeyPattern keyPattern{spec->keyPattern};
        sb << " " << keyPattern;
    } else if (STAGE_GEO_NEAR_2DSPHERE == stage->stageType()) {
        const NearStats* spec = static_cast<const NearStats*>(specific);
        const KeyPattern keyPattern{spec->keyPattern};
        sb << " " << keyPattern;
    } else if (STAGE_IXSCAN == stage->stageType()) {
        const IndexScanStats* spec = static_cast<const IndexScanStats*>(specific);
        const KeyPattern keyPattern{spec->keyPattern};
        sb << " " << keyPattern;
    } else if (STAGE_TEXT_MATCH == stage->stageType()) {
        const TextMatchStats* spec = static_cast<const TextMatchStats*>(specific);
        const KeyPattern keyPattern{spec->indexPrefix};
        sb << " " << keyPattern;
    }
}

/**
 * Traverses the tree rooted at 'root', and adds all nodes into the list 'flattened'. If a
 * MultiPlanStage is encountered, only adds the best plan and its children to 'flattened'.
 */
void flattenExecTree(const PlanStage* root, std::vector<const PlanStage*>* flattened) {
    flattened->push_back(root);

    if (root->stageType() == STAGE_MULTI_PLAN) {
        // Only add the winning plan from a MultiPlanStage.
        auto mps = static_cast<const MultiPlanStage*>(root);
        auto bestPlanIdx = mps->bestPlanIdx();
        tassert(3420001, "Trying to explain MultiPlanStage without best plan", bestPlanIdx);
        const auto winningStage = mps->getChildren()[*bestPlanIdx].get();
        flattenExecTree(winningStage, flattened);
        return;
    }

    const auto& children = root->getChildren();
    for (size_t i = 0; i < children.size(); ++i) {
        flattenExecTree(children[i].get(), flattened);
    }
}

/**
 * Traverses the tree rooted at 'root', and adds all tree nodes into the list 'flattened'.
 * If there is a MultiPlanStage node, follows the subplan at 'planIdx'.
 */
void flattenStatsTree(const PlanStageStats* root,
                      const boost::optional<size_t> planIdx,
                      std::vector<const PlanStageStats*>* flattened) {
    if (STAGE_MULTI_PLAN == root->stageType) {
        // Skip the MultiPlanStage, and continue with its planIdx child
        tassert(3420002, "Invalid child plan index", planIdx && planIdx < root->children.size());
        root = root->children[*planIdx].get();
    }
    flattened->push_back(root);
    for (auto&& child : root->children) {
        flattenStatsTree(child.get(), planIdx, flattened);
    }
}

/**
 * Given the SpecificStats object for a stage and the type of the stage, returns the number of index
 * keys examined by the stage.
 */
size_t getKeysExamined(StageType type, const SpecificStats* specific) {
    if (STAGE_IXSCAN == type) {
        const IndexScanStats* spec = static_cast<const IndexScanStats*>(specific);
        return spec->keysExamined;
    } else if (STAGE_IDHACK == type) {
        const IDHackStats* spec = static_cast<const IDHackStats*>(specific);
        return spec->keysExamined;
    } else if (STAGE_COUNT_SCAN == type) {
        const CountScanStats* spec = static_cast<const CountScanStats*>(specific);
        return spec->keysExamined;
    } else if (STAGE_DISTINCT_SCAN == type) {
        const DistinctScanStats* spec = static_cast<const DistinctScanStats*>(specific);
        return spec->keysExamined;
    }

    return 0;
}

/**
 * Given the SpecificStats object for a stage and the type of the stage, returns the number of
 * documents examined by the stage.
 */
size_t getDocsExamined(StageType type, const SpecificStats* specific) {
    if (STAGE_COLLSCAN == type) {
        const CollectionScanStats* spec = static_cast<const CollectionScanStats*>(specific);
        return spec->docsTested;
    } else if (STAGE_FETCH == type) {
        const FetchStats* spec = static_cast<const FetchStats*>(specific);
        return spec->docsExamined;
    } else if (STAGE_IDHACK == type) {
        const IDHackStats* spec = static_cast<const IDHackStats*>(specific);
        return spec->docsExamined;
    } else if (STAGE_TEXT_OR == type) {
        const TextOrStats* spec = static_cast<const TextOrStats*>(specific);
        return spec->fetches;
    }

    return 0;
}

/**
 * Converts the stats tree 'stats' into a corresponding BSON object containing explain information.
 * If there is a MultiPlanStage node, skip that node, and follow the subplan at 'planIdx'.
 *
 * Generates the BSON stats at a verbosity specified by 'verbosity'.
 */
void statsToBSON(const PlanStageStats& stats,
                 ExplainOptions::Verbosity verbosity,
                 const boost::optional<size_t> planIdx,
                 BSONObjBuilder* bob,
                 BSONObjBuilder* topLevelBob) {
    invariant(bob);
    invariant(topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > internalQueryExplainSizeThresholdBytes.load()) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
        return;
    }

    if (STAGE_MULTI_PLAN == stats.stageType) {
        tassert(3420003, "Invalid child plan index", planIdx && planIdx < stats.children.size());
        const PlanStageStats* childStage = stats.children[*planIdx].get();
        statsToBSON(*childStage, verbosity, planIdx, bob, topLevelBob);
        return;
    }

    // Stage name.
    bob->append("stage", stats.common.stageTypeStr);

    // Display the BSON representation of the filter, if there is one.
    if (!stats.common.filter.isEmpty()) {
        bob->append("filter", stats.common.filter);
    }

    // Some top-level exec stats get pulled out of the root stage.
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        bob->appendNumber("nReturned", static_cast<long long>(stats.common.advanced));
        // Include executionTimeMillis if it was recorded.
        if (stats.common.executionTime) {
            bob->appendNumber("executionTimeMillisEstimate",
                              durationCount<Milliseconds>(*stats.common.executionTime));
        }

        bob->appendNumber("works", static_cast<long long>(stats.common.works));
        bob->appendNumber("advanced", static_cast<long long>(stats.common.advanced));
        bob->appendNumber("needTime", static_cast<long long>(stats.common.needTime));
        bob->appendNumber("needYield", static_cast<long long>(stats.common.needYield));
        bob->appendNumber("saveState", static_cast<long long>(stats.common.yields));
        bob->appendNumber("restoreState", static_cast<long long>(stats.common.unyields));
        if (stats.common.failed)
            bob->appendBool("failed", stats.common.failed);
        bob->appendNumber("isEOF", stats.common.isEOF);
    }

    // Stage-specific stats
    if (STAGE_AND_HASH == stats.stageType) {
        AndHashStats* spec = static_cast<AndHashStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("memUsage", static_cast<long long>(spec->memUsage));
            bob->appendNumber("memLimit", static_cast<long long>(spec->memLimit));

            for (size_t i = 0; i < spec->mapAfterChild.size(); ++i) {
                bob->appendNumber(std::string(str::stream() << "mapAfterChild_" << i),
                                  static_cast<long long>(spec->mapAfterChild[i]));
            }
        }
    } else if (STAGE_AND_SORTED == stats.stageType) {
        AndSortedStats* spec = static_cast<AndSortedStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            for (size_t i = 0; i < spec->failedAnd.size(); ++i) {
                bob->appendNumber(std::string(str::stream() << "failedAnd_" << i),
                                  static_cast<long long>(spec->failedAnd[i]));
            }
        }
    } else if (STAGE_COLLSCAN == stats.stageType) {
        CollectionScanStats* spec = static_cast<CollectionScanStats*>(stats.specific.get());
        bob->append("direction", spec->direction > 0 ? "forward" : "backward");
        if (spec->minRecord) {
            spec->minRecord->appendToBSONAs(bob, "minRecord");
        }
        if (spec->maxRecord) {
            spec->maxRecord->appendToBSONAs(bob, "maxRecord");
        }
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsExamined", static_cast<long long>(spec->docsTested));
        }
    } else if (STAGE_COUNT == stats.stageType) {
        CountStats* spec = static_cast<CountStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nCounted", spec->nCounted);
            bob->appendNumber("nSkipped", spec->nSkipped);
        }
    } else if (STAGE_COUNT_SCAN == stats.stageType) {
        CountScanStats* spec = static_cast<CountScanStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("keysExamined", static_cast<long long>(spec->keysExamined));
        }

        bob->append("keyPattern", spec->keyPattern);
        bob->append("indexName", spec->indexName);
        if (!spec->collation.isEmpty()) {
            bob->append("collation", spec->collation);
        }
        bob->appendBool("isMultiKey", spec->isMultiKey);
        if (!spec->multiKeyPaths.empty()) {
            appendMultikeyPaths(spec->keyPattern, spec->multiKeyPaths, bob);
        }
        bob->appendBool("isUnique", spec->isUnique);
        bob->appendBool("isSparse", spec->isSparse);
        bob->appendBool("isPartial", spec->isPartial);
        bob->append("indexVersion", spec->indexVersion);

        BSONObjBuilder indexBoundsBob(bob->subobjStart("indexBounds"));
        indexBoundsBob.append("startKey", spec->startKey);
        indexBoundsBob.append("startKeyInclusive", spec->startKeyInclusive);
        indexBoundsBob.append("endKey", spec->endKey);
        indexBoundsBob.append("endKeyInclusive", spec->endKeyInclusive);
    } else if (STAGE_DELETE == stats.stageType || STAGE_BATCHED_DELETE == stats.stageType) {
        DeleteStats* spec = static_cast<DeleteStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nWouldDelete", static_cast<long long>(spec->docsDeleted));
        }
    } else if (STAGE_DISTINCT_SCAN == stats.stageType) {
        DistinctScanStats* spec = static_cast<DistinctScanStats*>(stats.specific.get());

        bob->append("keyPattern", spec->keyPattern);
        bob->append("indexName", spec->indexName);
        if (!spec->collation.isEmpty()) {
            bob->append("collation", spec->collation);
        }
        bob->appendBool("isMultiKey", spec->isMultiKey);
        if (!spec->multiKeyPaths.empty()) {
            appendMultikeyPaths(spec->keyPattern, spec->multiKeyPaths, bob);
        }
        bob->appendBool("isUnique", spec->isUnique);
        bob->appendBool("isSparse", spec->isSparse);
        bob->appendBool("isPartial", spec->isPartial);
        bob->append("indexVersion", spec->indexVersion);
        bob->append("direction", spec->direction > 0 ? "forward" : "backward");

        if ((topLevelBob->len() + spec->indexBounds.objsize()) >
            internalQueryExplainSizeThresholdBytes.load()) {
            bob->append("warning", "index bounds omitted due to BSON size limit for explain");
        } else {
            bob->append("indexBounds", spec->indexBounds);
        }

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("keysExamined", static_cast<long long>(spec->keysExamined));
        }
    } else if (STAGE_FETCH == stats.stageType) {
        FetchStats* spec = static_cast<FetchStats*>(stats.specific.get());
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsExamined", static_cast<long long>(spec->docsExamined));
            bob->appendNumber("alreadyHasObj", static_cast<long long>(spec->alreadyHasObj));
        }
    } else if (STAGE_GEO_NEAR_2D == stats.stageType || STAGE_GEO_NEAR_2DSPHERE == stats.stageType) {
        NearStats* spec = static_cast<NearStats*>(stats.specific.get());

        bob->append("keyPattern", spec->keyPattern);
        bob->append("indexName", spec->indexName);
        bob->append("indexVersion", spec->indexVersion);

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            BSONArrayBuilder intervalsBob(bob->subarrayStart("searchIntervals"));
            for (std::vector<IntervalStats>::const_iterator it = spec->intervalStats.begin();
                 it != spec->intervalStats.end();
                 ++it) {
                BSONObjBuilder intervalBob(intervalsBob.subobjStart());
                intervalBob.append("minDistance", it->minDistanceAllowed);
                intervalBob.append("maxDistance", it->maxDistanceAllowed);
                intervalBob.append("maxInclusive", it->inclusiveMaxDistanceAllowed);
                intervalBob.appendNumber("nBuffered", it->numResultsBuffered);
                intervalBob.appendNumber("nReturned", it->numResultsReturned);
            }
            intervalsBob.doneFast();
        }
    } else if (STAGE_IDHACK == stats.stageType) {
        IDHackStats* spec = static_cast<IDHackStats*>(stats.specific.get());
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("keysExamined", static_cast<long long>(spec->keysExamined));
            bob->appendNumber("docsExamined", static_cast<long long>(spec->docsExamined));
        }
    } else if (STAGE_IXSCAN == stats.stageType) {
        IndexScanStats* spec = static_cast<IndexScanStats*>(stats.specific.get());

        bob->append("keyPattern", spec->keyPattern);
        bob->append("indexName", spec->indexName);
        if (!spec->collation.isEmpty()) {
            bob->append("collation", spec->collation);
        }
        bob->appendBool("isMultiKey", spec->isMultiKey);
        if (!spec->multiKeyPaths.empty()) {
            appendMultikeyPaths(spec->keyPattern, spec->multiKeyPaths, bob);
        }
        bob->appendBool("isUnique", spec->isUnique);
        bob->appendBool("isSparse", spec->isSparse);
        bob->appendBool("isPartial", spec->isPartial);
        bob->append("indexVersion", spec->indexVersion);
        bob->append("direction", spec->direction > 0 ? "forward" : "backward");

        if ((topLevelBob->len() + spec->indexBounds.objsize()) >
            internalQueryExplainSizeThresholdBytes.load()) {
            bob->append("warning", "index bounds omitted due to BSON size limit for explain");
        } else {
            bob->append("indexBounds", spec->indexBounds);
        }

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("keysExamined", static_cast<long long>(spec->keysExamined));
            bob->appendNumber("seeks", static_cast<long long>(spec->seeks));
            bob->appendNumber("dupsTested", static_cast<long long>(spec->dupsTested));
            bob->appendNumber("dupsDropped", static_cast<long long>(spec->dupsDropped));
        }
    } else if (STAGE_OR == stats.stageType) {
        OrStats* spec = static_cast<OrStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("dupsTested", static_cast<long long>(spec->dupsTested));
            bob->appendNumber("dupsDropped", static_cast<long long>(spec->dupsDropped));
        }
    } else if (STAGE_LIMIT == stats.stageType) {
        LimitStats* spec = static_cast<LimitStats*>(stats.specific.get());
        bob->appendNumber("limitAmount", static_cast<long long>(spec->limit));
    } else if (isProjectionStageType(stats.stageType)) {
        ProjectionStats* spec = static_cast<ProjectionStats*>(stats.specific.get());
        bob->append("transformBy", spec->projObj);
    } else if (STAGE_RECORD_STORE_FAST_COUNT == stats.stageType) {
        CountStats* spec = static_cast<CountStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nCounted", spec->nCounted);
            bob->appendNumber("nSkipped", spec->nSkipped);
        }
    } else if (STAGE_SAMPLE_FROM_TIMESERIES_BUCKET == stats.stageType) {
        SampleFromTimeseriesBucketStats* spec =
            static_cast<SampleFromTimeseriesBucketStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nBucketsDiscarded", static_cast<long long>(spec->nBucketsDiscarded));
            bob->appendNumber("dupsTested", static_cast<long long>(spec->dupsTested));
            bob->appendNumber("dupsDropped", static_cast<long long>(spec->dupsDropped));
        }
    } else if (STAGE_SHARDING_FILTER == stats.stageType) {
        ShardingFilterStats* spec = static_cast<ShardingFilterStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("chunkSkips", static_cast<long long>(spec->chunkSkips));
        }
    } else if (STAGE_SKIP == stats.stageType) {
        SkipStats* spec = static_cast<SkipStats*>(stats.specific.get());
        bob->appendNumber("skipAmount", static_cast<long long>(spec->skip));
    } else if (isSortStageType(stats.stageType)) {
        SortStats* spec = static_cast<SortStats*>(stats.specific.get());
        bob->append("sortPattern", spec->sortPattern);
        bob->appendNumber("memLimit", static_cast<long long>(spec->maxMemoryUsageBytes));

        if (spec->limit > 0) {
            bob->appendNumber("limitAmount", static_cast<long long>(spec->limit));
        }

        bob->append("type", stats.stageType == STAGE_SORT_SIMPLE ? "simple" : "default");

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("totalDataSizeSorted",
                              static_cast<long long>(spec->totalDataSizeBytes));
            bob->appendBool("usedDisk", (spec->spills > 0));
            bob->appendNumber("spills", static_cast<long long>(spec->spills));
            bob->appendNumber("spilledDataStorageSize",
                              static_cast<long long>(spec->spilledDataStorageSize));
        }
    } else if (STAGE_SORT_MERGE == stats.stageType) {
        MergeSortStats* spec = static_cast<MergeSortStats*>(stats.specific.get());
        bob->append("sortPattern", spec->sortPattern);

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("dupsTested", static_cast<long long>(spec->dupsTested));
            bob->appendNumber("dupsDropped", static_cast<long long>(spec->dupsDropped));
        }
    } else if (STAGE_TEXT_MATCH == stats.stageType) {
        TextMatchStats* spec = static_cast<TextMatchStats*>(stats.specific.get());

        bob->append("indexPrefix", spec->indexPrefix);
        bob->append("indexName", spec->indexName);
        bob->append("parsedTextQuery", spec->parsedTextQuery);
        bob->append("textIndexVersion", spec->textIndexVersion);

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsRejected", static_cast<long long>(spec->docsRejected));
        }
    } else if (STAGE_TEXT_OR == stats.stageType) {
        TextOrStats* spec = static_cast<TextOrStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsExamined", static_cast<long long>(spec->fetches));
        }
    } else if (STAGE_TIMESERIES_MODIFY == stats.stageType) {
        TimeseriesModifyStats* spec = static_cast<TimeseriesModifyStats*>(stats.specific.get());

        bob->append("opType", spec->opType);
        bob->append("bucketFilter", spec->bucketFilter);
        bob->append("residualFilter", spec->residualFilter);

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nBucketsUnpacked", static_cast<long long>(spec->nBucketsUnpacked));
            bob->appendNumber("nMeasurementsDeleted",
                              static_cast<long long>(spec->nMeasurementsDeleted));
        }
    } else if (STAGE_UNPACK_TIMESERIES_BUCKET == stats.stageType) {
        UnpackTimeseriesBucketStats* spec =
            static_cast<UnpackTimeseriesBucketStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nBucketsUnpacked", static_cast<long long>(spec->nBucketsUnpacked));
        }
    } else if (STAGE_UPDATE == stats.stageType) {
        UpdateStats* spec = static_cast<UpdateStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nMatched", static_cast<long long>(spec->nMatched));
            bob->appendNumber("nWouldModify", static_cast<long long>(spec->nModified));
            bob->appendNumber("nWouldUpsert", static_cast<long long>(spec->nUpserted));
        }
    } else if (STAGE_SPOOL == stats.stageType) {
        SpoolStats* spec = static_cast<SpoolStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("totalDataSizeSpooled",
                              static_cast<long long>(spec->totalDataSizeBytes));
        }
    }

    // We're done if there are no children.
    if (stats.children.empty()) {
        return;
    }

    // If there's just one child (a common scenario), avoid making an array. This makes
    // the output more readable by saving a level of nesting. Name the field 'inputStage'
    // rather than 'inputStages'.
    if (1 == stats.children.size()) {
        BSONObjBuilder childBob(bob->subobjStart("inputStage"));
        statsToBSON(*stats.children[0], verbosity, planIdx, &childBob, topLevelBob);
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.

    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
    for (size_t i = 0; i < stats.children.size(); ++i) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSON(*stats.children[i], verbosity, planIdx, &childBob, topLevelBob);
    }
    childrenBob.doneFast();
}

PlanSummaryStats collectExecutionStatsSummary(const PlanStageStats* stats,
                                              const boost::optional<size_t> planIdx) {
    if (STAGE_MULTI_PLAN == stats->stageType) {
        tassert(3420004, "Invalid child plan index", planIdx && planIdx < stats->children.size());
        // Skip the MultiPlanStage when it is at the top of the plan, and extract stats from
        // its child of interest to the caller.
        stats = stats->children[*planIdx].get();
    }

    PlanSummaryStats summary;
    summary.nReturned = stats->common.advanced;

    if (stats->common.executionTime) {
        summary.executionTime.executionTimeEstimate = *stats->common.executionTime;
        summary.executionTime.precision = QueryExecTimerPrecision::kMillis;
    }

    // Flatten the stats tree into a list.
    std::vector<const PlanStageStats*> statsNodes;
    flattenStatsTree(stats, planIdx, &statsNodes);

    // Iterate over all stages in the tree and get the total number of keys/docs examined.
    // These are just aggregations of information already available in the stats tree.
    for (size_t i = 0; i < statsNodes.size(); ++i) {
        tassert(3420005, "Unexpected MultiPlanStage", STAGE_MULTI_PLAN != statsNodes[i]->stageType);
        summary.totalKeysExamined +=
            getKeysExamined(statsNodes[i]->stageType, statsNodes[i]->specific.get());
        summary.totalDocsExamined +=
            getDocsExamined(statsNodes[i]->stageType, statsNodes[i]->specific.get());
    }

    summary.planFailed = stats->common.failed;

    return summary;
}
}  // namespace

void appendMultikeyPaths(const BSONObj& keyPattern,
                         const MultikeyPaths& multikeyPaths,
                         BSONObjBuilder* bob) {
    BSONObjBuilder subMultikeyPaths(bob->subobjStart("multiKeyPaths"));

    size_t i = 0;
    for (const auto& keyElem : keyPattern) {
        const FieldRef path{keyElem.fieldNameStringData()};

        BSONArrayBuilder arrMultikeyComponents(
            subMultikeyPaths.subarrayStart(keyElem.fieldNameStringData()));
        for (const auto& multikeyComponent : multikeyPaths[i]) {
            arrMultikeyComponents.append(path.dottedSubstring(0, multikeyComponent + 1));
        }
        arrMultikeyComponents.doneFast();

        ++i;
    }

    subMultikeyPaths.doneFast();
}

const PlanExplainer::ExplainVersion& PlanExplainerImpl::getVersion() const {
    static const ExplainVersion kExplainVersion = "1";
    return kExplainVersion;
}

bool PlanExplainerImpl::isMultiPlan() const {
    return getStageByType(_root, STAGE_MULTI_PLAN) != nullptr;
}

std::string PlanExplainerImpl::getPlanSummary() const {
    std::vector<const PlanStage*> stages;
    flattenExecTree(_root, &stages);

    // Use this stream to build the plan summary string.
    StringBuilder sb;
    bool seenLeaf = false;

    for (size_t i = 0; i < stages.size(); i++) {
        if (stages[i]->getChildren().empty()) {
            tassert(
                3420006, "Unexpected MultiPlanStage", STAGE_MULTI_PLAN != stages[i]->stageType());
            // This is a leaf node. Add to the plan summary string accordingly. Unless
            // this is the first leaf we've seen, add a delimiting string first.
            if (seenLeaf) {
                sb << ", ";
            } else {
                seenLeaf = true;
            }
            addStageSummaryStr(stages[i], sb);
        }
    }

    return sb.str();
}

/**
 * Returns a pointer to a MultiPlanStage under 'root', or null if this plan has no such stage.
 */
MultiPlanStage* getMultiPlanStage(PlanStage* root) {
    if (!root) {
        return nullptr;
    }
    auto stage = getStageByType(root, STAGE_MULTI_PLAN);
    tassert(3420007,
            "Found stage must be MultiPlanStage",
            stage == nullptr || stage->stageType() == STAGE_MULTI_PLAN);
    auto mps = static_cast<MultiPlanStage*>(stage);
    return mps;
}

/**
 * If 'root' has a MultiPlanStage returns the index of its best plan. Otherwise returns an
 * initialized value.
 */
boost::optional<size_t> getWinningPlanIdx(PlanStage* root) {
    if (auto mps = getMultiPlanStage(root); mps) {
        auto planIdx = mps->bestPlanIdx();
        tassert(3420008, "Trying to get stats of a MultiPlanStage without winning plan", planIdx);
        return planIdx;
    }
    return {};
}

/**
 * If 'root' has a MultiPlanStage returns the score of its best plan.
 */
boost::optional<double> getWinningPlanScore(PlanStage* root) {
    if (const auto mps = getMultiPlanStage(root); mps) {
        auto bestPlanIdx = mps->bestPlanIdx();
        tassert(5408300,
                "Trying to get best plan index of a MultiPlanStage without winning plan",
                bestPlanIdx);
        return mps->getCandidateScore(*bestPlanIdx);
    }
    return {};
}

void PlanExplainerImpl::getSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);
    // We can get some of the fields we need from the common stats stored in the
    // root stage of the plan tree.
    const CommonStats* common = _root->getCommonStats();
    statsOut->nReturned = common->advanced;

    // The other fields are aggregations over the stages in the plan tree. We flatten
    // the tree into a list and then compute these aggregations.
    std::vector<const PlanStage*> stages;
    flattenExecTree(_root, &stages);

    statsOut->totalKeysExamined = 0;
    statsOut->totalDocsExamined = 0;
    statsOut->indexesUsed.clear();
    statsOut->collectionScans = 0;
    statsOut->collectionScansNonTailable = 0;

    for (size_t i = 0; i < stages.size(); i++) {
        statsOut->totalKeysExamined +=
            getKeysExamined(stages[i]->stageType(), stages[i]->getSpecificStats());
        statsOut->totalDocsExamined +=
            getDocsExamined(stages[i]->stageType(), stages[i]->getSpecificStats());

        if (isSortStageType(stages[i]->stageType())) {
            auto sortStage = static_cast<const SortStage*>(stages[i]);
            auto sortStats = static_cast<const SortStats*>(sortStage->getSpecificStats());
            PlanSummaryStatsVisitor(*statsOut).visit(sortStats);
        }

        if (STAGE_IXSCAN == stages[i]->stageType()) {
            const IndexScan* ixscan = static_cast<const IndexScan*>(stages[i]);
            const IndexScanStats* ixscanStats =
                static_cast<const IndexScanStats*>(ixscan->getSpecificStats());
            statsOut->indexesUsed.insert(ixscanStats->indexName);
        } else if (STAGE_COUNT_SCAN == stages[i]->stageType()) {
            const CountScan* countScan = static_cast<const CountScan*>(stages[i]);
            const CountScanStats* countScanStats =
                static_cast<const CountScanStats*>(countScan->getSpecificStats());
            statsOut->indexesUsed.insert(countScanStats->indexName);
        } else if (STAGE_IDHACK == stages[i]->stageType()) {
            const IDHackStage* idHackStage = static_cast<const IDHackStage*>(stages[i]);
            const IDHackStats* idHackStats =
                static_cast<const IDHackStats*>(idHackStage->getSpecificStats());
            statsOut->indexesUsed.insert(idHackStats->indexName);
        } else if (STAGE_DISTINCT_SCAN == stages[i]->stageType()) {
            const DistinctScan* distinctScan = static_cast<const DistinctScan*>(stages[i]);
            const DistinctScanStats* distinctScanStats =
                static_cast<const DistinctScanStats*>(distinctScan->getSpecificStats());
            statsOut->indexesUsed.insert(distinctScanStats->indexName);
        } else if (STAGE_TEXT_MATCH == stages[i]->stageType()) {
            const TextMatchStage* textStage = static_cast<const TextMatchStage*>(stages[i]);
            const TextMatchStats* textStats =
                static_cast<const TextMatchStats*>(textStage->getSpecificStats());
            statsOut->indexesUsed.insert(textStats->indexName);
        } else if (STAGE_GEO_NEAR_2D == stages[i]->stageType() ||
                   STAGE_GEO_NEAR_2DSPHERE == stages[i]->stageType()) {
            const NearStage* nearStage = static_cast<const NearStage*>(stages[i]);
            const NearStats* nearStats =
                static_cast<const NearStats*>(nearStage->getSpecificStats());
            statsOut->indexesUsed.insert(nearStats->indexName);
        } else if (STAGE_CACHED_PLAN == stages[i]->stageType()) {
            const CachedPlanStage* cachedPlan = static_cast<const CachedPlanStage*>(stages[i]);
            const CachedPlanStats* cachedStats =
                static_cast<const CachedPlanStats*>(cachedPlan->getSpecificStats());
            statsOut->replanReason = cachedStats->replanReason;
            // Nonnull replanReason indicates cached plan was less effecient than expected and an
            // alternative plan was chosen.
            statsOut->replanReason ? statsOut->fromPlanCache = false
                                   : statsOut->fromPlanCache = true;
        } else if (STAGE_MULTI_PLAN == stages[i]->stageType()) {
            statsOut->fromMultiPlanner = true;
        } else if (STAGE_COLLSCAN == stages[i]->stageType()) {
            statsOut->collectionScans++;
            const auto collScan = static_cast<const CollectionScan*>(stages[i]);
            const auto collScanStats =
                static_cast<const CollectionScanStats*>(collScan->getSpecificStats());
            if (!collScanStats->tailable)
                statsOut->collectionScansNonTailable++;
        }
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerImpl::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    const auto winningPlanIdx = getWinningPlanIdx(_root);
    auto&& [stats, summary] = [&]()
        -> std::pair<std::unique_ptr<PlanStageStats>, const boost::optional<PlanSummaryStats>> {
        auto stats = _root->getStats();
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            auto summary = collectExecutionStatsSummary(stats.get(), winningPlanIdx);
            if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
                summary.score = getWinningPlanScore(_root);
            }
            return {std::move(stats), summary};
        }

        return {std::move(stats), boost::none};
    }();

    BSONObjBuilder bob;
    statsToBSON(*stats, verbosity, winningPlanIdx, &bob, &bob);
    return {bob.obj(), std::move(summary)};
}

PlanExplainer::PlanStatsDetails PlanExplainerImpl::getWinningPlanTrialStats() const {
    return getWinningPlanStats(ExplainOptions::Verbosity::kExecAllPlans);
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerImpl::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    std::vector<PlanStatsDetails> res;
    auto mps = getMultiPlanStage(_root);
    if (nullptr == mps) {
        return res;
    }
    auto bestPlanIdx = mps->bestPlanIdx();

    tassert(3420009, "Trying to get stats of a MultiPlanStage without winning plan", bestPlanIdx);

    const auto mpsStats = mps->getStats();
    // Get the stats from the trial period for all the plans.
    for (size_t i = 0; i < mpsStats->children.size(); ++i) {
        if (i != *bestPlanIdx) {
            BSONObjBuilder bob;
            auto stats = _root->getStats();
            statsToBSON(*stats, verbosity, i, &bob, &bob);
            auto summary = [&]() -> boost::optional<PlanSummaryStats> {
                if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
                    auto summary = collectExecutionStatsSummary(stats.get(), i);
                    if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
                        summary.score = mps->getCandidateScore(i);
                    }
                    return summary;
                }
                return {};
            }();
            res.push_back({bob.obj(), summary});
        }
    }

    return res;
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerImpl::getCachedPlanStats(
    const plan_cache_debug_info::DebugInfo& debugInfo, ExplainOptions::Verbosity verbosity) const {
    const auto& decision = *debugInfo.decision;
    std::vector<PlanStatsDetails> res;
    auto winningPlanIdx = getWinningPlanIdx(_root);

    for (auto&& stats : decision.getStats<PlanStageStats>().candidatePlanStats) {
        BSONObjBuilder bob;
        statsToBSON(*stats, verbosity, winningPlanIdx, &bob, &bob);
        res.push_back({bob.obj(),
                       {verbosity >= ExplainOptions::Verbosity::kExecStats,
                        collectExecutionStatsSummary(stats.get(), winningPlanIdx)}});
    }
    return res;
}

PlanStage* getStageByType(PlanStage* root, StageType type) {
    tassert(3420010, "Can't find a stage in a NULL plan root", root != nullptr);
    if (root->stageType() == type) {
        return root;
    }

    const auto& children = root->getChildren();
    for (size_t i = 0; i < children.size(); i++) {
        PlanStage* result = getStageByType(children[i].get(), type);
        if (result) {
            return result;
        }
    }

    return nullptr;
}
}  // namespace mongo
