/**
 * Test that variable-type fields are found correctly in timeseries collections.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Requires pipeline optimization to run in order to produce expected explain output
 *   requires_pipeline_optimization,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.isMongos().

const timeFieldName = "time";

/*
 * Creates a collection, populates it with `docs`, runs the `query` and ensures that the result set
 * is equal to `results`.
 */
function runTest(docs, query, results) {
    // Setup our DB & our collections.
    const tsColl = db.getCollection(jsTestName());
    tsColl.drop();
    const bucketColl = db.getCollection('system.buckets.' + tsColl.getName());

    assert.commandWorked(
        db.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}));

    // Construct our pipelines for later use.
    const pipeline = [{$match: query}, {$sort: {_id: 1}}, {$project: {_id: 0, time: 0}}];

    // Populate the collection with documents.
    docs.forEach(d => tsColl.insert(Object.assign({[timeFieldName]: new Date("2021-01-01")}, d)));

    // Check that the result is in the result set.
    assert.docEq(tsColl.aggregate(pipeline).toArray(), results);

    // Ensure $type operator was not used.
    const explain = tsColl.explain().aggregate(pipeline);
    const noTypeExpression = (obj) => {
        if (typeof obj === 'object' && obj != null) {
            assert(!Object.keys(obj).includes("$type"));
            for (const [k, v] of Object.entries(obj))
                noTypeExpression(v);
        } else if (Array.isArray(obj)) {
            for (const member of obj) {
                noTypeExpression(obj);
            }
        }
    };
    if (!FixtureHelpers.isMongos(db)) {
        assert(Array.isArray(explain.stages), explain);
        noTypeExpression(explain.stages);
    } else {
        if (explain.splitPipeline) {
            assert(Array.isArray(explain.splitPipeline), explain);
            noTypeExpression(explain.splitPipeline);
        } else if (explain.stages) {
            assert(Array.isArray(explain.stages), explain);
            noTypeExpression(explain.stages);
        } else {
            const firstShardName = Object.getOwnPropertyNames(explain.shards)[0];
            const firstShard = explain.shards[firstShardName];
            assert(Array.isArray(firstShard.stages));
            noTypeExpression(firstShard.stages);
        }
    }
}

// 'a.b' is missing in the bounds even though it appears in the events.
runTest([{a: 1}, {a: {b: 3}}, {a: new Date("2021-01-01")}], {"a.b": {$gt: 2}}, [{a: {b: 3}}]);

// 'a.b' is missing in the bounds even though it appears in the events. The bounds it is missing
// in are arrays on both sides.
runTest(
    [{a: [1]}, {a: [{b: 3}]}, {a: [new Date("2021-01-01")]}], {"a.b": {$gt: 2}}, [{a: [{b: 3}]}]);

// 'a.b' appears in the bounds which are arrays. But it doesn't appear not in every pair of bounds.
// And the relevant value of 'a.b' does not appear in the bounds despite being present in the
// events.
runTest([{a: [1]}, {a: [{b: 3}]}, {a: [new Date("2021-01-01"), {b: 1}]}],
        {"a.b": {$gt: 2}},
        [{a: [{b: 3}]}]);

// 'a.b' appears in the bounds but not the relevant side of the bounds.
runTest(
    [
        {a: 1},
        {a: {b: 1}},
    ],
    {"a.b": {$lt: 2}},
    [{a: {b: 1}}]);

// We query the upper bound for 'a.b', but 'a.b' only appears in the lower bound.
runTest(
    [
        {a: {b: 3}},
        {a: {b: [1, 2]}},
    ],
    {"a.b": {$gte: 3}},
    [{a: {b: 3}}]);

// We query the lower bound for 'a.b', but 'a.b' only appears in the upper bound.
runTest(
    [
        {a: {b: 4}},
        {a: {b: [1, 2]}},
    ],
    {"a.b": {$lte: 3}},
    [{a: {b: [1, 2]}}]);

// 'a.b' appears in the bounds but the matching values appear in neither side of the bounds.
runTest([{a: {b: 3}}, {a: {b: [1, 2]}}, {a: new Date("2021-01-01")}],
        {"a.b": {$eq: 2}},
        [{a: {b: [1, 2]}}]);

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
    [{a: ["ya", {"b": "/lc/"}, 0.9999999701976788, 0.3044235921021081]}]);

// We test arrays wrapping objects and objects wrapping arrays as different ways of achieving
// multiple bounds on 'a.b'.
runTest([{a: {b: [3, 4]}}, {a: [{b: 1}, {b: 2}]}], {"a.b": {$lt: 2}}, [{a: [{b: 1}, {b: 2}]}]);
runTest([{a: {b: [3, 4]}}, {a: [{b: 1}, {b: 2}]}], {"a.b": {$gte: 3}}, [{a: {b: [3, 4]}}]);
})();
