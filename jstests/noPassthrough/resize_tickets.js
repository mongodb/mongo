/**
 * Tests that tickets can be resized during runtime. This test exercises both increase and decrease
 * of tickets.
 *
 * @tags: [
 *   requires_replication,  # Tickets can only be resized when using the WiredTiger engine.
 *   requires_wiredtiger,
 * ]
 */
(function() {
'use strict';

var replTest = new ReplSetTest({name: "test_ticket_resize", nodes: 1});
replTest.startSet();
replTest.initiate();
var mongod = replTest.getPrimary();
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
