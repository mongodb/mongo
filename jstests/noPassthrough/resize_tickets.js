/**
 * Tests that tickets can be resized during runtime, if not being dynamically adjusted. This test
 * exercises both increase and decrease of tickets.
 *
 * @tags: [
 *   requires_replication,  # Tickets can only be resized when using the WiredTiger engine.
 *   requires_wiredtiger,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

jsTestLog("Start a replica set with execution control enabled by default");
let replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
});
replTest.startSet();
replTest.initiate();
let mongod = replTest.getPrimary();
// TODO (SERVER-67104): Remove the feature flag check.
if (FeatureFlagUtil.isPresentAndEnabled(mongod, 'ExecutionControl')) {
    // Users cannot manually adjust read/write tickets once execution control is enabled at startup.
    assert.commandFailedWithCode(
        mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 10}),
        ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(
        mongod.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 10}),
        ErrorCodes.IllegalOperation);
}
replTest.stopSet();

const gfixedConcurrentTransactions = "fixedConcurrentTransactions";
jsTestLog("Start a replica set with execution control explicitly disabled on startup");
replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        // Users can opt out of execution control by specifying the 'fixedConcurrentTransactions'
        // option on startup.
        setParameter: {storageEngineConcurrencyAdjustmentAlgorithm: gfixedConcurrentTransactions}
    },
});
replTest.startSet();
replTest.initiate();
mongod = replTest.getPrimary();

assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 20}));
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 20}));
replTest.stopSet();

jsTestLog("Start a replica set with execution control implicitly disabled on startup");
replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        // If a user manually sets read/write tickets on startup, implicitly set the
        // 'storageEngineConcurrencyAdjustmentAlgorithm' parameter to 'fixedConcurrentTransactions'
        // and disable execution control.
        setParameter: {wiredTigerConcurrentReadTransactions: 20}
    },
});
replTest.startSet();
replTest.initiate();
mongod = replTest.getPrimary();

const getParameterResult =
    mongod.adminCommand({getParameter: 1, storageEngineConcurrencyAdjustmentAlgorithm: 1});
assert.commandWorked(getParameterResult);
assert.eq(getParameterResult.storageEngineConcurrencyAdjustmentAlgorithm,
          gfixedConcurrentTransactions);

// The 20, 10, 30 sequence of ticket resizes are just arbitrary numbers in order to test a decrease
// (20 -> 10) and an increase (10 -> 30) of tickets.
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 20}));
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 10}));
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 30}));
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 20}));
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 10}));
assert.commandWorked(
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 30}));
replTest.stopSet();
}());
