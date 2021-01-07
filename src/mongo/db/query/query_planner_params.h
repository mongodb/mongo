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

#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {

struct QueryPlannerParams {
    QueryPlannerParams()
        : options(DEFAULT),
          indexFiltersApplied(false),
          maxIndexedSolutions(internalQueryPlannerMaxIndexedSolutions.load()) {}

    enum Options {
        // You probably want to set this.
        DEFAULT = 0,

        // Set this if you don't want a table scan.
        // See http://docs.mongodb.org/manual/reference/parameters/
        NO_TABLE_SCAN = 1,

        // Set this if you *always* want a collscan outputted, even if there's an ixscan.  This
        // makes ranking less accurate, especially in the presence of blocking stages.
        INCLUDE_COLLSCAN = 1 << 1,

        // Set this if you're running on a sharded cluster.  We'll add a "drop all docs that
        // shouldn't be on this shard" stage before projection.
        //
        // In order to set this, you must check OperationShardingState::isOperationVersioned() in
        // the same lock that you use to build the query executor. You must also wrap the
        // PlanExecutor in a ClientCursor within the same lock.
        //
        // See the comment on ShardFilterStage for details.
        INCLUDE_SHARD_FILTER = 1 << 2,

        // Set this if you want to turn on index intersection.
        INDEX_INTERSECTION = 1 << 3,

        // Indicate to the planner that this query could be eligible for count optimization. For
        // example, the query {$group: {_id: null, sum: {$sum: 1}}} is a count-like operation and
        // could be eligible for the COUNT_SCAN.
        IS_COUNT = 1 << 4,

        // Set this if you want to handle batchSize properly with sort(). If limits on SORT
        // stages are always actually limits, then this should be left off. If they are
        // sometimes to be interpreted as batchSize, then this should be turned on.
        SPLIT_LIMITED_SORT = 1 << 5,

        // Set this to generate covered whole IXSCAN plans.
        GENERATE_COVERED_IXSCANS = 1 << 6,

        // Set this to track the most recent timestamp seen by this cursor while scanning the oplog.
        TRACK_LATEST_OPLOG_TS = 1 << 7,

        // Set this so that collection scans on the oplog wait for visibility before reading.
        OPLOG_SCAN_WAIT_FOR_VISIBLE = 1 << 8,

        // Set this so that getExecutorDistinct() will only use a plan that _guarantees_ it will
        // return exactly one document per value of the distinct field. See the comments above the
        // declaration of getExecutorDistinct() for more detail.
        STRICT_DISTINCT_ONLY = 1 << 9,

        // Instruct the planner that the caller is expecting to consume the record ids associated
        // with documents returned by the plan. Any generated query solution must not discard record
        // ids. In some cases, record ids can be discarded as an optimization when they will not be
        // consumed downstream.
        PRESERVE_RECORD_ID = 1 << 10,

        // Set this on an oplog scan to uassert that the oplog has not already rolled over the
        // minimum 'ts' timestamp specified in the query.
        ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG = 1 << 11,

        // Instruct the plan enumerator to enumerate contained $ors in a special order. $or
        // enumeration can generate an exponential number of plans, and is therefore limited at some
        // arbitrary cutoff controlled by a parameter. When this limit is hit, the order of
        // enumeration is important. For example, a query like the following has a "contained $or"
        // (within an $and):
        // {a: 1, $or: [{b: 1, c: 1}, {b: 2, c: 2}]}
        // For this query if there are indexes a_b={a: 1, b: 1} and a_c={a: 1, c: 1}, the normal
        // enumeration order would output assignments [a_b, a_b], [a_c, a_b], [a_b, a_c], then [a_c,
        // a_c]. This flag will instruct the enumerator to instead prefer a different order. It's
        // hard to summarize, but perhaps the phrases "lockstep enumeration", "simultaneous
        // advancement", or "parallel iteration" will help the reader. The effect is to give earlier
        // enumeration to plans which use the same index of alternative across all branches. In this
        // order, we would get assignments [a_b, a_b], [a_c, a_c], [a_c, a_b], then [a_b, a_c]. This
        // is thought to be helpful in general, but particularly in cases where all children of the
        // $or use the same fields and have the same indexes available, as in this example.
        ENUMERATE_OR_CHILDREN_LOCKSTEP = 1 << 12,

        // Instructs the planner to produce a plan which will *not* check at runtime that the node's
        // replica set member state allows reads. Typically, replica set members will only serve
        // reads to clients if thet are in parimary or secondary state. Client reads are disallowed
        // in other states, e.g. during initial sync. Internal operations, on the other hand, can
        // use this flag to exempt themselves from this repl set note state requirement.
        OMIT_REPL_STATE_PERMITS_READS_CHECK = 1 << 13,
    };

    // See Options enum above.
    size_t options;

    // What indices are available for planning?
    std::vector<IndexEntry> indices;

    // What's our shard key?  If INCLUDE_SHARD_FILTER is set we will create a shard filtering
    // stage.  If we know the shard key, we can perform covering analysis instead of always
    // forcing a fetch.
    BSONObj shardKey;

    // Were index filters applied to indices?
    bool indexFiltersApplied;

    // What's the max number of indexed solutions we want to output?  It's expensive to compare
    // plans via the MultiPlanStage, and the set of possible plans is very large for certain
    // index+query combinations.
    size_t maxIndexedSolutions;
};

}  // namespace mongo
