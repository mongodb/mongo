/**
 * Test $natural hint on a time-series collection, for find and aggregate.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may
 *   # return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # Pipeline optimization required to get expected explain output
 *   requires_pipeline_optimization,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

const coll = db.timeseries_hint;
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {
    timeseries: {timeField: 't', metaField: 'm'},
}));

// With only one event per bucket, the order of results will be predictable.
// The min time of each bucket is encoded in its _id, and the clustered collection
// ensures that $natural order is also _id order.
const docsAsc = [
    {_id: 1, m: 1, t: ISODate('1970-01-01')},
    {_id: 2, m: 2, t: ISODate('1970-01-02')},
    {_id: 3, m: 3, t: ISODate('1970-01-03')},
];
const docsDesc = docsAsc.slice().reverse();
assert.commandWorked(coll.insert(docsAsc));

function runTest({command, expectedResult, expectedDirection}) {
    const result = assert.commandWorked(db.runCommand(command));
    assert.docEq(result.cursor.firstBatch, expectedResult);

    const plan = db.runCommand({explain: command});
    const scan = getAggPlanStage(plan, 'COLLSCAN');
    assert(scan, 'Expected a COLLSCAN stage' + tojson(plan));
    assert.eq(scan.direction,
              expectedDirection,
              'Expected a ' + expectedDirection + ' COLLSCAN ' + tojson(scan));
}

// Test find: ascending and descending.
runTest({
    command: {
        find: coll.getName(),
        filter: {},
        hint: {$natural: 1},
    },
    expectedResult: docsAsc,
    expectedDirection: 'forward',
});
runTest({
    command: {
        find: coll.getName(),
        filter: {},
        hint: {$natural: -1},
    },
    expectedResult: docsDesc,
    expectedDirection: 'backward',
});

// Test aggregate: ascending and descending.
runTest({
    command: {
        aggregate: coll.getName(),
        pipeline: [],
        cursor: {},
        hint: {$natural: 1},
    },
    expectedResult: docsAsc,
    expectedDirection: 'forward',
});
runTest({
    command: {
        aggregate: coll.getName(),
        pipeline: [],
        cursor: {},
        hint: {$natural: -1},
    },
    expectedResult: docsDesc,
    expectedDirection: 'backward',
});
})();
