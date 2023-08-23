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
    {setFeatureCompatibilityVersion: '7.0', confirm: true}));
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
    {setFeatureCompatibilityVersion: '7.1', confirm: true}));

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

rst.stopSet();
})();
