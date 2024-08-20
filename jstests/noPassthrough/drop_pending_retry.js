/**
 * Tests that the drop pending ident reaper retries table drops when ObjectIsBusy is returned.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_wiredtiger
 * ]
 */
import {getUriForColl, getUriForIndex} from "jstests/disk/libs/wt_file_helper.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the history window to zero to explicitly control the oldest timestamp.
            minSnapshotHistoryWindowInSeconds: 0,
            logComponentVerbosity: tojson({storage: 1}),
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
const indexUri = getUriForIndex(db.getCollection("toDrop"), /*indexName=*/ "_id_");

assert(db.getCollection("toDrop").drop());

// We need to perform this write so that the drop timestamp above is less than the checkpoint
// timestamp.
let insertResponse = db.runCommand({insert: "toWrite", documents: [{x: 1}]});

assert(insertResponse.ok);
let insertOptime = insertResponse.opTime;
// Check that lastCommited, lastApplied, and allDurable are what we expect them to be.

let replSetStatus = rst.status();
assert.eq(replSetStatus.optimes.lastCommittedOpTime, insertOptime, tojson(replSetStatus));
assert.eq(replSetStatus.optimes.appliedOpTime, insertOptime, tojson(replSetStatus));
assert.eq(replSetStatus.optimes.durableOpTime, insertOptime, tojson(replSetStatus));
// Take a checkpoint to advance the checkpoint timestamp.
assert.commandWorked(db.adminCommand({fsync: 1}));

// Tests that the table drops are retried when the drop pending reaper runs. Once for the collection
// and once for the index.
checkLog.containsWithAtLeastCount(primary, "Drop-pending ident is still in use", 2);

// Let the table drops succeed.
assert.commandWorked(primary.adminCommand({configureFailPoint: "WTDropEBUSY", mode: "off"}));

// Perform another write and another checkpoint to advance the checkpoint timestamp, triggering
// the reaper.
assert.commandWorked(db.getCollection("toWrite").insert({x: 1}));
assert.commandWorked(db.adminCommand({fsync: 1}));

// "The ident was successfully dropped".
checkLog.containsJson(primary, 6776600, {
    ident: function(ident) {
        return ident == collUri;
    }
});
checkLog.containsJson(primary, 6776600, {
    ident: function(ident) {
        return ident == indexUri;
    }
});

assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "off"}));

rst.stopSet();