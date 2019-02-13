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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

void QueryPlannerCommon::reverseScans(QuerySolutionNode* node) {
    StageType type = node->getType();

    if (STAGE_IXSCAN == type) {
        IndexScanNode* isn = static_cast<IndexScanNode*>(node);
        isn->direction *= -1;

        isn->bounds = isn->bounds.reverse();

        invariant(isn->bounds.isValidFor(isn->index.keyPattern, isn->direction),
                  str::stream() << "Invalid bounds: " << redact(isn->bounds.toString()));

        // TODO: we can just negate every value in the already computed properties.
        isn->computeProperties();
    } else if (STAGE_DISTINCT_SCAN == type) {
        DistinctNode* dn = static_cast<DistinctNode*>(node);
        dn->direction *= -1;

        dn->bounds = dn->bounds.reverse();

        invariant(dn->bounds.isValidFor(dn->index.keyPattern, dn->direction),
                  str::stream() << "Invalid bounds: " << redact(dn->bounds.toString()));

        dn->computeProperties();
    } else if (STAGE_SORT_MERGE == type) {
        // reverse direction of comparison for merge
        MergeSortNode* msn = static_cast<MergeSortNode*>(node);
        msn->sort = reverseSortObj(msn->sort);
    } else {
        invariant(STAGE_SORT != type);
        // This shouldn't be here...
    }

    for (size_t i = 0; i < node->children.size(); ++i) {
        reverseScans(node->children[i]);
    }
}

}  // namespace mongo
