/**
 * Test replanning of distinct-eligible queries.
 * @tags: [
 *   assumes_unsharded_collection,
 *   # Relies on plan cache state persisting across multiple calls
 *   does_not_support_stepdowns,
 *   # Uses indexes not supported on time-series collections
 *   exclude_from_timeseries_crud_passthrough
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("distinct-eligible query replanning", function () {
    const coll = db.distinct_replanning;

    // The $or predicate spans two different fields, so no single index can serve a DISTINCT_SCAN.
    const pipeline = [
        {$match: {$or: [{a: {$eq: 0}}, {"m.m1": {$eq: 0}}]}},
        {$group: {_id: "$a"}},
        {$project: {_id: 0, a: 1}},
    ];

    before(function () {
        coll.drop();
        assert.commandWorked(coll.insert({a: 0, m: {m1: 0}}));

        // Create several indexes so the query has multiple solutions and gets cached.
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1, b: 1, m: "hashed", "m.m1": 1}));
        assert.commandWorked(coll.createIndex({"$**": 1}, {wildcardProjection: {"m.m1": 1}}));
        assert.commandWorked(coll.createIndex({"m.m2": 1, a: 1}));

        // Cache the solution.
        coll.aggregate(pipeline).toArray();
        coll.aggregate(pipeline).toArray();

        // Add more documents so the cached plan is stale and triggers replanning.
        const newDocs = [];
        for (let i = 0; i < 10000; i++) {
            newDocs.push({a: 0, m: {m1: 0}});
        }
        assert.commandWorked(coll.insert(newDocs));
    });

    after(function () {
        coll.drop();
    });

    it("survives a three-step replanning cascade", function () {
        // Expected behaviour:
        // 1. In the first replanning the query is considered eligible for distinct. This fails with
        //    'NoDistinctScansForDistinctEligibleQuery' exception since the $or predicate prevents any
        //    DISTINCT_SCAN.
        // 2. This fallbacks and starts a second replan iteration with distinctEligible = false.
        //    It throws an exception 'ReplanningRequired' due to difference between observed
        //    and expected works. The exception should be caught and a new replanning iteration
        //    should start with information propagated from the exception.
        // 3. The 3rd replanning iteration is with disabled plan cache and starts a fresh multiplanner,
        //    selects a winning plan and adds a new cache entry.
        coll.aggregate(pipeline).toArray();
    });
});
