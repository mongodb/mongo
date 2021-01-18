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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
#include "mongo/db/exec/text.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/explain.h"
#include "mongo/util/str.h"

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
    } else if (STAGE_TEXT == stage->stageType()) {
        const TextStats* spec = static_cast<const TextStats*>(specific);
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
        const PlanStage* winningStage = mps->getChildren()[mps->bestPlanIdx()].get();
        return flattenExecTree(winningStage, flattened);
    }

    const auto& children = root->getChildren();
    for (size_t i = 0; i < children.size(); ++i) {
        flattenExecTree(children[i].get(), flattened);
    }
}

/**
 * Traverse the tree rooted at 'root', and add all tree nodes into the list 'flattened'.
 */
void flattenStatsTree(const PlanStageStats* root, std::vector<const PlanStageStats*>* flattened) {
    invariant(root->stageType != STAGE_MULTI_PLAN);
    flattened->push_back(root);
    for (auto&& child : root->children) {
        flattenStatsTree(child.get(), flattened);
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
 *
 * Generates the BSON stats at a verbosity specified by 'verbosity'.
 */
void statsToBSON(const PlanStageStats& stats,
                 ExplainOptions::Verbosity verbosity,
                 BSONObjBuilder* bob,
                 BSONObjBuilder* topLevelBob) {
    invariant(bob);
    invariant(topLevelBob);

    // Stop as soon as the BSON object we're building exceeds the limit.
    if (topLevelBob->len() > kMaxExplainStatsBSONSizeMB) {
        bob->append("warning", "stats tree exceeded BSON size limit for explain");
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
                bob->appendNumber(std::string(str::stream() << "mapAfterChild_" << i),
                                  spec->mapAfterChild[i]);
            }
        }
    } else if (STAGE_AND_SORTED == stats.stageType) {
        AndSortedStats* spec = static_cast<AndSortedStats*>(stats.specific.get());

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            for (size_t i = 0; i < spec->failedAnd.size(); ++i) {
                bob->appendNumber(std::string(str::stream() << "failedAnd_" << i),
                                  spec->failedAnd[i]);
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

        if ((topLevelBob->len() + spec->indexBounds.objsize()) > kMaxExplainStatsBSONSizeMB) {
            bob->append("warning", "index bounds omitted due to BSON size limit for explain");
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

        if ((topLevelBob->len() + spec->indexBounds.objsize()) > kMaxExplainStatsBSONSizeMB) {
            bob->append("warning", "index bounds omitted due to BSON size limit for explain");
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
            bob->appendBool("usedDisk", (spec->spills > 0));
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

PlanSummaryStats collectExecutionStatsSummary(const PlanStageStats* stats) {
    PlanSummaryStats summary;
    summary.nReturned = stats->common.advanced;

    if (stats->common.executionTimeMillis) {
        summary.executionTimeMillisEstimate = *stats->common.executionTimeMillis;
    }

    // Flatten the stats tree into a list.
    std::vector<const PlanStageStats*> statsNodes;
    flattenStatsTree(stats, &statsNodes);

    // Iterate over all stages in the tree and get the total number of keys/docs examined.
    // These are just aggregations of information already available in the stats tree.
    for (size_t i = 0; i < statsNodes.size(); ++i) {
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
    return getStageByType(_root, StageType::STAGE_MULTI_PLAN) != nullptr;
}

std::string PlanExplainerImpl::getPlanSummary() const {
    std::vector<const PlanStage*> stages;
    flattenExecTree(_root, &stages);

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

    for (size_t i = 0; i < stages.size(); i++) {
        statsOut->totalKeysExamined +=
            getKeysExamined(stages[i]->stageType(), stages[i]->getSpecificStats());
        statsOut->totalDocsExamined +=
            getDocsExamined(stages[i]->stageType(), stages[i]->getSpecificStats());

        if (isSortStageType(stages[i]->stageType())) {
            statsOut->hasSortStage = true;

            auto sortStage = static_cast<const SortStage*>(stages[i]);
            auto sortStats = static_cast<const SortStats*>(sortStage->getSpecificStats());
            statsOut->usedDisk = sortStats->spills > 0;
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
        } else if (STAGE_TEXT == stages[i]->stageType()) {
            const TextStage* textStage = static_cast<const TextStage*>(stages[i]);
            const TextStats* textStats =
                static_cast<const TextStats*>(textStage->getSpecificStats());
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
    auto stage = getStageByType(_root, StageType::STAGE_MULTI_PLAN);
    invariant(stage == nullptr || stage->stageType() == StageType::STAGE_MULTI_PLAN);
    auto mps = static_cast<MultiPlanStage*>(stage);

    auto&& [stats, summary] =
        [&]() -> std::pair<std::unique_ptr<PlanStageStats>, boost::optional<PlanSummaryStats>> {
        auto stats =
            mps ? std::move(mps->getStats()->children[mps->bestPlanIdx()]) : _root->getStats();

        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            return {std::move(stats), collectExecutionStatsSummary(stats.get())};
        }

        return {std::move(stats), boost::none};
    }();

    BSONObjBuilder bob;
    statsToBSON(*stats, verbosity, &bob, &bob);
    return {bob.obj(), std::move(summary)};
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerImpl::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    auto stage = getStageByType(_root, StageType::STAGE_MULTI_PLAN);
    invariant(stage == nullptr || stage->stageType() == StageType::STAGE_MULTI_PLAN);
    auto mps = static_cast<MultiPlanStage*>(stage);

    std::vector<PlanStatsDetails> res;

    // Get the stats from the trial period for all the plans.
    if (mps) {
        const auto mpsStats = mps->getStats();
        for (size_t i = 0; i < mpsStats->children.size(); ++i) {
            if (i != static_cast<size_t>(mps->bestPlanIdx())) {
                BSONObjBuilder bob;
                statsToBSON(*mpsStats->children[i], verbosity, &bob, &bob);
                res.push_back({bob.obj(),
                               {verbosity >= ExplainOptions::Verbosity::kExecStats,
                                collectExecutionStatsSummary(mpsStats->children[i].get())}});
            }
        }
    }

    return res;
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerImpl::getCachedPlanStats(
    const PlanCacheEntry::DebugInfo& debugInfo, ExplainOptions::Verbosity verbosity) const {
    const auto& decision = *debugInfo.decision;
    std::vector<PlanStatsDetails> res;
    for (auto&& stats : decision.getStats<PlanStageStats>().candidatePlanStats) {
        BSONObjBuilder bob;
        statsToBSON(*stats, verbosity, &bob, &bob);
        res.push_back({bob.obj(),
                       {verbosity >= ExplainOptions::Verbosity::kExecStats,
                        collectExecutionStatsSummary(stats.get())}});
    }
    return res;
}

PlanStage* getStageByType(PlanStage* root, StageType type) {
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
