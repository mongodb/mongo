/* Test that snapshot reads and afterClusterTime majority reads are not allowed on
 * config.transactions.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const primaryDB = primary.getDB('config');

const operationTime =
    assert.commandWorked(primaryDB.runCommand({find: "transactions"})).operationTime;
assert.commandWorked(
    primaryDB.runCommand({find: "transactions", readConcern: {level: "majority"}}));
assert.commandFailedWithCode(
    primaryDB.runCommand(
        {find: "transactions", readConcern: {level: "majority", afterClusterTime: operationTime}}),
    5557800);
assert.commandFailedWithCode(
    primaryDB.runCommand({find: "transactions", readConcern: {level: "snapshot"}}), 5557800);
assert.commandFailedWithCode(
    primaryDB.runCommand(
        {find: "transactions", readConcern: {level: "snapshot", atClusterTime: operationTime}}),
    5557800);

replSet.stopSet();
})();
