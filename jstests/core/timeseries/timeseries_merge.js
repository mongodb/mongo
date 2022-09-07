/**
 * Verifies that time-series collections work as expected with $merge.
 *
 *
 * @tags: [
 *   # TimeseriesAggTests doesn't handle stepdowns.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_agg_helpers.js");

const testDB = TimeseriesAggTests.getTestDb();
assert.commandWorked(testDB.dropDatabase());
const numHosts = 10;
const numIterations = 20;

let [inColl, observerInColl] = TimeseriesAggTests.prepareInputCollections(numHosts, numIterations);

// $merge on field requires the output collection to have an unique index on key(s) for "on" field.
function prepareOutputCollectionForMergeOn(outColl) {
    outColl.drop();
    assert.commandWorked(testDB.createCollection(outColl.getName()));

    assert.commandWorked(outColl.createIndex({"tags.hostid": 1}, {unique: true}));
}

/**
 * Verifies that simple $merge works as expected with time-series source collection.
 */
let runSimpleMergeTestCase = () => {
    // Gets the expected results from non time-series observer input collection.
    let expectedResults = TimeseriesAggTests.getOutputAggregateResults(
        observerInColl, [{$merge: {into: "observer_out"}}]);

    // Gets the actual results from time-series input collection.
    let actualResults =
        TimeseriesAggTests.getOutputAggregateResults(inColl, [{$merge: {into: "out"}}]);

    // Verifies that the number of measurements is same as expected.
    assert.eq(actualResults.length, expectedResults.length, actualResults);

    // Verifies that every measurement is same as expected.
    for (var i = 0; i < expectedResults.length; ++i) {
        assert.eq(actualResults[i], expectedResults[i], actualResults);
    }
};

let runMergeOnErrorTestCase = () => {
    var outColl = TimeseriesAggTests.getOutputCollection("out");
    prepareOutputCollectionForMergeOn(outColl);

    // This must fail because source '_id' field will try to replace target '_id' field which is
    // immutable. This verifies that source '_id' is materialized.
    jsTestLog("'ImmutableField' error expected below!");
    var err = assert.throws(() => inColl.aggregate([{
        $merge: {
            into: outColl.getName(),
            on: "tags.hostid",
            whenMatched: "replace",
        }
    }]));
    assert.commandFailedWithCode(err, ErrorCodes.ImmutableField);
};

/**
 * Verifies that $merge "on" field works as expected with time-series source collection.
 */
let runMergeOnTestCase = () => {
    var mergePipeline = [
        {$project: {_id: 0, cpu: 1, idle_user: 1, "tags.hostid": 1, time: 1}},
        {$sort: {time: 1}},
        {
            $merge: {
                into: "observer_out",
                on: "tags.hostid",
                whenMatched: "merge",
            }
        }
    ];

    // Gets the expected results from non time-series observer input collection.
    let expectedResults = TimeseriesAggTests.getOutputAggregateResults(
        observerInColl,
        mergePipeline,
        /*prepareAction*/ (outColl) => prepareOutputCollectionForMergeOn(outColl));

    // Gets the actual results from time-series input collection.
    mergePipeline[mergePipeline.length - 1]["$merge"].into = "out";
    let actualResults = TimeseriesAggTests.getOutputAggregateResults(
        inColl,
        mergePipeline,
        /*prepareAction*/ (outColl) => prepareOutputCollectionForMergeOn(outColl));

    // Verifies that the number of measurements is same as expected.
    assert.eq(actualResults.length, expectedResults.length, actualResults);

    // Verifies that every measurement is same as expected.
    for (var i = 0; i < expectedResults.length; ++i) {
        assert.eq(actualResults[i], expectedResults[i], actualResults);
    }
};

runSimpleMergeTestCase();
runMergeOnErrorTestCase();
runMergeOnTestCase();
})();
