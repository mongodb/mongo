/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/explain.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count_scan.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/near.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/text.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/hex.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

namespace {

using namespace mongo;
using std::string;
using std::unique_ptr;
using std::vector;

/**
 * Traverse the tree rooted at 'root', and add all tree nodes into the list 'flattened'.
 */
void flattenStatsTree(const PlanStageStats* root, vector<const PlanStageStats*>* flattened) {
    invariant(root->stageType != STAGE_MULTI_PLAN);
    flattened->push_back(root);
    for (auto&& child : root->children) {
        flattenStatsTree(child.get(), flattened);
    }
}

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
    } else if (STAGE_TEXT == stage->stageType()) {
        const TextStats* spec = static_cast<const TextStats*>(specific);
        const KeyPattern keyPattern{spec->indexPrefix};
        sb << " " << keyPattern;
    }
}

/**
 * Adds the path-level multikey information to the explain output in a field called "multiKeyPaths".
 * The value associated with the "multiKeyPaths" field is an object with keys equal to those in the
 * index key pattern and values equal to an array of strings corresponding to paths that cause the
 * index to be multikey.
 *
 * For example, with the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause the
 * index to be multikey, we'd have {'multiKeyPaths': {'a.b': ['a', 'a.b'], 'a.c': ['a']}}.
 *
 * This function should only be called if the associated index supports path-level multikey
 * tracking.
 */
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

/**
 * Gather the PlanStageStats for all of the losing plans. If exec doesn't have a MultiPlanStage
 * (or any losing plans), will return an empty vector.
 */
std::vector<std::unique_ptr<PlanStageStats>> getRejectedPlansTrialStats(PlanExecutorImpl* exec) {
    // Inspect the tree to see if there is a MultiPlanStage. Plan selection has already happened at
    // this point, since we have a PlanExecutor.
    const auto mps = exec->getMultiPlanStage();
    std::vector<std::unique_ptr<PlanStageStats>> res;

    // Get the stats from the trial period for all the plans.
    if (mps) {
        const auto mpsStats = mps->getStats();
        for (size_t i = 0; i < mpsStats->children.size(); ++i) {
            if (i != static_cast<size_t>(mps->bestPlanIdx())) {
                res.emplace_back(std::move(mpsStats->children[i]));
            }
        }
    }

    return res;
}

/**
 * Get PlanExecutor's winning plan stats tree.
 */
unique_ptr<PlanStageStats> getWinningPlanStatsTree(const PlanExecutorImpl* exec) {
    auto mps = exec->getMultiPlanStage();
    return mps ? std::move(mps->getStats()->children[mps->bestPlanIdx()])
               : std::move(exec->getRootStage()->getStats());
}

/**
 * Executes the given plan executor, discarding the resulting documents, until it reaches EOF. If a
 * runtime error occur or execution is killed, throws a DBException.
 *
 * If 'exec' is configured for yielding, then a call to this helper could result in a yield.
 */
void executePlan(PlanExecutor* exec) {
    BSONObj obj;
    while (exec->getNext(&obj, nullptr) == PlanExecutor::ADVANCED) {
        // Discard the resulting documents.
    }
}

}  // namespace

namespace mongo {

using str::stream;

void Explain::flattenExecTree(const PlanStage* root, vector<const PlanStage*>* flattened) {
    flattened->push_back(root);

    if (root->stageType() == STAGE_MULTI_PLAN) {
        // Only add the winning plan from a MultiPlanStage.
        auto mps = static_cast<const MultiPlanStage*>(root);
        const PlanStage* winningStage = mps->getChildren()[mps->bestPlanIdx()].get();
        return flattenExecTree(winningStage, flattened);
    }

    const auto& children = root->getChildren();
    for (size_t i = 0; i < children.size(); ++i) {
        flattenExecTree(children[i].get(), flattened);
    }
}

size_t Explain::getKeysExamined(StageType type, const SpecificStats* specific) {
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

size_t Explain::getDocsExamined(StageType type, const SpecificStats* specific) {
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

void Explain::statsToBSON(const PlanStageStats& stats,
                          ExplainOptions::Verbosity verbosity,
                          BSONObjBuilder* bob,
                          BSONObjBuilder* topLevelBob) {
    invariant(bob);
    invariant(topLevelBob);

    // Stop as soon as the BSON object we're building exceeds 10 MB.
    static const int kMaxStatsBSONSize = 10 * 1024 * 1024;
    if (topLevelBob->len() > kMaxStatsBSONSize) {
        bob->append("warning", "stats tree exceeded 10 MB");
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
        bob->appendNumber("nReturned", stats.common.advanced);
        // Include executionTimeMillis if it was recorded.
        if (stats.common.executionTimeMillis) {
            bob->appendNumber("executionTimeMillisEstimate", *stats.common.executionTimeMillis);
        }

        bob->appendNumber("works", stats.common.works);
        bob->appendNumber("advanced", stats.common.advanced);
        bob->appendNumber("needTime", stats.common.needTime);
        bob->appendNumber("needYield", stats.common.needYield);
        bob->appendNumber("saveState", stats.common.yields);
        bob->appendNumber("restoreState", stats.common.unyields);
        if (stats.common.failed)
            bob->appendBool("failed", stats.common.failed);
        bob->appendNumber("isEOF", stats.common.isEOF);
    }

    // Stage-specific stats
    if (STAGE_AND_HASH == stats.stageType) {
        AndHashStats* spec = static_cast<AndHashStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("memUsage", spec->memUsage);
            bob->appendNumber("memLimit", spec->memLimit);

            for (size_t i = 0; i < spec->mapAfterChild.size(); ++i) {
                bob->appendNumber(string(stream() << "mapAfterChild_" << i),
                                  spec->mapAfterChild[i]);
            }
        }
    } else if (STAGE_AND_SORTED == stats.stageType) {
        AndSortedStats* spec = static_cast<AndSortedStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            for (size_t i = 0; i < spec->failedAnd.size(); ++i) {
                bob->appendNumber(string(stream() << "failedAnd_" << i), spec->failedAnd[i]);
            }
        }
    } else if (STAGE_COLLSCAN == stats.stageType) {
        CollectionScanStats* spec = static_cast<CollectionScanStats*>(stats.specific.get());
        bob->append("direction", spec->direction > 0 ? "forward" : "backward");
        if (spec->minTs) {
            bob->append("minTs", *(spec->minTs));
        }
        if (spec->maxTs) {
            bob->append("maxTs", *(spec->maxTs));
        }
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsExamined", spec->docsTested);
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
            bob->appendNumber("keysExamined", spec->keysExamined);
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

        BSONObjBuilder indexBoundsBob;
        indexBoundsBob.append("startKey", spec->startKey);
        indexBoundsBob.append("startKeyInclusive", spec->startKeyInclusive);
        indexBoundsBob.append("endKey", spec->endKey);
        indexBoundsBob.append("endKeyInclusive", spec->endKeyInclusive);
        bob->append("indexBounds", indexBoundsBob.obj());
    } else if (STAGE_DELETE == stats.stageType) {
        DeleteStats* spec = static_cast<DeleteStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nWouldDelete", spec->docsDeleted);
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

        if ((topLevelBob->len() + spec->indexBounds.objsize()) > kMaxStatsBSONSize) {
            bob->append("warning", "index bounds omitted due to BSON size limit");
        } else {
            bob->append("indexBounds", spec->indexBounds);
        }

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("keysExamined", spec->keysExamined);
        }
    } else if (STAGE_ENSURE_SORTED == stats.stageType) {
        EnsureSortedStats* spec = static_cast<EnsureSortedStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nDropped", spec->nDropped);
        }
    } else if (STAGE_FETCH == stats.stageType) {
        FetchStats* spec = static_cast<FetchStats*>(stats.specific.get());
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsExamined", spec->docsExamined);
            bob->appendNumber("alreadyHasObj", spec->alreadyHasObj);
        }
    } else if (STAGE_GEO_NEAR_2D == stats.stageType || STAGE_GEO_NEAR_2DSPHERE == stats.stageType) {
        NearStats* spec = static_cast<NearStats*>(stats.specific.get());

        bob->append("keyPattern", spec->keyPattern);
        bob->append("indexName", spec->indexName);
        bob->append("indexVersion", spec->indexVersion);

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            BSONArrayBuilder intervalsBob(bob->subarrayStart("searchIntervals"));
            for (vector<IntervalStats>::const_iterator it = spec->intervalStats.begin();
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
            bob->appendNumber("keysExamined", spec->keysExamined);
            bob->appendNumber("docsExamined", spec->docsExamined);
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

        if ((topLevelBob->len() + spec->indexBounds.objsize()) > kMaxStatsBSONSize) {
            bob->append("warning", "index bounds omitted due to BSON size limit");
        } else {
            bob->append("indexBounds", spec->indexBounds);
        }

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("keysExamined", spec->keysExamined);
            bob->appendNumber("seeks", spec->seeks);
            bob->appendNumber("dupsTested", spec->dupsTested);
            bob->appendNumber("dupsDropped", spec->dupsDropped);
        }
    } else if (STAGE_OR == stats.stageType) {
        OrStats* spec = static_cast<OrStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("dupsTested", spec->dupsTested);
            bob->appendNumber("dupsDropped", spec->dupsDropped);
        }
    } else if (STAGE_LIMIT == stats.stageType) {
        LimitStats* spec = static_cast<LimitStats*>(stats.specific.get());
        bob->appendNumber("limitAmount", spec->limit);
    } else if (isProjectionStageType(stats.stageType)) {
        ProjectionStats* spec = static_cast<ProjectionStats*>(stats.specific.get());
        bob->append("transformBy", spec->projObj);
    } else if (STAGE_RECORD_STORE_FAST_COUNT == stats.stageType) {
        CountStats* spec = static_cast<CountStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nCounted", spec->nCounted);
            bob->appendNumber("nSkipped", spec->nSkipped);
        }
    } else if (STAGE_SHARDING_FILTER == stats.stageType) {
        ShardingFilterStats* spec = static_cast<ShardingFilterStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("chunkSkips", spec->chunkSkips);
        }
    } else if (STAGE_SKIP == stats.stageType) {
        SkipStats* spec = static_cast<SkipStats*>(stats.specific.get());
        bob->appendNumber("skipAmount", spec->skip);
    } else if (isSortStageType(stats.stageType)) {
        SortStats* spec = static_cast<SortStats*>(stats.specific.get());
        bob->append("sortPattern", spec->sortPattern);
        bob->appendIntOrLL("memLimit", spec->maxMemoryUsageBytes);

        if (spec->limit > 0) {
            bob->appendIntOrLL("limitAmount", spec->limit);
        }

        bob->append("type", stats.stageType == STAGE_SORT_SIMPLE ? "simple" : "default");

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendIntOrLL("totalDataSizeSorted", spec->totalDataSizeBytes);
            bob->appendBool("usedDisk", spec->wasDiskUsed);
        }
    } else if (STAGE_SORT_MERGE == stats.stageType) {
        MergeSortStats* spec = static_cast<MergeSortStats*>(stats.specific.get());
        bob->append("sortPattern", spec->sortPattern);

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("dupsTested", spec->dupsTested);
            bob->appendNumber("dupsDropped", spec->dupsDropped);
        }
    } else if (STAGE_TEXT == stats.stageType) {
        TextStats* spec = static_cast<TextStats*>(stats.specific.get());

        bob->append("indexPrefix", spec->indexPrefix);
        bob->append("indexName", spec->indexName);
        bob->append("parsedTextQuery", spec->parsedTextQuery);
        bob->append("textIndexVersion", spec->textIndexVersion);
    } else if (STAGE_TEXT_MATCH == stats.stageType) {
        TextMatchStats* spec = static_cast<TextMatchStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsRejected", spec->docsRejected);
        }
    } else if (STAGE_TEXT_OR == stats.stageType) {
        TextOrStats* spec = static_cast<TextOrStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("docsExamined", spec->fetches);
        }
    } else if (STAGE_UPDATE == stats.stageType) {
        UpdateStats* spec = static_cast<UpdateStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            bob->appendNumber("nMatched", spec->nMatched);
            bob->appendNumber("nWouldModify", spec->nModified);
            bob->appendNumber("nWouldUpsert", spec->nUpserted);
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
        BSONObjBuilder childBob;
        statsToBSON(*stats.children[0], verbosity, &childBob, topLevelBob);
        bob->append("inputStage", childBob.obj());
        return;
    }

    // There is more than one child. Recursively call statsToBSON(...) on each
    // of them and add them to the 'inputStages' array.

    BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
    for (size_t i = 0; i < stats.children.size(); ++i) {
        BSONObjBuilder childBob(childrenBob.subobjStart());
        statsToBSON(*stats.children[i], verbosity, &childBob, topLevelBob);
    }
    childrenBob.doneFast();
}

BSONObj Explain::statsToBSON(const PlanStageStats& stats, ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder bob;
    statsToBSON(stats, &bob, verbosity);
    return bob.obj();
}

BSONObj Explain::statsToBSON(const sbe::PlanStageStats& stats,
                             ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder bob;
    return bob.obj();
}

void Explain::statsToBSON(const PlanStageStats& stats,
                          BSONObjBuilder* bob,
                          ExplainOptions::Verbosity verbosity) {
    statsToBSON(stats, verbosity, bob, bob);
}

void Explain::generatePlannerInfo(PlanExecutor* exec,
                                  const CollectionPtr& collection,
                                  BSONObj extraInfo,
                                  BSONObjBuilder* out) {
    auto planExecImpl = dynamic_cast<PlanExecutorImpl*>(exec);
    uassert(4847801,
            "queryPlanner explain section is only supported for classic PlanStages",
            planExecImpl);

    CanonicalQuery* query = exec->getCanonicalQuery();

    BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));

    plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);
    plannerBob.append("namespace", exec->nss().ns());

    // Find whether there is an index filter set for the query shape. The 'indexFilterSet'
    // field will always be false in the case of EOF or idhack plans.
    bool indexFilterSet = false;
    boost::optional<uint32_t> queryHash;
    boost::optional<uint32_t> planCacheKeyHash;
    if (collection && exec->getCanonicalQuery()) {
        const QuerySettings* querySettings =
            QuerySettingsDecoration::get(collection->getSharedDecorations());
        PlanCacheKey planCacheKey = CollectionQueryInfo::get(collection)
                                        .getPlanCache()
                                        ->computeKey(*exec->getCanonicalQuery());
        planCacheKeyHash = canonical_query_encoder::computeHash(planCacheKey.toString());
        queryHash = canonical_query_encoder::computeHash(planCacheKey.getStableKeyStringData());

        if (auto allowedIndicesFilter =
                querySettings->getAllowedIndicesFilter(planCacheKey.getStableKey())) {
            // Found an index filter set on the query shape.
            indexFilterSet = true;
        }
    }
    plannerBob.append("indexFilterSet", indexFilterSet);

    // In general we should have a canonical query, but sometimes we may avoid
    // creating a canonical query as an optimization (specifically, the update system
    // does not canonicalize for idhack updates). In these cases, 'query' is NULL.
    if (nullptr != query) {
        BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
        query->root()->serialize(&parsedQueryBob);
        parsedQueryBob.doneFast();

        if (query->getCollator()) {
            plannerBob.append("collation", query->getCollator()->getSpec().toBSON());
        }
    }

    if (queryHash) {
        plannerBob.append("queryHash", zeroPaddedHex(*queryHash));
    }

    if (planCacheKeyHash) {
        plannerBob.append("planCacheKey", zeroPaddedHex(*planCacheKeyHash));
    }

    if (!extraInfo.isEmpty()) {
        plannerBob.appendElements(extraInfo);
    }

    BSONObjBuilder winningPlanBob(plannerBob.subobjStart("winningPlan"));
    const auto winnerStats = getWinningPlanStatsTree(planExecImpl);
    statsToBSON(*winnerStats.get(), &winningPlanBob, ExplainOptions::Verbosity::kQueryPlanner);
    winningPlanBob.doneFast();

    // Genenerate array of rejected plans.
    const vector<unique_ptr<PlanStageStats>> rejectedStats =
        getRejectedPlansTrialStats(planExecImpl);
    BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
    for (size_t i = 0; i < rejectedStats.size(); i++) {
        BSONObjBuilder childBob(allPlansBob.subobjStart());
        statsToBSON(*rejectedStats[i], &childBob, ExplainOptions::Verbosity::kQueryPlanner);
    }
    allPlansBob.doneFast();

    plannerBob.doneFast();
}

// static
void Explain::generateSinglePlanExecutionInfo(const PlanStageStats* stats,
                                              ExplainOptions::Verbosity verbosity,
                                              boost::optional<long long> totalTimeMillis,
                                              BSONObjBuilder* out) {
    out->appendNumber("nReturned", stats->common.advanced);

    // Time elapsed could might be either precise or approximate.
    if (totalTimeMillis) {
        out->appendNumber("executionTimeMillis", *totalTimeMillis);
    } else {
        invariant(stats->common.executionTimeMillis);
        out->appendNumber("executionTimeMillisEstimate", *stats->common.executionTimeMillis);
    }

    // Flatten the stats tree into a list.
    vector<const PlanStageStats*> statsNodes;
    flattenStatsTree(stats, &statsNodes);

    // Iterate over all stages in the tree and get the total number of keys/docs examined.
    // These are just aggregations of information already available in the stats tree.
    size_t totalKeysExamined = 0;
    size_t totalDocsExamined = 0;
    for (size_t i = 0; i < statsNodes.size(); ++i) {
        totalKeysExamined +=
            getKeysExamined(statsNodes[i]->stageType, statsNodes[i]->specific.get());
        totalDocsExamined +=
            getDocsExamined(statsNodes[i]->stageType, statsNodes[i]->specific.get());
    }

    out->appendNumber("totalKeysExamined", totalKeysExamined);
    out->appendNumber("totalDocsExamined", totalDocsExamined);
    if (stats->common.failed)
        out->appendBool("failed", stats->common.failed);

    // Add the tree of stages, with individual execution stats for each stage.
    BSONObjBuilder stagesBob(out->subobjStart("executionStages"));
    statsToBSON(*stats, &stagesBob, verbosity);
    stagesBob.doneFast();
}

std::unique_ptr<PlanStageStats> Explain::getWinningPlanTrialStats(PlanExecutor* exec) {
    auto planExecImpl = dynamic_cast<PlanExecutorImpl*>(exec);
    uassert(4847802,
            "getWinningPlanTrialStats() is only supported for classic PlanStages",
            planExecImpl);

    // Inspect the tree to see if there is a MultiPlanStage. Plan selection has already happened at
    // this point, since we have a PlanExecutor.
    const auto mps = planExecImpl->getMultiPlanStage();

    if (mps) {
        const auto mpsStats = mps->getStats();
        return std::move(mpsStats->children[mps->bestPlanIdx()]);
    }

    return nullptr;
}

void Explain::generateExecutionInfo(PlanExecutor* exec,
                                    ExplainOptions::Verbosity verbosity,
                                    Status executePlanStatus,
                                    PlanStageStats* winningPlanTrialStats,
                                    BSONObjBuilder* out) {
    auto planExecImpl = dynamic_cast<PlanExecutorImpl*>(exec);
    uassert(4847800,
            "executionStats explain section is only supported for classic PlanStages",
            planExecImpl);

    invariant(verbosity >= ExplainOptions::Verbosity::kExecStats);
    if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans &&
        planExecImpl->getMultiPlanStage()) {
        invariant(winningPlanTrialStats,
                  "winningPlanTrialStats must be non-null when requesting all execution stats");
    }
    BSONObjBuilder execBob(out->subobjStart("executionStats"));

    // If there is an execution error while running the query, the error is reported under
    // the "executionStats" section and the explain as a whole succeeds.
    execBob.append("executionSuccess", executePlanStatus.isOK());
    if (!executePlanStatus.isOK()) {
        execBob.append("errorMessage", executePlanStatus.reason());
        execBob.append("errorCode", executePlanStatus.code());
    }

    // Generate exec stats BSON for the winning plan.
    OperationContext* opCtx = exec->getOpCtx();
    long long totalTimeMillis = durationCount<Milliseconds>(CurOp::get(opCtx)->elapsedTimeTotal());
    const auto winningExecStats = getWinningPlanStatsTree(planExecImpl);
    generateSinglePlanExecutionInfo(winningExecStats.get(), verbosity, totalTimeMillis, &execBob);

    // Also generate exec stats for all plans, if the verbosity level is high enough.
    // These stats reflect what happened during the trial period that ranked the plans.
    if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
        // If we ranked multiple plans against each other, then add stats collected
        // from the trial period of the winning plan. The "allPlansExecution" section
        // will contain an apples-to-apples comparison of the winning plan's stats against
        // all rejected plans' stats collected during the trial period.

        BSONArrayBuilder allPlansBob(execBob.subarrayStart("allPlansExecution"));

        if (winningPlanTrialStats) {
            BSONObjBuilder planBob(allPlansBob.subobjStart());
            generateSinglePlanExecutionInfo(
                winningPlanTrialStats, verbosity, boost::none, &planBob);
            planBob.doneFast();
        }

        const vector<unique_ptr<PlanStageStats>> rejectedStats =
            getRejectedPlansTrialStats(planExecImpl);
        for (size_t i = 0; i < rejectedStats.size(); ++i) {
            BSONObjBuilder planBob(allPlansBob.subobjStart());
            generateSinglePlanExecutionInfo(
                rejectedStats[i].get(), verbosity, boost::none, &planBob);
            planBob.doneFast();
        }

        allPlansBob.doneFast();
    }

    execBob.doneFast();
}

void Explain::explainStages(PlanExecutor* exec,
                            const CollectionPtr& collection,
                            ExplainOptions::Verbosity verbosity,
                            Status executePlanStatus,
                            PlanStageStats* winningPlanTrialStats,
                            BSONObj extraInfo,
                            BSONObjBuilder* out) {
    //
    // Use the stats trees to produce explain BSON.
    //

    if (verbosity >= ExplainOptions::Verbosity::kQueryPlanner) {
        generatePlannerInfo(exec, collection, extraInfo, out);
    }

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        generateExecutionInfo(exec, verbosity, executePlanStatus, winningPlanTrialStats, out);
    }
}

void Explain::explainPipelineExecutor(PlanExecutorPipeline* exec,
                                      ExplainOptions::Verbosity verbosity,
                                      BSONObjBuilder* out) {
    invariant(exec);
    invariant(out);

    // If we need execution stats, this runs the plan in order to gather the stats.
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        // TODO SERVER-32732: An execution error should be reported in explain, but should not
        // cause the explain itself to fail.
        executePlan(exec);
    }

    *out << "stages" << Value(exec->writeExplainOps(verbosity));

    explain_common::generateServerInfo(out);
}

void Explain::explainStages(PlanExecutor* exec,
                            const CollectionPtr& collection,
                            ExplainOptions::Verbosity verbosity,
                            BSONObj extraInfo,
                            BSONObjBuilder* out) {
    uassert(4822877,
            "explainStages() is only supported for PlanStage trees",
            dynamic_cast<PlanExecutorImpl*>(exec));

    auto winningPlanTrialStats = Explain::getWinningPlanTrialStats(exec);

    Status executePlanStatus = Status::OK();
    const CollectionPtr* collectionPtr = &collection;

    // If we need execution stats, then run the plan in order to gather the stats.
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        try {
            executePlan(exec);
        } catch (const DBException&) {
            executePlanStatus = exceptionToStatus();
        }

        // If executing the query failed, for any number of reasons other than a planning failure,
        // then the collection may no longer be valid. We conservatively set our collection pointer
        // to null in case it is invalid.
        if (executePlanStatus != ErrorCodes::NoQueryExecutionPlans) {
            collectionPtr = &CollectionPtr::null;
        }
    }

    explainStages(exec,
                  *collectionPtr,
                  verbosity,
                  executePlanStatus,
                  winningPlanTrialStats.get(),
                  extraInfo,
                  out);

    explain_common::generateServerInfo(out);
}

std::string Explain::getPlanSummary(const PlanStage* root) {
    std::vector<const PlanStage*> stages;
    flattenExecTree(root, &stages);

    // Use this stream to build the plan summary string.
    StringBuilder sb;
    bool seenLeaf = false;

    for (size_t i = 0; i < stages.size(); i++) {
        if (stages[i]->getChildren().empty()) {
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

std::string Explain::getPlanSummary(const sbe::PlanStage* root) {
    // TODO: Support 'planSummary' for SBE plan stage trees.
    return "unsupported";
}

void Explain::planCacheEntryToBSON(const PlanCacheEntry& entry, BSONObjBuilder* out) {
    BSONObjBuilder shapeBuilder(out->subobjStart("createdFromQuery"));
    shapeBuilder.append("query", entry.query);
    shapeBuilder.append("sort", entry.sort);
    shapeBuilder.append("projection", entry.projection);
    if (!entry.collation.isEmpty()) {
        shapeBuilder.append("collation", entry.collation);
    }
    shapeBuilder.doneFast();
    out->append("queryHash", zeroPaddedHex(entry.queryHash));
    out->append("planCacheKey", zeroPaddedHex(entry.planCacheKey));

    // Append whether or not the entry is active.
    out->append("isActive", entry.isActive);
    out->append("works", static_cast<long long>(entry.works));

    BSONObjBuilder cachedPlanBob(out->subobjStart("cachedPlan"));
    Explain::statsToBSON(*(entry.decision->getStats<PlanStageStats>()[0]),
                         &cachedPlanBob,
                         ExplainOptions::Verbosity::kQueryPlanner);
    cachedPlanBob.doneFast();

    out->append("timeOfCreation", entry.timeOfCreation);

    BSONArrayBuilder creationBuilder(out->subarrayStart("creationExecStats"));
    for (auto&& stat : entry.decision->getStats<PlanStageStats>()) {
        BSONObjBuilder planBob(creationBuilder.subobjStart());
        Explain::generateSinglePlanExecutionInfo(
            stat.get(), ExplainOptions::Verbosity::kExecAllPlans, boost::none, &planBob);
        planBob.doneFast();
    }
    creationBuilder.doneFast();

    BSONArrayBuilder scoresBuilder(out->subarrayStart("candidatePlanScores"));
    for (double score : entry.decision->scores) {
        scoresBuilder.append(score);
    }

    std::for_each(entry.decision->failedCandidates.begin(),
                  entry.decision->failedCandidates.end(),
                  [&scoresBuilder](const auto&) { scoresBuilder.append(0.0); });

    scoresBuilder.doneFast();

    out->append("indexFilterSet", entry.plannerData[0]->indexFilterApplied);
}

}  // namespace mongo
