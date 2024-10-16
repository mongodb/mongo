/**
 * Tests the SBE stage builder's ability handle queries where the planner generates plans containing
 * a union of IXSCAN stages and a CLUSTERED_IDXSCAN stage.
 *
 * Technically this issue is not specific to timeseries, but we use timeseries collections in this
 * test for convenience to get the planner generate the kinds of plans we want to test.
 *
 * @tags: [
 *   # Requires a timeseries collection.
 *   requires_timeseries,
 *   # The test assumes no index exists on the time field. shardCollection implicitly creates an
 *   # index.
 *   assumes_unsharded_collection,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const datePrefix = 1680912440;

    let coll = db[jsTestName()];
    coll.drop();

    // Create a timeseries collection with some documents.
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: 'time', metaField: 'measurement'},
    }));

    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({x: 1, time: 1}));

    insert(coll, {_id: 0, time: new Date(datePrefix + 100), measurement: 0});
    insert(coll, {_id: 1, time: new Date(datePrefix + 150), measurement: 1});
    insert(coll, {_id: 2, time: new Date(datePrefix + 200), measurement: 2});
    insert(coll, {_id: 3, time: new Date(datePrefix + 250), measurement: 3, x: 4});
    insert(coll, {_id: 4, time: new Date(datePrefix + 300), measurement: 4});
    insert(coll, {_id: 5, time: new Date(datePrefix + 350), measurement: 5});
    insert(coll, {_id: 6, time: new Date(datePrefix + 400), measurement: 6});
    insert(coll, {_id: 7, time: new Date(datePrefix + 450), measurement: 7, x: 1});
    insert(coll, {_id: 8, time: new Date(datePrefix + 500), measurement: 8});
    insert(coll, {_id: 9, time: new Date(datePrefix + 550), measurement: 9, x: 5});

    function compareResultEntries(lhs, rhs) {
        const lhsJson = tojson(lhs);
        const rhsJson = tojson(rhs);
        return lhsJson < rhsJson ? -1 : (lhsJson > rhsJson ? 1 : 0);
    }

    // Create a list of testcases where the planner would typically generate a QSN tree containing a
    // union of IXSCAN stages and a CLUSTERED_IDXSCAN stage that looks something like this:
    //   OR
    //   --OR
    //   ----IXSCAN
    //   ----IXSCAN
    //   ----..
    //   --CLUSTERED_IDXSCAN

    const filter = {
        $or: [
            {time: {$lte: new Date(datePrefix + 100)}},
            {$and: [{x: {$lt: 3}}, {x: {$in: [1, 2, 4]}}]}
        ]
    };

    const testcases = [
        {pipeline: [{$match: filter}, {$project: {_id: 0, x: 1}}], expected: [{x: 1}, {}]},
        {pipeline: [{$match: filter}, {$project: {_id: 0, x: "$x"}}], expected: [{x: 1}, {}]},
        {
            pipeline: [{$match: filter}, {$group: {_id: null, count: {$sum: NumberInt(1)}}}],
            expected: [{_id: null, count: NumberInt(2)}]
        },
        {
            pipeline: [{$match: filter}, {$project: {_id: 0, time: 1}}],
            expected: [{time: new Date(datePrefix + 100)}, {time: new Date(datePrefix + 450)}]
        },
        {
            pipeline: [{$match: filter}, {$project: {_id: 0, time: "$time"}}],
            expected: [{time: new Date(datePrefix + 100)}, {time: new Date(datePrefix + 450)}]
        },
        {
            pipeline: [
                {$match: filter},
                {$group: {_id: null, count: {$sum: NumberInt(1)}, x: {$max: "$x"}}}
            ],
            expected: [{_id: null, count: NumberInt(2), x: 1}]
        },
        {
            pipeline: [{$match: filter}, {$group: {_id: null, time: {$max: "$time"}}}],
            expected: [{_id: null, time: new Date(datePrefix + 450)}]
        },
    ];

    // Execute all the testcases.
    for (const testcase of testcases) {
        const pipeline = testcase.pipeline;
        const expected = testcase.expected;

        assert.eq(coll.aggregate(pipeline).toArray().sort(compareResultEntries), expected);
    }
});
