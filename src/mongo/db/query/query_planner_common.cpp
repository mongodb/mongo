// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/query_planner_common.h"

#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

bool QueryPlannerCommon::scanDirectionsEqual(QuerySolutionNode* node, int direction) {
    StageType type = node->getType();

    boost::optional<int> scanDir;
    if (STAGE_IXSCAN == type) {
        IndexScanNode* isn = static_cast<IndexScanNode*>(node);
        scanDir = isn->direction;
    } else if (STAGE_DISTINCT_SCAN == type) {
        DistinctNode* dn = static_cast<DistinctNode*>(node);
        scanDir = dn->direction;
    } else if (STAGE_COLLSCAN == type) {
        CollectionScanNode* collScan = static_cast<CollectionScanNode*>(node);
        scanDir = collScan->direction;
    } else {
        // We shouldn't encounter a sort stage.
        tassert(11321046,
                fmt::format("Encountered unexpected sort stage {}", nodeStageTypeToString(node)),
                !isSortStageType(type));
    }

    // If we found something with a direction, and the direction doesn't match, we return false.
    if (scanDir && scanDir != direction) {
        return false;
    }

    for (size_t i = 0; i < node->children.size(); ++i) {
        if (!scanDirectionsEqual(node->children[i].get(), direction)) {
            return false;
        }
    }
    return true;
}

void QueryPlannerCommon::reverseScans(QuerySolutionNode* node, bool reverseCollScans) {
    StageType type = node->getType();

    if (STAGE_IXSCAN == type) {
        IndexScanNode* isn = static_cast<IndexScanNode*>(node);
        isn->direction *= -1;

        isn->bounds = isn->bounds.reverse();

        invariant(isn->bounds.isValidFor(
                      isn->index.keyPattern, isn->direction, isn->index.collator != nullptr),
                  str::stream() << "Invalid bounds: "
                                << redact(isn->bounds.toString(isn->index.collator != nullptr)));

        // TODO: we can just negate every value in the already computed properties.
        isn->computeProperties();
    } else if (STAGE_DISTINCT_SCAN == type) {
        DistinctNode* dn = static_cast<DistinctNode*>(node);
        dn->direction *= -1;

        dn->bounds = dn->bounds.reverse();

        invariant(dn->bounds.isValidFor(
                      dn->index.keyPattern, dn->direction, dn->index.collator != nullptr),
                  str::stream() << "Invalid bounds: "
                                << redact(dn->bounds.toString(dn->index.collator != nullptr)));

        dn->computeProperties();
    } else if (STAGE_SORT_MERGE == type) {
        // reverse direction of comparison for merge
        MergeSortNode* msn = static_cast<MergeSortNode*>(node);
        msn->sort = reverseSortObj(msn->sort);
    } else if (reverseCollScans && STAGE_COLLSCAN == type) {
        CollectionScanNode* collScan = static_cast<CollectionScanNode*>(node);
        collScan->direction *= -1;
    } else {
        // Reversing scans is done in order to determine whether or not we need to add an explicit
        // SORT stage. There shouldn't already be one present in the plan.
        tassert(11321047,
                fmt::format("Encountered unexpected sort stage {}", nodeStageTypeToString(node)),
                !isSortStageType(type));
    }

    for (size_t i = 0; i < node->children.size(); ++i) {
        reverseScans(node->children[i].get(), reverseCollScans);
    }
}

boost::optional<int> QueryPlannerCommon::determineClusteredScanDirection(
    const CanonicalQuery& query,
    const boost::optional<ClusteredCollectionInfo>& clusteredInfo,
    const CollatorInterface* clusteredCollectionCollator) {
    if (clusteredInfo && query.getSortPattern() &&
        CollatorInterface::collatorsMatch(clusteredCollectionCollator, query.getCollator())) {
        BSONObj kp = clustered_util::getSortPattern(clusteredInfo->getIndexSpec());
        if (QueryPlannerCommon::providesSort(query, kp)) {
            return 1;
        } else if (QueryPlannerCommon::providesSort(query,
                                                    QueryPlannerCommon::reverseSortObj(kp))) {
            return -1;
        }
    }

    return boost::none;
}

}  // namespace mongo
