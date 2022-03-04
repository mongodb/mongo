/**
 * Tests that a unique index built in last-lts can be converted to a regular index after upgrading
 * to latest version.
 */

(function() {
"use strict";
load('./jstests/multiVersion/libs/multi_rs.js');

const lastLTSVersion = "last-lts";
const latestVersion = "latest";

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {binVersion: lastLTSVersion},
});

jsTestLog("Running test with version " + lastLTSVersion);
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB('test');
const collName = 'collMod_convert_to_non_unique';
let coll = testDB.getCollection(collName);
coll.drop();

assert.commandWorked(testDB.createCollection(collName));

// Creates a unique index and verifies it rejects duplicate keys.
assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
assert.commandWorked(coll.insertMany([{_id: 0, a: 0}, {_id: 1, a: 1}]));
assert.commandFailedWithCode(coll.insert({_id: 2, a: 1}), ErrorCodes.DuplicateKey);

jsTestLog("Upgrading version " + latestVersion);
rst.upgradeSet({binVersion: latestVersion});

primary = rst.getPrimary();
testDB = primary.getDB('test');
coll = testDB.getCollection(collName);

let primaryAdminDB = primary.getDB("admin");
assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

const collModIndexUniqueEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagCollModIndexUnique: 1}))
        .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled');
    rst.stopSet();
    return;
}

// Converts the unique index back to non-unique and verifies it accepts duplicate keys.
assert.commandWorked(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, forceNonUnique: true}}));
assert.commandWorked(coll.insert({_id: 2, a: 1}));

// Attempts to convert the index to unique again but fails because of the duplicate.
assert.commandWorked(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    ErrorCodes.CannotConvertIndexToUnique);

// Removes the duplicate key and converts the index back to unique successfully.
assert.commandWorked(coll.deleteOne({_id: 2}));
assert.commandWorked(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}));

rst.stopSet();
})();
