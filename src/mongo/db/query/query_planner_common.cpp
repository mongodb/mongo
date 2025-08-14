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


#include "mongo/db/query/query_planner_common.h"

#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
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
        invariant(!isSortStageType(type));
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
        invariant(!isSortStageType(type));
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
