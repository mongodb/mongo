/**
 * Verifies that arbitrary deletes on timeseries collections are not supported after downgrade.
 * @tags: [requires_fcv_70]
 */

import "jstests/multiVersion/libs/multi_rs.js";

(function() {
"use strict";

const latestVersion = "latest";
const dateTime = ISODate("2023-10-30T16:00:00Z");
const rst = new ReplSetTest(
    {name: jsTestName(), nodes: [{binVersion: latestVersion}, {binVersion: latestVersion}]});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB("test");

// Create timeseries collection and insert documents to be deleted.
const collectionName = "coll";
assert.commandWorked(testDB.createCollection(
    collectionName, {timeseries: {timeField: 'timestamp', metaField: "a"}}));

const measurements = [
    {"metadata": 'a', "timestamp": dateTime, "value": 1},
    {"metadata": 'a', "timestamp": dateTime, "value": 2},
    {"metadata": 'a', "timestamp": dateTime, "value": 3},
    {"metadata": 'a', "timestamp": dateTime, "value": 4},
    {"metadata": 'a', "timestamp": dateTime, "value": 5}
];
let coll = testDB[collectionName];
assert.commandWorked(coll.insertMany(measurements));

// Verify that performing an arbitrary delete of a few measurements works on latest version.
assert.commandWorked(
    testDB.runCommand({delete: collectionName, deletes: [{q: {"value": {$gte: 3}}, limit: 0}]}));

// Downgrade FCV and verify that performing arbitrary deletes on timeseries collections does not
// work on downgraded version.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
coll.drop();
assert.commandWorked(testDB.createCollection(
    collectionName, {timeseries: {timeField: 'timestamp', metaField: "a"}}));
coll = testDB[collectionName];
assert.commandWorked(coll.insertMany(measurements));
assert.commandFailedWithCode(
    testDB.runCommand({delete: collectionName, deletes: [{q: {"value": {$gte: 3}}, limit: 0}]}),
    ErrorCodes.InvalidOptions);

// Upgrade back to latest version, verify that arbitrary deletes still work.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
coll.drop();
assert.commandWorked(testDB.createCollection(
    collectionName, {timeseries: {timeField: 'timestamp', metaField: "a"}}));
coll = testDB[collectionName];
assert.commandWorked(coll.insertMany(measurements));
assert.commandWorked(
    testDB.runCommand({delete: collectionName, deletes: [{q: {"value": {$gte: 3}}, limit: 0}]}));

// We will now test that arbitrary deletes are allowed if the FCV downgrade fails after only
// reaching the `downgrading` FCV state, and then is fully upgraded again.
assert.commandWorked(
    rst.getPrimary().adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
assert.commandFailedWithCode(
    testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);
// Upgrade fully again.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
// Arbitrary deletes on timeseries collections should still work.
coll.drop();
assert.commandWorked(testDB.createCollection(
    collectionName, {timeseries: {timeField: 'timestamp', metaField: "metadata"}}));
coll = testDB[collectionName];
assert.commandWorked(coll.insertMany(measurements));
assert.commandWorked(
    testDB.runCommand({delete: collectionName, deletes: [{q: {"value": {$gte: 3}}, limit: 0}]}));

rst.stopSet();
})();
