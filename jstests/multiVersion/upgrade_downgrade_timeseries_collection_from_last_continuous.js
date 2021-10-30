/**
 * Tests that upgrading time-series collections created using the last-continuous binary warns about
 * potentially mixed-schema data when building secondary indexes on time-series measurements on the
 * latest binary. Additionally, tests that downgrading FCV from 5.2 removes the
 * 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag from time-series collections.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/multiVersion/libs/multi_rs.js");

const oldVersion = "last-continuous";
const nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion}
};

const rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

let primary = rst.getPrimary();
let db = primary.getDB(dbName);
let coll = db.getCollection(collName);

// Create a time-series collection while using older binaries.
const timeField = "time";
const metaField = "meta";
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 1, x: 1}));
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 2, x: {y: "z"}}));
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 3, x: "abc"}));

jsTest.log("Upgrading replica set from last-continuous to latest");
rst.upgradeSet({binVersion: "latest", setParameter: {logComponentVerbosity: tojson({storage: 1})}});

primary = rst.getPrimary();
db = primary.getDB(dbName);
coll = db.getCollection(collName);

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
const secondary = rst.getSecondary();
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: true}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: true}, /*expectedCount=*/1));

assert.commandWorked(coll.createIndex({[timeField]: 1}, {name: "time_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

assert.commandWorked(coll.createIndex({[metaField]: 1}, {name: "meta_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

assert.commandWorked(db.getCollection(collName).createIndex({x: 1}, {name: "x_1"}));

// May have mixed-schema data.
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/1));

// No mixed-schema data detected.
assert(checkLog.checkContainsWithCountJson(
    primary, 6057700, {namespace: bucketCollName}, /*expectedCount=*/0));

// Catalog entry flag gets set to false.
assert(
    checkLog.checkContainsWithCountJson(primary, 6057601, {setting: false}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: false}, /*expectedCount=*/1));

// After successfully building an index on a time-series measurement, subsequent index builds on
// time-series measurements will skip checking for mixed-schema data.
assert.commandWorked(
    db.getCollection(collName).createIndex({[timeField]: 1, x: 1}, {name: "time_1_x_1"}));

// Check that the log message warning about potential mixed-schema data does not get logged again.
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/1));

// No mixed-schema data detected.
assert(checkLog.checkContainsWithCountJson(
    primary, 6057700, {namespace: bucketCollName}, /*expectedCount=*/0));

// Catalog entry flag should still be set to false, but not again.
assert(
    checkLog.checkContainsWithCountJson(primary, 6057601, {setting: false}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: false}, /*expectedCount=*/1));

// Cannot downgrade when there are indexes on time-series measurements present.
assert.commandFailedWithCode(
    primary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}),
    ErrorCodes.CannotDowngrade);
assert.commandWorked(db.getCollection(collName).dropIndex("x_1"));
assert.commandWorked(db.getCollection(collName).dropIndex("time_1_x_1"));

// The FCV downgrade process removes the catalog entry flag from time-series collections.
jsTest.log("Setting FCV to 'lastContinuousFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: null}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: null}, /*expectedCount=*/1));

rst.stopSet();
}());