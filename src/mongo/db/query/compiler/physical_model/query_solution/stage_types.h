// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

namespace mongo {
/**
 * This type acts as an identifier for a node in a query plan tree, such as a 'QuerySolution' tree
 * or an 'sbe::PlanStage' tree.
 *
 * An id of 0 is used to represent the absence of an explicitly assigned id.
 */
using PlanNodeId = uint32_t;
static constexpr PlanNodeId kEmptyPlanNodeId = 0u;

/**
 * These map to implementations of the PlanStage interface, all of which live in db/exec/. These
 * stage types are shared between Classic and SBE.
 */
enum [[MONGO_MOD_NEEDS_REPLACEMENT]] StageType {
    STAGE_AND_HASH,
    STAGE_AND_SORTED,
    STAGE_BATCHED_DELETE,
    STAGE_CACHED_PLAN,

    STAGE_COLLSCAN,
    STAGE_COLLSCAN_MULTI_RANGE,

    // A virtual scan stage that simulates a collection scan and doesn't depend on underlying
    // storage.
    STAGE_VIRTUAL_SCAN,

    // This stage sits at the root of the query tree and counts up the number of results
    // returned by its child.
    STAGE_COUNT,

    // If we're running a .count(), the query is fully covered by one ixscan, and the ixscan is
    // from one key to another, we can just skip through the keys without bothering to examine
    // them.
    STAGE_COUNT_SCAN,

    STAGE_DELETE,

    // If we're running a distinct, we only care about one value for each key.  The distinct
    // scan stage is an ixscan with some key-skipping behvaior that only distinct uses.
    STAGE_DISTINCT_SCAN,

    STAGE_EOF,

    STAGE_FETCH,

    // The two $geoNear impls imply a fetch+sort and must be stages.
    STAGE_GEO_NEAR_2D,
    STAGE_GEO_NEAR_2DSPHERE,

    STAGE_IDHACK,

    STAGE_IXSCAN,
    STAGE_LIMIT,
    STAGE_MATCH,
    STAGE_MOCK,

    // Implements iterating over one or more RecordStore::Cursor.
    STAGE_MULTI_ITERATOR,

    STAGE_MULTI_PLAN,
    STAGE_OR,

    // Projection has three alternate implementations.
    STAGE_PROJECTION_DEFAULT,
    STAGE_PROJECTION_COVERED,
    STAGE_PROJECTION_SIMPLE,

    STAGE_QUEUED_DATA,
    STAGE_RECORD_STORE_FAST_COUNT,
    STAGE_REPLACE_ROOT,
    STAGE_RETURN_KEY,
    STAGE_SAMPLE_FROM_TIMESERIES_BUCKET,
    STAGE_SHARDING_FILTER,
    STAGE_SKIP,

    STAGE_SORT_DEFAULT,
    STAGE_SORT_SIMPLE,
    STAGE_SORT_KEY_GENERATOR,

    STAGE_SORT_MERGE,

    STAGE_SPOOL,

    STAGE_SUBPLAN,

    // Stages for running text search.
    STAGE_TEXT_OR,
    STAGE_TEXT_MATCH,

    // Stage for modifying bucket documents in a time-series bucket collection.
    STAGE_TIMESERIES_MODIFY,

    // Stage for choosing between two alternate plans based on an initial trial period.
    STAGE_TRIAL,

    STAGE_UNKNOWN,

    // Stage for 'UnpackTimeseriesBucket' which is only used for $sample on a time-series bucket
    // collection.
    STAGE_UNPACK_SAMPLED_TS_BUCKET,

    STAGE_UNWIND,
    STAGE_UPDATE,

    // Stages for DocumentSources.
    STAGE_GROUP,
    STAGE_EQ_LOOKUP,
    STAGE_EQ_LOOKUP_UNWIND,
    STAGE_SEARCH,
    STAGE_WINDOW,
    STAGE_SENTINEL,
    STAGE_HASH_JOIN_EMBEDDING_NODE,
    STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE,
    STAGE_INDEXED_NESTED_LOOP_JOIN_EMBEDDING_NODE,
    STAGE_INDEX_PROBE_NODE,
    // Stage for the DocumentSource to unpack timeseries buckets.
    STAGE_UNPACK_TS_BUCKET,
};

inline bool isProjectionStageType(StageType stageType) {
    switch (stageType) {
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_SIMPLE:
            return true;
        default:
            return false;
    }
}

inline bool isSortStageType(StageType stageType) {
    switch (stageType) {
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE:
            return true;
        default:
            return false;
    }
}

struct QuerySolutionNode;

std::string_view nodeStageTypeToString(const QuerySolutionNode* node);

}  // namespace mongo
