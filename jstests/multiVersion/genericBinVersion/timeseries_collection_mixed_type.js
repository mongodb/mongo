/**
 * Test that variable-type fields are found correctly in upgraded timeseries collections with dirty
 * data.
 */

(function() {
"use strict";

load('jstests/multiVersion/libs/multi_rs.js');

const timeFieldName = "time";

// Note that this list will need to be kept up to date as versions are added/dropped.
const upgradeVersions = [{binVersion: "6.0", fcv: "6.0"}, {binVersion: "latest"}];

/*
 * Creates a collection, populates it with `docs`, runs the `query` and ensures that the result set
 * is equal to `results`. Also checks that the bounds at `path` formed by the min & max of the
 * control values for the path `path` are equal to `bounds`. In order to do this we assert that only
 * one bucket was created.
 */
function runTest(docs, query, results, path, bounds) {
    const oldVersion = "5.0";
    const nodes = {
        n1: {binVersion: oldVersion},
        n2: {binVersion: oldVersion},
        n3: {binVersion: oldVersion}
    };

    const rst = new ReplSetTest({nodes: nodes});

    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB("test");

    // Setup our DB & our collections.
    let tsColl = db.getCollection(jsTestName());
    tsColl.drop();
    let bucketColl = db.getCollection('system.buckets.' + tsColl.getName());

    assert.commandWorked(
        db.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}));

    // Construct our pipelines for later use.
    const pipeline = [{$match: query}, {$sort: {_id: 1}}, {$project: {_id: 0, time: 0}}];
    const controlPipeline =
        [{$project: {_id: 0, value: [`$control.min.${path}`, `$control.max.${path}`]}}];

    // Populate the collection with documents.
    docs.forEach(d => tsColl.insert(Object.assign({[timeFieldName]: new Date("2021-01-01")}, d)));

    // We may need to upgrade through several versions to reach the latest. This ensures that each
    // subsequent upgrade preserves the mixed-schema data upon upgrading.
    for (const {binVersion, fcv} of upgradeVersions) {
        rst.upgradeSet({binVersion});
        db = rst.getPrimary().getDB("test");

        // Set the fcv if needed.
        if (fcv) {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv}));
            rst.awaitReplication();
        }

        tsColl = db.getCollection(jsTestName());
        bucketColl = db.getCollection('system.buckets.' + tsColl.getName());

        // Confirm expected results.
        assert.docEq(tsColl.aggregate(pipeline).toArray(), results);
        const buckets = bucketColl.aggregate(controlPipeline).toArray();

        // Check that we only have one bucket.
        assert.eq(buckets.length, 1);

        // Check that the bounds are what we expect.
        assert.docEq(buckets[0].value, bounds);
    }

    rst.stopSet();
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

// 'a.b' appears in the bounds which are arrays. But it doesn't appear in every pair of bounds.
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
