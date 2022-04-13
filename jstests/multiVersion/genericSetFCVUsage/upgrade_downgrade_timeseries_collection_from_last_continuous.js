/**
 * Tests that there are no upgrade or downgrade requirements for secondary indexes on time-series
 * measurements between kLastContinuous and kLatest.
 *
 * @tags: [disabled_for_fcv_6_1_upgrade]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/multiVersion/libs/multi_rs.js");

const oldVersion = "last-continuous";
const nodes = {
    n1: {binVersion: oldVersion, setParameter: 'featureFlagTimeseriesMetricIndexes=true'},
    n2: {binVersion: oldVersion, setParameter: 'featureFlagTimeseriesMetricIndexes=true'}
};

const rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();
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

const bucketCollName = dbName + ".system.buckets." + collName;

assert.commandWorked(coll.createIndex({[timeField]: 1}, {name: "time_1"}));
assert.commandWorked(coll.createIndex({[metaField]: 1}, {name: "meta_1"}));
assert.commandWorked(db.getCollection(collName).createIndex({x: 1}, {name: "x_1"}));

// No mixed-schema data detected.
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057700, {namespace: bucketCollName}, /*expectedCount=*/0));

// Catalog entry flag does not get set to false. It is already false.
assert(
    checkLog.checkContainsWithCountJson(primary, 6057601, {setting: false}, /*expectedCount=*/0));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: false}, /*expectedCount=*/0));

jsTest.log("Upgrading replica set from last-continuous to latest");
rst.upgradeSet({binVersion: "latest", setParameter: {logComponentVerbosity: tojson({storage: 1})}});

primary = rst.getPrimary();
secondary = rst.getSecondary();
db = primary.getDB(dbName);
coll = db.getCollection(collName);

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(primary)) {
    jsTest.log("Skipping test as the featureFlagTimeseriesMetricIndexes feature flag is disabled");
    rst.stopSet();
    return;
}

jsTest.log("Setting FCV to 'latestFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// The FCV upgrade process does not add the catalog entry flag to time-series collections.
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: true}, /*expectedCount=*/0));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: true}, /*expectedCount=*/0));

// The FCV downgrade process does not remove the catalog entry flag from time-series collections.
jsTest.log("Setting FCV to 'lastContinuousFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: null}, /*expectedCount=*/0));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: null}, /*expectedCount=*/0));

rst.stopSet();
}());
