/**
 * Test that variable-type fields are found correctly in timeseries collections.
 *
 * @tags: [
 *     does_not_support_transactions,
 *     does_not_support_stepdowns,
 *     requires_pipeline_optimization,
 *     requires_timeseries,
 *     # Tenant migration may cause they test case to fail as it could lead to bucket splitting.
 *     tenant_migration_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const timeFieldName = "time";

/*
 * Creates a collection, populates it with `docs`, runs the `query` and ensures that the result set
 * is equal to `results`. Also checks that the bounds at `path` formed by the min & max of the
 * control values for the path `path` are equal to `bounds`. In order to do this we assert that only
 * one bucket was created.
 */
function runTest(docs, query, results, path, bounds) {
    // Setup our DB & our collections.
    const tsColl = db.getCollection(jsTestName());
    tsColl.drop();
    const bucketColl = db.getCollection('system.buckets.' + tsColl.getName());

    assert.commandWorked(
        db.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}));

    // Construct our pipelines for later use.
    const pipeline = [{$match: query}, {$sort: {_id: 1}}, {$project: {_id: 0, time: 0}}];
    const controlPipeline =
        [{$project: {_id: 0, value: [`$control.min.${path}`, `$control.max.${path}`]}}];

    // Populate the collection with documents.
    docs.forEach(d => tsColl.insert(Object.assign({[timeFieldName]: new Date("2021-01-01")}, d)));

    // Check that the result is in the result set.
    assert.docEq(tsColl.aggregate(pipeline).toArray(), results);
    const buckets = bucketColl.aggregate(controlPipeline).toArray();

    // Check that we only have one bucket.
    assert.eq(buckets.length, 1);

    // Check that the bounds are what we expect.
    assert.docEq(buckets[0].value, bounds);
}

// 'a.b' is missing in the bounds even though it appears in the events.
runTest([{a: 1}, {a: {b: 3}}, {a: new Date("2021-01-01")}],
        {"a.b": {$gt: 2}},
        [{a: {b: 3}}],
        "a",
        [1, new Date("2021-01-01")]);

// 'a.b' is missing in the bounds even though it appears in the events. The bounds it is missing
// in are arrays on both sides.
runTest([{a: [1]}, {a: [{b: 3}]}, {a: [new Date("2021-01-01")]}],
        {"a.b": {$gt: 2}},
        [{a: [{b: 3}]}],
        "a",
        [[1], [new Date("2021-01-01")]]);

// 'a.b' appears in the bounds which are arrays. But it doesn't appear not in every pair of bounds.
// And the relevant value of 'a.b' does not appear in the bounds despite being present in the
// events.
runTest([{a: [1]}, {a: [{b: 3}]}, {a: [new Date("2021-01-01"), {b: 1}]}],
        {"a.b": {$gt: 2}},
        [{a: [{b: 3}]}],
        "a",
        [[1, {b: 1}], [new Date("2021-01-01"), {b: 1}]]);

// 'a.b' appears in the bounds but not the relevant side of the bounds.
runTest(
    [
        {a: 1},
        {a: {b: 1}},
    ],
    {"a.b": {$lt: 2}},
    [{a: {b: 1}}],
    "a",
    [1, {b: 1}]);

// We query the upper bound for 'a.b', but 'a.b' only appears in the lower bound.
runTest(
    [
        {a: {b: 3}},
        {a: {b: [1, 2]}},
    ],
    {"a.b": {$gte: 3}},
    [{a: {b: 3}}],
    "a.b",
    [3, [1, 2]]);

// We query the lower bound for 'a.b', but 'a.b' only appears in the upper bound.
runTest(
    [
        {a: {b: 4}},
        {a: {b: [1, 2]}},
    ],
    {"a.b": {$lte: 3}},
    [{a: {b: [1, 2]}}],
    "a.b",
    [4, [1, 2]]);

// 'a.b' appears in the bounds but the matching values appear in neither side of the bounds.
runTest([{a: {b: 3}}, {a: {b: [1, 2]}}, {a: new Date("2021-01-01")}],
        {"a.b": {$eq: 2}},
        [{a: {b: [1, 2]}}],
        "a",
        [{b: 3}, new Date("2021-01-01")]);

// 'a.0' doesn't appear in the bounds.
runTest(
    [
        {a: 1.7881393632457332e-7},
        {
            a: ["ya", {"b": "/lc/"}, 0.9999999701976788, 0.3044235921021081],
        },
        {a: true}
    ],
    {"a.0": {$gte: ""}},
    [{a: ["ya", {"b": "/lc/"}, 0.9999999701976788, 0.3044235921021081]}],
    "a",
    [1.7881393632457332e-7, true]);

// We test arrays wrapping objects and objects wrapping arrays as different ways of achieving
// multiple bounds on 'a.b'.
runTest([{a: {b: [3, 4]}}, {a: [{b: 1}, {b: 2}]}],
        {"a.b": {$lt: 2}},
        [{a: [{b: 1}, {b: 2}]}],
        "a",
        [{b: [3, 4]}, [{b: 1}, {b: 2}]]);
runTest([{a: {b: [3, 4]}}, {a: [{b: 1}, {b: 2}]}],
        {"a.b": {$gte: 3}},
        [{a: {b: [3, 4]}}],
        "a",
        [{b: [3, 4]}, [{b: 1}, {b: 2}]]);
})();
