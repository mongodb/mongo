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

#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

namespace mongo {
StringData nodeStageTypeToString(const QuerySolutionNode* node) {
    switch (node->getType()) {
        case STAGE_AND_HASH:
            return "AND_HASH"_sd;
        case STAGE_AND_SORTED:
            return "AND_SORTED"_sd;
        case STAGE_BATCHED_DELETE:
            return "BATCHED_DELETE"_sd;
        case STAGE_CACHED_PLAN:
            return "CACHED_PLAN"_sd;
        case STAGE_COLLSCAN: {
            if (static_cast<const CollectionScanNode*>(node)->doClusteredCollectionScanClassic() ||
                static_cast<const CollectionScanNode*>(node)->doClusteredCollectionScanSbe()) {
                return "CLUSTERED_IXSCAN";
            }
            return "COLLSCAN"_sd;
        }
        case STAGE_COUNT:
            return "COUNT"_sd;
        case STAGE_COUNT_SCAN:
            return "COUNT_SCAN"_sd;
        case STAGE_DELETE:
            return "DELETE"_sd;
        case STAGE_DISTINCT_SCAN:
            return "DISTINCT_SCAN"_sd;
        case STAGE_EOF:
            return "EOF"_sd;
        case STAGE_EQ_LOOKUP:
            return "EQ_LOOKUP"_sd;
        case STAGE_EQ_LOOKUP_UNWIND:
            return "EQ_LOOKUP_UNWIND"_sd;
        case STAGE_FETCH:
            return "FETCH"_sd;
        case STAGE_GEO_NEAR_2D:
            return "GEO_NEAR_2D"_sd;
        case STAGE_GEO_NEAR_2DSPHERE:
            return "GEO_NEAR_2DSPHERE"_sd;
        case STAGE_GROUP:
            return "GROUP"_sd;
        case STAGE_IDHACK:
            return "IDHACK"_sd;
        case STAGE_IXSCAN:
            return "IXSCAN"_sd;
        case STAGE_LIMIT:
            return "LIMIT"_sd;
        case STAGE_MATCH:
            return "MATCH"_sd;
        case STAGE_MOCK:
            return "MOCK"_sd;
        case STAGE_MULTI_ITERATOR:
            return "MULTI_ITERATOR"_sd;
        case STAGE_MULTI_PLAN:
            return "MULTI_PLAN"_sd;
        case STAGE_OR:
            return "OR"_sd;
        case STAGE_PROJECTION_DEFAULT:
            return "PROJECTION_DEFAULT"_sd;
        case STAGE_PROJECTION_COVERED:
            return "PROJECTION_COVERED"_sd;
        case STAGE_PROJECTION_SIMPLE:
            return "PROJECTION_SIMPLE"_sd;
        case STAGE_QUEUED_DATA:
            return "QUEUED_DATA"_sd;
        case STAGE_RECORD_STORE_FAST_COUNT:
            return "RECORD_STORE_FAST_COUNT"_sd;
        case STAGE_REPLACE_ROOT:
            return "REPLACE_ROOT"_sd;
        case STAGE_RETURN_KEY:
            return "RETURN_KEY"_sd;
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
            return "SAMPLE_FROM_TIMESERIES_BUCKET"_sd;
        case STAGE_SEARCH:
            return "SEARCH"_sd;
        case STAGE_SHARDING_FILTER:
            return "SHARDING_FILTER"_sd;
        case STAGE_SKIP:
            return "SKIP"_sd;
        case STAGE_SORT_DEFAULT:
            return "SORT"_sd;
        case STAGE_SORT_SIMPLE:
            return "SORT"_sd;
        case STAGE_SORT_KEY_GENERATOR:
            return "SORT_KEY_GENERATOR"_sd;
        case STAGE_SORT_MERGE:
            return "SORT_MERGE"_sd;
        case STAGE_SPOOL:
            return "SPOOL"_sd;
        case STAGE_SUBPLAN:
            return "SUBPLAN"_sd;
        case STAGE_TEXT_OR:
            return "TEXT_OR"_sd;
        case STAGE_TEXT_MATCH:
            return "TEXT_MATCH"_sd;
        case STAGE_TIMESERIES_MODIFY:
            return "TIMESERIES_MODIFY"_sd;
        case STAGE_TRIAL:
            return "TRIAL"_sd;
        case STAGE_UNKNOWN:
            return "UNKNOWN"_sd;
        case STAGE_UNPACK_SAMPLED_TS_BUCKET:
            return "UNPACK_SAMPLED_TS_BUCKET"_sd;
        case STAGE_UNPACK_TS_BUCKET:
            return "UNPACK_TS_BUCKET"_sd;
        case STAGE_UNWIND:
            return "UNWIND"_sd;
        case STAGE_UPDATE:
            return "UPDATE"_sd;
        case STAGE_WINDOW:
            return "WINDOW"_sd;
        default:
            return "UNKNOWN"_sd;
    }
}
}  // namespace mongo
