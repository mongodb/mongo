/**
 * Tests that upgrading time-series collections created in earlier server versions warn about
 * potentially mixed-schema data when building secondary indexes on time-series measurements.
 *
 * TODO SERVER-60577: expand testing by checking that index builds will fail with mixed-schema data
 * and succeed when there is no mixed-schema data in time-series collections.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/multiVersion/libs/multi_rs.js");

const oldVersion = "last-lts";
const nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    n3: {binVersion: oldVersion}
};

const rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

let primary = rst.getPrimary();
let db = primary.getDB(dbName);

// Create a time-series collection while using older binaries.
const timeField = "time";
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: timeField}}));

jsTest.log("Upgrading replica set from last-lts to latest");
rst.upgradeSet({binVersion: "latest"});

primary = rst.getPrimary();
db = primary.getDB(dbName);

// Building indexes on time-series measurements is only supported in FCV >= 5.2.
jsTest.log("Setting FCV to 'latestFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(primary)) {
    jsTest.log("Skipping test as the featureFlagTimeseriesMetricIndexes feature flag is disabled");
    rst.stopSet();
    return;
}

const bucketCollName = dbName + ".system.buckets." + collName;

assert.commandWorked(db.getCollection(collName).createIndex({[timeField]: 1}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

assert.commandWorked(db.getCollection(collName).createIndex({x: 1}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/1));

assert.commandWorked(db.getCollection(collName).createIndex({[timeField]: 1, x: 1}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/2));

rst.stopSet();
}());