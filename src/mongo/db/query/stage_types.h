/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

    // Dummy stage used for receiving notifications of deletions during chunk migration.
    STAGE_NOTIFY_DELETE,

    STAGE_ENSURE_SORTED,

    STAGE_EOF,

    // This is more of an "internal-only" stage where we try to keep docs that were mutated
    // during query execution.
    STAGE_KEEP_MUTATIONS,

    STAGE_FETCH,

    // The two $geoNear impls imply a fetch+sort and must be stages.
    STAGE_GEO_NEAR_2D,
    STAGE_GEO_NEAR_2DSPHERE,

    STAGE_GROUP,

    STAGE_IDHACK,

    // Simple wrapper to iterate a SortedDataInterface::Cursor.
    STAGE_INDEX_ITERATOR,

    STAGE_IXSCAN,
    STAGE_LIMIT,

    // Implements parallelCollectionScan.
    STAGE_MULTI_ITERATOR,

    STAGE_MULTI_PLAN,
    STAGE_OPLOG_START,
    STAGE_OR,
    STAGE_PROJECTION,

    // Stage for running aggregation pipelines.
    STAGE_PIPELINE_PROXY,

    STAGE_QUEUED_DATA,
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

    STAGE_UNKNOWN,

    STAGE_UPDATE,
};

}  // namespace mongo
