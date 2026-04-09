/**
 * Validates that plan cache entries exceeding the BSON size limit can still be printed without
 * errors.
 * @tags: [
 *   resource_intensive,
 *   # unfortunately, some suites excludes only query_intensive_pbt, but not resource_intensive
 *   query_intensive_pbt,
 *   do_not_wrap_aggregations_in_facets,
 *   assumes_against_mongod_not_mongos,
 *   # This test attempts to perform queries and introspect the server's plan cache entries. The
 *   # former operation may be routed to a secondary in the replica set, whereas the latter must be
 *   # routed to the primary.
 *   assumes_read_preference_unchanged,
 *   # $planCacheStats cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   # $planCacheStats requires readConcern local and reading from the same node.
 *   assumes_read_concern_local,
 *   assumes_read_concern_unchanged,
 *   # TODO(SERVER-75416) This test should work with timeseries.
 *   exclude_from_timeseries_crud_passthrough,
 *   # Wildcard index creation conflicts with this test.
 *   assumes_no_implicit_index_creation,
 *   # A fix was added in this version to prevent $planCacheStats output from exceeding the BSON size limit for large entries.
 *   requires_fcv_90,
 * ]
 */

import {getPlanCacheKeyFromShape} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

assert.commandWorked(
    coll.insertMany([
        {a: 1, b: 1},
        {a: 2, b: 1},
        {a: 3, b: 2},
    ]),
);

const listSize = 1000000;
const inList = [];
for (let i = 0; i < listSize; i++) {
    inList.push(i + 1);
}

const query = {
    a: {$in: inList},
    b: 1,
};

const planCacheKey = getPlanCacheKeyFromShape({query, collection: coll, db});

// Cache insertion.
for (let i = 0; i < 3; i++) {
    coll.find(query).toArray();
}

// Make sure that we can serialize the plan cache entry.
const planCacheEntries = coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray();
assert.eq(planCacheEntries.length, 1);
