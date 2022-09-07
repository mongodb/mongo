/**
 * Tests that $_internalBoundedSort uses the same equality comparator as $sort.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');
load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.bucketUnpackWithSortEnabled(db.getMongo())) {
    jsTestLog("Skipping test because 'BucketUnpackWithSort' is disabled.");
    return;
}

const coll = db.timeseries_internal_bounded_sort_compound_mixed_types;
const buckets = db['system.buckets.' + coll.getName()];
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {
    timeseries: {timeField: 't', metaField: 'm'},
    collation: {locale: 'en_US', strength: 2},
}));
const bucketMaxSpanSeconds =
    db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.bucketMaxSpanSeconds;

// For each interesting value, insert two events:
//   {m: <interesting>, t: <low>}
//   {m: <interesting>, t: <high>}
// The 't' values are globally unique, so that a sort on {m, t} has only one valid outcome.
// This lets us compare naive vs optimized query results directly.
// We insert two events per 'm' value so we can tell whether two 'm' values tied or not:
// the events will be interleaved if (and only if) the 'm' values tied.
{
    const epoch = ISODate('1970-01-01');

    // Pick values with interesting equality behavior.
    let interestingValues = [
        // Strings that differ only by case.
        'a',
        'A',
        'b',
        'B',
        // Arrays whose highest or lowest element is equal.
        [5],
        [5, 99],
        [99],
        // Objects that differ only by field order.
        {x: 1, y: 2},
        {y: 2, x: 1},
        // Numbers that differ only by type.
        7,
        NumberInt(7),
        NumberLong(7),
        NumberDecimal(7),
        // A variety of values that are "empty" somehow.
        // Missing can't be represented as a JS value--handled later.
        null,
        // Undefined is not supported:
        // "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it must not
        //  be the deprecated undefined bson type", code 5369600.
        // undefined,
        [],
        {},
        "",
    ];
    // Also wrap each interesting value in an object or array.
    // Some values that are "equal" at the top level may be distinguished when wrapped this way.
    const arrayWrapped = interestingValues.map(v => [v]);
    const objectWrapped = interestingValues.map(v => ({w: v}));
    interestingValues = interestingValues.concat(arrayWrapped).concat(objectWrapped);

    let docs = [];
    // Each event's _id is an autoincrementing number, and its timestamp is epoch + _id.
    // Run through the interesting values twice to ensure each one has two events.
    // Do this in the outer loop to ensure all the intervals overlap.
    for (const _ of [1, 2]) {
        for (const m of interestingValues) {
            docs.push({_id: docs.length, m, t: new Date(+epoch + docs.length)});
        }
        // Handle 'missing'.
        docs.push({_id: docs.length, t: new Date(+epoch + docs.length)});
    }

    assert.commandWorked(coll.insert(docs));
    printjson(buckets.find({}, {_id: 0, meta: 1}).sort({meta: 1}).toArray());
    const numInterestingValues = 1 + interestingValues.length;  // +1 for missing.
    // Some of these interestingValues may be considered equal for bucketing purposes, so
    // we can get fewer than numInterestingValues buckets. But if we get more buckets than expected,
    // that probably means our timestamps are too far apart, which could lead to this test passing
    // for the wrong reason.
    assert.lte(buckets.count(),
               numInterestingValues,
               `Expected no more than numInterestingValues (${numInterestingValues}) buckets`);
}

const unpackStage = getAggPlanStage(coll.explain().aggregate(), '$_internalUnpackBucket');

function runTest(sortSpec) {
    assert.eq(['m', 't'], Object.keys(sortSpec), 'Expected a compound sort on {m: _, t: _}');
    assert.contains(sortSpec.m, [-1, +1]);
    assert.contains(sortSpec.t, [-1, +1]);

    const naiveQuery = [
        unpackStage,
        {$_internalInhibitOptimization: {}},
        {$sort: sortSpec},
    ];
    const naive = buckets.aggregate(naiveQuery).toArray();

    const optFromMinQuery = [
        {
            $sort: {
                'meta': sortSpec.m,
                'control.min.t': sortSpec.t,
            }
        },
        unpackStage,
        {
            $_internalBoundedSort: {
                sortKey: sortSpec,
                // Use a much looser bound than necessary, to exercise the partitioning logic more.
                // With such a loose bound, events are only released due to a partition boundary,
                // never a bucket boundary.
                bound: {base: "min", offsetSeconds: -sortSpec.t * 10 * bucketMaxSpanSeconds},
            }
        },
    ];
    const optFromMin = buckets.aggregate(optFromMinQuery).toArray();
    assert.eq(naive, optFromMin);

    const optFromMaxQuery = [
        {
            $sort: {
                'meta': sortSpec.m,
                'control.max.t': sortSpec.t,
            }
        },
        unpackStage,
        {
            $_internalBoundedSort: {
                sortKey: sortSpec,
                // Use a much looser bound than necessary, to exercise the partitioning logic more.
                // With such a loose bound, events are only released due to a partition boundary,
                // never a bucket boundary.
                bound: {base: "max", offsetSeconds: -sortSpec.t * 10 * bucketMaxSpanSeconds},
            }
        },
    ];
    const optFromMax = buckets.aggregate(optFromMaxQuery).toArray();
    assert.eq(naive, optFromMax);
}

runTest({m: +1, t: +1});
runTest({m: +1, t: -1});
runTest({m: -1, t: +1});
runTest({m: -1, t: -1});
})();
