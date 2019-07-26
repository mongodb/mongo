// Tests that the "failCommand" failpoint ignores commands from internal clients: SERVER-34943.
// @tags: [requires_replication]
(function() {
"use strict";

// Prevent elections.
const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {votes: 0, priority: 0}}]});
replTest.startSet();
replTest.initiate();
const primary = replTest.getPrimary();
const testDB = primary.getDB("test_failcommand_ignores_internal");

// Enough documents for three getMores.
assert.commandWorked(testDB.collection.insertMany([{}, {}, {}]));
const findReply = assert.commandWorked(testDB.runCommand({find: "collection", batchSize: 0}));
const cursorId = findReply.cursor.id;

// Test failing twice with a particular error code.
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {errorCode: ErrorCodes.BadValue, failCommands: ["getMore"]}
}));
const getMore = {
    getMore: cursorId,
    collection: "collection",
    batchSize: 1
};
assert.commandFailedWithCode(testDB.runCommand(getMore), ErrorCodes.BadValue);

// Waits for secondaries to do getMores on the oplog, which should be ignored by failCommand.
assert.commandWorked(testDB.collection.insertOne({}, {writeConcern: {w: 2}}));

// Second getMore fails but third succeeds, because configureFailPoint was passed {times: 2}.
assert.commandFailedWithCode(testDB.runCommand(getMore), ErrorCodes.BadValue);
assert.commandWorked(testDB.runCommand(getMore));

replTest.stopSet();
}());
