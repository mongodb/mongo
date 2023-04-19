/**
 * Verifies that time-series collections work as expected with $out.
 *
 *
 * @tags: [
 *   # TimeseriesAggTests doesn't handle stepdowns.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # TODO SERVER-74601 remove tag after support for secondaries.
 *   assumes_read_preference_unchanged,
 *   # TODO SERVER-74601 remove tag after support for sharded clusters.
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_71
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

function generateOutPipeline(collName, options, aggStage = null) {
    let outStage = {$out: {db: testDB.getName(), coll: collName, timeseries: options}};
    if (aggStage) {
        return [aggStage, outStage];
    }
    return [outStage];
}

function runTest(observerPipeline, actualPipeline, shouldDrop = true, valueToCheck = null) {
    // Gets the expected results from a non time-series observer input collection.
    const expectedResults = TimeseriesAggTests.getOutputAggregateResults(
        observerInColl, observerPipeline, null, shouldDrop);

    // Gets the actual results from a time-series input collection.
    const actualResults =
        TimeseriesAggTests.getOutputAggregateResults(inColl, actualPipeline, null, shouldDrop);

    // Verifies that the number of measurements is same as expected.
    TimeseriesAggTests.verifyResults(actualResults, expectedResults);
    if (valueToCheck) {
        for (var i = 0; i < expectedResults.length; ++i) {
            assert.eq(actualResults[i], {"time": valueToCheck}, actualResults);
        }
    }
}

//  Tests that $out works with time-series collections writing to a non-timeseries collection.
runTest([{$out: "observer_out"}], [{$out: "out"}]);

//  Tests that $out creates a time-series collection when the collection does not exist.
let actualPipeline = generateOutPipeline("out_time", {timeField: "time", metaField: "tags"});
runTest([{$out: "observer_out"}], actualPipeline);

// Tests that $out creates a time-series collection with more time-series options.
actualPipeline = generateOutPipeline(
    "out_time",
    {timeField: "time", metaField: "tags", bucketMaxSpanSeconds: 100, bucketRoundingSeconds: 100});
runTest([{$out: "observer_out"}], actualPipeline);

// Change an option in the existing time-series collections.
assert.commandWorked(testDB.runCommand({collMod: "out_time", expireAfterSeconds: 360}));
assert.commandWorked(testDB.runCommand({collMod: "observer_out", validationLevel: "moderate"}));

// Tests that a time-series collection is replaced when a time-series collection does exist.
let newDate = new Date('1999-09-30T03:24:00');
let observerPipeline = [{$set: {"time": newDate}}, {$out: "observer_out"}];
actualPipeline = generateOutPipeline("out_time", {timeField: "time"}, {$set: {"time": newDate}});

// Confirms that all the documents have the 'newDate' value.
runTest(observerPipeline, actualPipeline, false, newDate);

// Confirms that the original time-series collection options were preserved by $out.
let collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
let coll = collections.find(entry => entry.name === "out_time");
assert.eq(coll["options"]["expireAfterSeconds"], 360);
coll = collections.find(entry => entry.name === "observer_out");
assert.eq(coll["options"]["validationLevel"], "moderate");

// Tests that an error is raised when trying to create a time-series collection from a non
// time-series collection.
let pipeline = generateOutPipeline("observer_out", {timeField: "time"});
assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268700);
assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268700);

// Tests that an error is raised for invalid timeseries options.
pipeline = generateOutPipeline("out_time", {timeField: "time", invalidField: "invalid"});
assert.throwsWithCode(() => inColl.aggregate(pipeline), 40415);
assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 40415);

// Tests that an error is raised if the time-series specification changes.
pipeline = generateOutPipeline("out_time", {timeField: "usage_guest_nice"});
assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268701);
assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268701);

pipeline = generateOutPipeline("out_time", {timeField: "time", metaField: "usage_guest_nice"});
assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268702);
assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268702);

// Tests that a time-series collection can be replaced with a non-timeseries collection.
runTest([{"$out": "observer_out_time"}], [{"$out": "out_time"}]);

// Tests that an error is raised if a conflicting view exists.
assert.commandWorked(testDB.createCollection("view_out", {viewOn: "out"}));

pipeline = generateOutPipeline("view_out", {timeField: "time"});
assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268703);
assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268703);

// Test $out for time-series works with a non-existent database.
const destDB = testDB.getSiblingDB("outDifferentDB");
assert.commandWorked(destDB.dropDatabase());
inColl.aggregate({$out: {db: destDB.getName(), coll: "out_time", timeseries: {timeField: "time"}}});
assert.eq(300, destDB["out_time"].find().itcount());
})();
