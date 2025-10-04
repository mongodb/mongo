/**
 * Fills WiredTiger cache during steady state oplog application.
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [
        {
            slowms: 30000, // Don't log slow operations on primary.
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
    nodeOptions: {
        // Constrain the storage engine cache size to make it easier to fill it up with
        // unflushed modifications.
        wiredTigerCacheSizeGB: 1,
    },
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const mydb = primary.getDB("test");
const coll = mydb.getCollection("t");

const numDocs = 2;
const minDocSizeMB = 10;

for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(
        coll.save(
            {_id: i, i: 0, x: "x".repeat(minDocSizeMB * 1024 * 1024)},
            {writeConcern: {w: nodes.length, wtimeout: ReplSetTest.kDefaultTimeoutMS}},
        ),
    );
}
assert.eq(numDocs, coll.find().itcount());

const numUpdates = 500;
const secondary = rst.getSecondary();
const batchOpsLimit = assert.commandWorked(
    secondary.adminCommand({getParameter: 1, replBatchLimitOperations: 1}),
).replBatchLimitOperations;
jsTestLog(
    "Oplog application on secondary " + secondary.host + " is limited to " + batchOpsLimit + " operations per batch.",
);

jsTestLog("Buffering " + numUpdates + " updates to " + numDocs + " documents on secondary.");
const session = primary.startSession();
const sessionDB = session.getDatabase(mydb.getName());
const sessionColl = sessionDB.getCollection(coll.getName());
session.startTransaction();
for (let i = 0; i < numDocs; ++i) {
    for (let j = 0; j < numUpdates; ++j) {
        assert.commandWorked(sessionColl.update({_id: i}, {$inc: {i: 1}}));
    }
}
assert.commandWorked(session.commitTransaction_forTesting());
session.endSession();

jsTestLog("Applying updates on secondary " + secondary.host);

// If the secondary is unable to apply all the operations in the unprepared transaction within
// a single batch with the constrained cache settings, the replica set will not reach a stable
// state.
rst.awaitReplication();

rst.stopSet();
