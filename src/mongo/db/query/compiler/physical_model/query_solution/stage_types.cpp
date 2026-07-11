// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
std::string_view nodeStageTypeToString(const QuerySolutionNode* node) {
    switch (node->getType()) {
        case STAGE_AND_HASH:
            return "AND_HASH"sv;
        case STAGE_AND_SORTED:
            return "AND_SORTED"sv;
        case STAGE_BATCHED_DELETE:
            return "BATCHED_DELETE"sv;
        case STAGE_CACHED_PLAN:
            return "CACHED_PLAN"sv;
        case STAGE_COLLSCAN: {
            if (static_cast<const CollectionScanNode*>(node)->doClusteredCollectionScanClassic() ||
                static_cast<const CollectionScanNode*>(node)->doClusteredCollectionScanSbe()) {
                return "CLUSTERED_IXSCAN";
            }
            return "COLLSCAN"sv;
        }
        case STAGE_COUNT:
            return "COUNT"sv;
        case STAGE_COUNT_SCAN:
            return "COUNT_SCAN"sv;
        case STAGE_DELETE:
            return "DELETE"sv;
        case STAGE_DISTINCT_SCAN:
            return "DISTINCT_SCAN"sv;
        case STAGE_EOF:
            return "EOF"sv;
        case STAGE_EQ_LOOKUP:
            return "EQ_LOOKUP"sv;
        case STAGE_EQ_LOOKUP_UNWIND:
            return "EQ_LOOKUP_UNWIND"sv;
        case STAGE_FETCH:
            return "FETCH"sv;
        case STAGE_GEO_NEAR_2D:
            return "GEO_NEAR_2D"sv;
        case STAGE_GEO_NEAR_2DSPHERE:
            return "GEO_NEAR_2DSPHERE"sv;
        case STAGE_GROUP:
            return "GROUP"sv;
        case STAGE_IDHACK:
            return "IDHACK"sv;
        case STAGE_IXSCAN:
            return "IXSCAN"sv;
        case STAGE_LIMIT:
            return "LIMIT"sv;
        case STAGE_MATCH:
            return "MATCH"sv;
        case STAGE_MOCK:
            return "MOCK"sv;
        case STAGE_MULTI_ITERATOR:
            return "MULTI_ITERATOR"sv;
        case STAGE_MULTI_PLAN:
            return "MULTI_PLAN"sv;
        case STAGE_OR:
            return "OR"sv;
        case STAGE_PROJECTION_DEFAULT:
            return "PROJECTION_DEFAULT"sv;
        case STAGE_PROJECTION_COVERED:
            return "PROJECTION_COVERED"sv;
        case STAGE_PROJECTION_SIMPLE:
            return "PROJECTION_SIMPLE"sv;
        case STAGE_QUEUED_DATA:
            return "QUEUED_DATA"sv;
        case STAGE_RECORD_STORE_FAST_COUNT:
            return "RECORD_STORE_FAST_COUNT"sv;
        case STAGE_REPLACE_ROOT:
            return "REPLACE_ROOT"sv;
        case STAGE_RETURN_KEY:
            return "RETURN_KEY"sv;
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
            return "SAMPLE_FROM_TIMESERIES_BUCKET"sv;
        case STAGE_SEARCH:
            return "SEARCH"sv;
        case STAGE_SHARDING_FILTER:
            return "SHARDING_FILTER"sv;
        case STAGE_SKIP:
            return "SKIP"sv;
        case STAGE_SORT_DEFAULT:
            return "SORT"sv;
        case STAGE_SORT_SIMPLE:
            return "SORT"sv;
        case STAGE_SORT_KEY_GENERATOR:
            return "SORT_KEY_GENERATOR"sv;
        case STAGE_SORT_MERGE:
            return "SORT_MERGE"sv;
        case STAGE_SPOOL:
            return "SPOOL"sv;
        case STAGE_SUBPLAN:
            return "SUBPLAN"sv;
        case STAGE_TEXT_OR:
            return "TEXT_OR"sv;
        case STAGE_TEXT_MATCH:
            return "TEXT_MATCH"sv;
        case STAGE_TIMESERIES_MODIFY:
            return "TIMESERIES_MODIFY"sv;
        case STAGE_TRIAL:
            return "TRIAL"sv;
        case STAGE_UNKNOWN:
            return "UNKNOWN"sv;
        case STAGE_UNPACK_SAMPLED_TS_BUCKET:
            return "UNPACK_SAMPLED_TS_BUCKET"sv;
        case STAGE_UNPACK_TS_BUCKET:
            return "UNPACK_TS_BUCKET"sv;
        case STAGE_UNWIND:
            return "UNWIND"sv;
        case STAGE_UPDATE:
            return "UPDATE"sv;
        case STAGE_WINDOW:
            return "WINDOW"sv;
        case STAGE_HASH_JOIN_EMBEDDING_NODE:
            return "HASH_JOIN_EMBEDDING"sv;
        case STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE:
            return "NESTED_LOOP_JOIN_EMBEDDING"sv;
        case STAGE_INDEXED_NESTED_LOOP_JOIN_EMBEDDING_NODE:
            return "INDEXED_NESTED_LOOP_JOIN_EMBEDDING"sv;
        case STAGE_INDEX_PROBE_NODE:
            return "INDEX_PROBE_NODE"sv;
        default:
            return "UNKNOWN"sv;
    }
}
}  // namespace mongo
