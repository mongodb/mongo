/**
 * Verifies that time-series collections work as expected with $out.
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

// Gets the expected results from non time-series observer input collection.
let expectedResults =
    TimeseriesAggTests.getOutputAggregateResults(observerInColl, [{$out: "observer_out"}]);

// Gets the actual results from time-series input collection.
let actualResults = TimeseriesAggTests.getOutputAggregateResults(inColl, [{$out: "out"}]);

// Verifies that the number of measurements is same as expected.
assert.eq(actualResults.length, expectedResults.length, actualResults);

// Verifies that every measurement is same as expected.
for (var i = 0; i < expectedResults.length; ++i) {
    assert.eq(actualResults[i], expectedResults[i], actualResults);
}
})();
