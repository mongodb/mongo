/*
 * Tests time-series server parameter settings on server startup.

 * @tags: [
 *   requires_replication
 * ]
 */

(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/noPassthrough/libs/server_parameter_helpers.js");

// Test that collection clustering can be disabled on the buckets collection, and that it behaves
// correctly in a replicated environment.
(() => {
    const replSet = new ReplSetTest({
        nodes: 2,
        nodeOptions: {setParameter: {timeseriesBucketsCollectionClusterById: false}},
    });
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    if (!TimeseriesTest.timeseriesCollectionsEnabled(primary)) {
        jsTestLog("Skipping test case because time-series collections are not enabled.");
        replSet.stopSet();
        return;
    }

    const testDB = primary.getDB('test');
    assert.commandWorked(testDB.createCollection('ts', {timeseries: {timeField: 'time'}}));
    testDB.ts.insert({time: new Date()});

    let res = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {name: 'system.buckets.ts'}}));
    let options = res.cursor.firstBatch[0].options;
    assert(!options.clusteredIndex);

    replSet.stopSet();
})();

// Valid parameter values are in the range [0, infinity).
testNumericServerParameter('timeseriesBucketMaxCount',
                           true /*isStartupParameter*/,
                           false /*isRuntimeParameter*/,
                           1000 /*defaultValue*/,
                           100 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           0 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);

// Valid parameter values are in the range [0, infinity).
testNumericServerParameter('timeseriesBucketMaxSize',
                           true /*isStartupParameter*/,
                           false /*isRuntimeParameter*/,
                           1024 * 125 /*defaultValue*/,
                           1024 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           0 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);
})();
