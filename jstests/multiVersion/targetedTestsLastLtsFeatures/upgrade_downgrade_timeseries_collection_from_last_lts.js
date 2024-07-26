/**
 * Tests that upgrading time-series collections created using the last-lts binary warns about
 * potentially mixed-schema data when building secondary indexes on time-series measurements on the
 * latest binary. Additionally, tests that downgrading FCV from 5.3 removes the
 * 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag from time-series collections.
 *
 * Also, tests that upgrading a time-series collection with no mixed-schema data allows metric
 * indexes to be created.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/multiVersion/libs/multi_rs.js");

// Since v5.0 no longer supports writing mixed schema buckets as of 5.0.29, pin the version
// to 5.0.28 to retain coverage as v5.0 still supports the presence of mixed schema data.
const oldVersion = "5.0.28";
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
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 1, x: {y: "z"}}));
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 1, x: "abc"}));

jsTest.log("Upgrading replica set from last-lts to latest");
rst.upgradeSet({binVersion: "latest", setParameter: {logComponentVerbosity: tojson({storage: 1})}});

primary = rst.getPrimary();
db = primary.getDB(dbName);
coll = db.getCollection(collName);

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(primary)) {
    jsTest.log("Skipping test as the featureFlagTimeseriesMetricIndexes feature flag is disabled");
    rst.stopSet();
    return;
}

// Building indexes on time-series measurements is only supported in FCV >= 5.3.
jsTest.log("Setting FCV to 'latestFCV'");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

const bucketCollName = dbName + ".system.buckets." + collName;

// The FCV upgrade process adds the catalog entry flag to time-series collections.
const secondary = rst.getSecondary();
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: true}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: true}, /*expectedCount=*/1));

// Creating an index on time does not involve checking for mixed-schema data.
assert.commandWorked(coll.createIndex({[timeField]: 1}, {name: "time_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

// Creating an index on metadata does not involve checking for mixed-schema data.
assert.commandWorked(coll.createIndex({[metaField]: 1}, {name: "meta_1"}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

// Creating a partial index, on metadata and time only, does not involve checking for mixed-schema
// data.
assert.commandWorked(coll.createIndex({[timeField]: 1, [metaField]: 1}, {
    name: "time_1_meta_1_partial",
    partialFilterExpression: {[timeField]: {$gt: ISODate()}, [metaField]: 1}
}));
assert(checkLog.checkContainsWithCountJson(
    primary, 6057502, {namespace: bucketCollName}, /*expectedCount=*/0));

// Creating a metric index requires checking for mixed-schema data.
assert.commandFailedWithCode(coll.createIndex({x: 1}, {name: "x_1"}), ErrorCodes.CannotCreateIndex);
assert(checkLog.checkContainsWithCountJson(primary,
                                           6057502 /* May have mixed-schema data. */,
                                           {namespace: bucketCollName},
                                           /*expectedCount=*/1));
assert(checkLog.checkContainsWithCountJson(primary,
                                           6057700 /* Mixed-schema data detected. */,
                                           {namespace: bucketCollName},
                                           /*expectedCount=*/1));

// A compound index that includes a metric counts as a metric index.
assert.commandFailedWithCode(coll.createIndex({[timeField]: 1, x: 1}, {name: "time_1_x_1"}),
                             ErrorCodes.CannotCreateIndex);
assert(checkLog.checkContainsWithCountJson(primary,
                                           6057502 /* May have mixed-schema data. */,
                                           {namespace: bucketCollName},
                                           /*expectedCount=*/2));
assert(checkLog.checkContainsWithCountJson(primary,
                                           6057700 /* Mixed-schema data detected. */,
                                           {namespace: bucketCollName},
                                           /*expectedCount=*/2));

// A partialFilterExperssion on a metric makes it a metric index, even if the index key is
// metadata+time only.
assert.commandFailedWithCode(
    coll.createIndex({[timeField]: 1, [metaField]: 1},
                     {name: "time_1_meta_1_partial_metric", partialFilterExpression: {x: 1}}),
    ErrorCodes.CannotCreateIndex);
assert(checkLog.checkContainsWithCountJson(primary,
                                           6057502 /* May have mixed-schema data. */,
                                           {namespace: bucketCollName},
                                           /*expectedCount=*/3));
assert(checkLog.checkContainsWithCountJson(primary,
                                           6057700 /* Mixed-schema data detected. */,
                                           {namespace: bucketCollName},
                                           /*expectedCount=*/3));

// Check that the catalog entry flag doesn't get set to false.
assert(
    checkLog.checkContainsWithCountJson(primary, 6057601, {setting: false}, /*expectedCount=*/0));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: false}, /*expectedCount=*/0));

// The FCV downgrade process removes the catalog entry flag from time-series collections.
jsTest.log("Setting FCV to 'lastLTSFCV'");
{
    // However, the first attempt at downgrading fails because a partial index still exists.
    assert.commandFailedWithCode(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);
    // Once we remove the index, downgrading succeeds.
    assert.commandWorked(coll.dropIndex("time_1_meta_1_partial"));
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
}
assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: null}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: null}, /*expectedCount=*/1));

// Check that upgrading a clean collection allows all kinds of indexes to be created
// (time, metadata, metric).
jsTest.log("Testing upgrade of a non-mixed-schema collection.");
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
assert.commandWorked(coll.insert([
    {[timeField]: ISODate(), [metaField]: 1, x: 1},
    {[timeField]: ISODate(), [metaField]: 1, x: 2},
    {[timeField]: ISODate(), [metaField]: 1, x: 3},
]));
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(coll.createIndex({[timeField]: 1}, {name: "time_1"}));
assert.commandWorked(coll.createIndex({[metaField]: 1}, {name: "meta_1"}));
assert.commandWorked(coll.createIndex({[timeField]: 1, [metaField]: 1}, {
    name: "time_1_meta_1_partial",
    partialFilterExpression: {[timeField]: {$gt: ISODate()}, [metaField]: 1}
}));
assert.commandWorked(coll.createIndex({x: 1}, {name: "x_1"}));
assert.commandWorked(coll.createIndex({[timeField]: 1, x: 1}, {name: "time_1_x_1"}));
assert.commandWorked(
    coll.createIndex({[timeField]: 1, [metaField]: 1},
                     {name: "time_1_meta_1_partial_metric", partialFilterExpression: {x: 1}}));

rst.stopSet();
}());
