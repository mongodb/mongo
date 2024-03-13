/* Test that both transaction and non-transaction snapshot reads on capped collections work
 * correctly.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const collName = "coll";
const primary = replSet.getPrimary();
const primaryDB = primary.getDB('test');

assert.commandWorked(primaryDB.createCollection(collName, {capped: true, size: 32, max: 1}));

// Non-transaction snapshot reads on capped collections are allowed starting in 8.0.
assert.commandWorked(primaryDB.runCommand({find: collName, readConcern: {level: "snapshot"}}));
assert.commandWorked(primaryDB.runCommand(
    {aggregate: collName, pipeline: [], cursor: {}, readConcern: {level: "snapshot"}}));
assert.commandWorked(
    primaryDB.runCommand({distinct: collName, key: "_id", readConcern: {level: "snapshot"}}));

// Testing that snapshot reads work in a transaction as well.
const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase('test');
session.startTransaction({readConcern: {level: 'snapshot'}});
assert.commandWorked(sessionDB.runCommand({find: collName}));

replSet.stopSet();
