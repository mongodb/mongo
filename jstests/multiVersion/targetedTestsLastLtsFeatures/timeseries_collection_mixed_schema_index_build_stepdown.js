/**
 * Only the primary node enforces the mixed-schema data constraint during an index build. This is
 * because index builds may not fail on secondaries. They can only be aborted via the
 * abortIndexBuild oplog entry. Secondaries will still record any mixed-schema data they detect
 * during an index build but take no action. This tests that a secondary stepping up will cause an
 * index build to fail due to the earlier detection of mixed-schema data.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fail_point_util.js");
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/noPassthrough/libs/index_build.js');

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

// Create a bucket with mixed-schema data.
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 1, x: 1}));
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 1, x: "abc"}));

// Create buckets without mixed-schema data.
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 2, x: 1}));
assert.commandWorked(coll.insert({[timeField]: ISODate(), [metaField]: 3, x: 1}));

jsTest.log("Upgrading replica set from last-lts to latest");
rst.upgradeSet(
    {binVersion: "latest", setParameter: {logComponentVerbosity: tojson({storage: 1, index: 1})}});

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

// Hang the index build on the primary after replicating the startIndexBuild oplog entry.
const primaryIndexBuild = configureFailPoint(primary, "hangAfterSettingUpIndexBuildUnlocked");

// Hang the index build on the secondary after the collection scan phase is complete.
const secondaryIndexBuild = configureFailPoint(secondary, "hangAfterStartingIndexBuildUnlocked");

const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary, bucketCollName, {x: 1}, {name: "x_1"}, [ErrorCodes.InterruptedDueToReplStateChange]);

primaryIndexBuild.wait();
secondaryIndexBuild.wait();

jsTestLog("Stepping up new primary");
assert.commandWorked(secondary.adminCommand({replSetStepUp: 1}));

primaryIndexBuild.off();
secondaryIndexBuild.off();

awaitIndexBuild();

// Aborting index build commit due to the earlier detection of mixed-schema data (now primary).
checkLog.containsJson(secondary, 6057701);

// Index build: failed (now primary).
checkLog.containsJson(secondary, 20649);

// Aborting index build from oplog entry (now secondary).
checkLog.containsJson(primary, 3856206);

// Check that the catalog entry flag doesn't get set to false.
assert(
    checkLog.checkContainsWithCountJson(primary, 6057601, {setting: false}, /*expectedCount=*/0));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: false}, /*expectedCount=*/0));

// The FCV downgrade process removes the catalog entry flag from time-series collections.
jsTest.log("Setting FCV to 'lastLTSFCV'");
assert.commandWorked(secondary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

assert(checkLog.checkContainsWithCountJson(primary, 6057601, {setting: null}, /*expectedCount=*/1));
assert(
    checkLog.checkContainsWithCountJson(secondary, 6057601, {setting: null}, /*expectedCount=*/1));

rst.stopSet();
}());