/**
 * Test speculative majority change stream reads against a secondary while it is applying an oplog
 * batch. Speculative majority change stream reads on secondaries should read from the lastApplied
 * timestamp.
 *
 *  @tags: [uses_speculative_majority]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.
load("jstests/libs/check_log.js");           // for checkLog.

const name = "speculative_majority_secondary";
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {enableMajorityReadConcern: 'false'}
});
replTest.startSet();
replTest.initiate();

const dbName = name;
const collName = "coll";

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let primaryDB = primary.getDB(dbName);
let primaryColl = primaryDB[collName];
let secondaryDB = secondary.getDB(dbName);

// Do a couple writes on primary and save the first operation time, so we can start the
// secondary change stream from this point.
let res = assert.commandWorked(primaryColl.runCommand("insert", {documents: [{_id: 0}]}));
let startTime = res.operationTime;
assert.commandWorked(primaryColl.update({_id: 0}, {$set: {v: 0}}));
replTest.awaitLastOpCommitted();

// Make the secondary pause after it has written a batch of entries to the oplog but before it
// has applied them.
assert.commandWorked(secondaryDB.adminCommand(
    {configureFailPoint: "pauseBatchApplicationAfterWritingOplogEntries", mode: "alwaysOn"}));

// Pause replication so that the secondary will sync and apply the set of writes from the
// primary in a single batch.
stopServerReplication(secondary);

jsTestLog("Do some writes on the primary.");
assert.writeOK(primaryColl.update({_id: 0}, {$set: {v: 1}}));
assert.writeOK(primaryColl.update({_id: 0}, {$set: {v: 2}}));
assert.writeOK(primaryColl.update({_id: 0}, {$set: {v: 3}}));

// Restart server replication on secondary and wait for the failpoint to be hit.
jsTestLog("Restarting server replication on secondary.");
restartServerReplication(secondary);
checkLog.contains(secondary, "pauseBatchApplicationAfterWritingOplogEntries fail point enabled");

// Open a change stream on the secondary.
res = assert.commandWorked(secondaryDB.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {startAtOperationTime: startTime}}],
    cursor: {}
}));

// We should not expect to see any of the ops currently being applied in the secondary batch.
let changes = res.cursor.firstBatch;
assert.eq(changes.length, 2);
assert.eq(changes[0].fullDocument, {_id: 0});
assert.eq(changes[1].updateDescription.updatedFields, {v: 0});

// Turn off the failpoint and let the test complete.
assert.commandWorked(secondaryDB.adminCommand(
    {configureFailPoint: "pauseBatchApplicationAfterWritingOplogEntries", mode: "off"}));
replTest.stopSet();
})();