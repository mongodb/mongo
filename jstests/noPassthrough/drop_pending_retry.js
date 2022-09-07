/**
 * Tests that the drop pending ident reaper retries table drops when ObjectIsBusy is returned.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_wiredtiger
 * ]
 */
(function() {
"use strict";

load("jstests/disk/libs/wt_file_helper.js");

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the history window to zero to explicitly control the oldest timestamp.
            minSnapshotHistoryWindowInSeconds: 0,
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const db = primary.getDB(dbName);

// Control checkpoints.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "alwaysOn"}));

// Mocks WT returning EBUSY when dropping the table.
assert.commandWorked(primary.adminCommand({configureFailPoint: "WTDropEBUSY", mode: "alwaysOn"}));

assert.commandWorked(db.createCollection("toDrop"));
assert.commandWorked(db.createCollection("toWrite"));

const collUri = getUriForColl(db.getCollection("toDrop"));
const indexUri = getUriForIndex(db.getCollection("toDrop"), /*indexName=*/"_id_");

assert(db.getCollection("toDrop").drop());

// We need to perform this write so that the drop timestamp above is less than the checkpoint
// timestamp.
assert.commandWorked(db.getCollection("toWrite").insert({x: 1}));

// Take a checkpoint to advance the checkpoint timestamp.
assert.commandWorked(db.adminCommand({fsync: 1}));

// Tests that the table drops are retried each time the drop pending reaper runs until they succeed.
// We wait for 5 retries here. 5 for the collection table and 5 for the index table.
checkLog.containsWithAtLeastCount(primary, "Drop-pending ident is still in use", 2 * 5);

// Let the table drops succeed.
assert.commandWorked(primary.adminCommand({configureFailPoint: "WTDropEBUSY", mode: "off"}));

// Completing drop for ident
checkLog.containsJson(primary, 22237, {
    ident: function(ident) {
        return ident == collUri;
    }
});
checkLog.containsJson(primary, 22237, {
    ident: function(ident) {
        return ident == indexUri;
    }
});

assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "off"}));

rst.stopSet();
}());