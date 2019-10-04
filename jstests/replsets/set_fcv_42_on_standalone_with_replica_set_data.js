/**
 * Tests that standalone nodes with replica set data are unable to upgrade or downgrade FCV while
 * the config.transactions collection is non-empty.
 * @tags: [uses_transactions, requires_persistence]
 */
(function() {

"use strict";
load("jstests/libs/feature_compatibility_version.js");

let replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const dbName = "test";
const collName = "set_fcv_42_on_standalone";
let adminDB = primary.getDB('admin');

assert.commandWorked(primary.getDB(dbName).createCollection(collName));

jsTestLog("Downgrade the featureCompatibilityVersion.");
assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(adminDB, lastStableFCV);
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);

session.startTransaction();
assert.commandWorked(sessionDB[collName].insert({_id: 1}));
assert.commandWorked(session.commitTransaction_forTesting());
// Restarting as a standalone causes the node to restart from the stable timestamp without applying
// operations from the oplog.
replTest.awaitLastStableRecoveryTimestamp();

jsTestLog(
    "Test upgrade on a standalone with replica set data and a non-empty config.transactions table.");
const standalone = replTest.restart(0, {noReplSet: true});
adminDB = standalone.getDB('admin');
const localDB = standalone.getDB('local');
const replSetData = localDB.getCollection('system.replset').findOne();
assert.neq(null, replSetData);

// Make sure the config.transactions table is not empty.
const configDB = standalone.getDB('config');
const txnRecord = configDB.getCollection('transactions').findOne();
assert.neq(null, txnRecord);

// Should fail on featureCompatibilityVersion upgrade attempt.
assert.commandFailedWithCode(
    standalone.getDB('admin').adminCommand({setFeatureCompatibilityVersion: latestFCV}),
    ErrorCodes.IllegalOperation);

jsTestLog(
    "Empty the config.transactions table and successfully upgrade featureCompatibilityVersion.");
assert.commandWorked(configDB.getCollection('transactions').remove({}));
assert.commandWorked(configDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(adminDB, latestFCV);

jsTestLog(
    "Test downgrade on a standalone with replica set data and a non-empty config.transactions table.");
assert.commandWorked(configDB.getCollection('transactions').insertOne(txnRecord));

// Should fail on featureCompatibilityVersion downgrade attempt.
assert.commandFailedWithCode(
    standalone.getDB('admin').adminCommand({setFeatureCompatibilityVersion: lastStableFCV}),
    ErrorCodes.IllegalOperation);

jsTestLog(
    "Empty the config.transactions table and successfully downgrade featureCompatibilityVersion.");
assert.commandWorked(configDB.getCollection('transactions').remove({}));
assert.commandWorked(configDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(adminDB, lastStableFCV);

replTest.stopSet();
})();