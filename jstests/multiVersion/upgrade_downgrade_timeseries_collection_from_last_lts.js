/**
 * Tests that upgrading time-series collections created using the last-lts binary warns about
 * potentially mixed-schema data when building secondary indexes on time-series measurements on the
 * latest binary. Additionally, tests that downgrading FCV from 5.2 removes the
 * 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag from time-series collections.
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
rst.upgradeSet({binVersion: "latest", setParameter: {logComponentVerbosity: tojson({storage: 1})}});

primary = rst.getPrimary();
db = primary.getDB(dbName);

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(primary)) {
    jsTest.log("Skipping test as the featureFlagTimeseriesMetricIndexes feature flag is disabled");
    rst.stopSet();
    return;
}

// Building indexes on time-series measurements is only supported in FCV >= 5.2.
jsTest.log("Setting FCV to 'latestFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

const bucketCollName = dbName + ".system.buckets." + collName;

// The FCV upgrade process adds the catalog entry flag to time-series collections.
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: true}, /*expectedCount=*/1));

assert.commandWorked(db.getCollection(collName).createIndex({[timeField]: 1}, {name: "time_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

assert.commandWorked(db.getCollection(collName).createIndex({x: 1}, {name: "x_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/1));

assert.commandWorked(
    db.getCollection(collName).createIndex({[timeField]: 1, x: 1}, {name: "time_1_x_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/2));

// Cannot downgrade when there are indexes on time-series measurements present.
assert.commandFailedWithCode(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             ErrorCodes.CannotDowngrade);
assert.commandWorked(db.getCollection(collName).dropIndex("x_1"));
assert.commandWorked(db.getCollection(collName).dropIndex("time_1_x_1"));

// The FCV downgrade process removes the catalog entry flag from time-series collections.
jsTest.log("Setting FCV to 'lastLTSFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: null}, /*expectedCount=*/1));

rst.stopSet();
}());