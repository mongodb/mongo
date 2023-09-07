/**
 * Verifies that the internal collection catalog parameter is correctly cleanup after downgrade.
 */

import "jstests/multiVersion/libs/multi_rs.js";

(function() {
"use strict";

const latestVersion = "latest";
const rst = new ReplSetTest(
    {name: jsTestName(), nodes: [{binVersion: latestVersion}, {binVersion: latestVersion}]});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB("test");

const collectionName = "coll";
assert.commandWorked(testDB.createCollection(collectionName, {timeseries: {timeField: 't'}}));
let output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: 'system.buckets.coll'}));
assert.eq(output.changed, false);

// Ensure that running empty collMod does not affect the flag while on latest FCV.
assert.commandWorked(testDB.runCommand({collMod: collectionName}));
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: 'system.buckets.coll'}));
assert.eq(output.changed, false);

// Running a setFCV downgrade command should remove the catalog flag.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: 'system.buckets.coll'}));
assert.eq(output.changed, undefined);

// While FCV is in downgrading/downgraded state, collMod command should not effect the bucketing
// parameters flag.
assert.commandWorked(
    testDB.runCommand({collMod: collectionName, timeseries: {granularity: "minutes"}}));
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: 'system.buckets.coll'}));
assert.eq(output.changed, undefined);

assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// After the FCV is set to latest, the value of the flag of the collection should be undefined.
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: 'system.buckets.coll'}));
assert.eq(output.changed, undefined);

// After FCV is set to latest, collMod command should explicity set the bucketing parameters flag
// to true.
assert.commandWorked(testDB.runCommand({
    collMod: collectionName,
    timeseries: {bucketRoundingSeconds: 100000, bucketMaxSpanSeconds: 100000}
}));
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: 'system.buckets.coll'}));
assert.eq(output.changed, true);

// Now that the feature is turned on, new time-series collections by default will have the flag set
// to 'false'.
const otherCollName = "other_coll";
const otherCollBucketsName = "system.buckets.other_coll";
assert.commandWorked(testDB.createCollection(
    otherCollName,
    {timeseries: {timeField: 't', bucketRoundingSeconds: 3600, bucketMaxSpanSeconds: 3600}}));
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: otherCollBucketsName}));
assert.eq(output.changed, false);

// We will now test the flag is set correctly if the FCV downgrade fails after only reaching the
// `downgrading` FCV state, and then is fully upgraded again.
jsTestLog("Turning the failpoint on.");
assert.commandWorked(
    rst.getPrimary().adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
assert.commandFailedWithCode(
    testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);

// Confirm feature is still enabled since the FCV downgrade failed.
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: otherCollBucketsName}));
assert.eq(output.changed, false);

assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// Confirm the feature remains enabled when the FCV is upgraded. Since the FCV is already at '7.1'
// the value of the flag should not change.
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: otherCollBucketsName}));
assert.eq(output.changed, false);

// The flag should be set to true after a collMod command.
assert.commandWorked(testDB.runCommand({
    collMod: otherCollName,
    timeseries: {bucketRoundingSeconds: 100000, bucketMaxSpanSeconds: 100000}
}));
output = assert.commandWorked(
    testDB.runCommand({timeseriesCatalogBucketParamsChanged: otherCollBucketsName}));
assert.eq(output.changed, true);

rst.stopSet();
})();
