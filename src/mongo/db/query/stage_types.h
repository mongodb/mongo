
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

#pragma once

namespace mongo {

/**
 * These map to implementations of the PlanStage interface, all of which live in db/exec/
 */
enum StageType {
    STAGE_AND_HASH,
    STAGE_AND_SORTED,
    STAGE_CACHED_PLAN,
    STAGE_COLLSCAN,

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

    STAGE_ENSURE_SORTED,

    STAGE_EOF,

    STAGE_FETCH,

    // The two $geoNear impls imply a fetch+sort and must be stages.
    STAGE_GEO_NEAR_2D,
    STAGE_GEO_NEAR_2DSPHERE,

    STAGE_IDHACK,

    STAGE_IXSCAN,
    STAGE_LIMIT,

    // Implements iterating over one or more RecordStore::Cursor.
    STAGE_MULTI_ITERATOR,

    STAGE_MULTI_PLAN,
    STAGE_OR,

    // Projection has three alternate implementations.
    STAGE_PROJECTION_DEFAULT,
    STAGE_PROJECTION_COVERED,
    STAGE_PROJECTION_SIMPLE,

    // Stages for running aggregation pipelines.
    STAGE_CHANGE_STREAM_PROXY,
    STAGE_PIPELINE_PROXY,

    STAGE_QUEUED_DATA,
    STAGE_RECORD_STORE_FAST_COUNT,
    STAGE_SHARDING_FILTER,
    STAGE_SKIP,
    STAGE_SORT,
    STAGE_SORT_KEY_GENERATOR,
    STAGE_SORT_MERGE,
    STAGE_SUBPLAN,

    // Stages for running text search.
    STAGE_TEXT,
    STAGE_TEXT_OR,
    STAGE_TEXT_MATCH,

    // Stage for choosing between two alternate plans based on an initial trial period.
    STAGE_TRIAL,

    STAGE_UNKNOWN,

    STAGE_UPDATE,
};

}  // namespace mongo
