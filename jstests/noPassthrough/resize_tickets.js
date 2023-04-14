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

let replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
});
replTest.startSet();
replTest.initiate();
let mongod = replTest.getPrimary();
// TODO (SERVER-67104): Remove the feature flag check.
if (FeatureFlagUtil.isPresentAndEnabled(mongod, 'ExecutionControl')) {
    assert.commandFailedWithCode(
        mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 10}),
        ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(
        mongod.adminCommand({setParameter: 1, wiredTigerConcurrentReadTransactions: 10}),
        ErrorCodes.IllegalOperation);
}
replTest.stopSet();

replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        setParameter: {storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions"}
    },
});
replTest.startSet();
replTest.initiate();
mongod = replTest.getPrimary();
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
