/**
 * This test tests that initial sync succeeds when the sync source's oplog rolls over before the
 * destination node reaches the oplog apply phase. It adds a new secondary to a replicaset and then
 * pauses the initial sync before it copies the databases but after it starts to fetch and buffer
 * oplog entries. The primary then fills up its oplog until it rolls over. At that point
 * initial sync is resumed and we assert that it succeeds and that all of the inserted documents
 * are there.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getFirstOplogEntry} from "jstests/replsets/rslib.js";

let name = "initial_sync_oplog_rollover";
let replSet = new ReplSetTest({
    name: name,
    // This test requires a third node (added later) to be syncing when the oplog rolls
    // over. Rolling over the oplog requires a majority of nodes to have confirmed and
    // persisted those writes. Set the syncdelay to one to speed up checkpointing.
    nodeOptions: {syncdelay: 1},
    nodes: [{rsConfig: {priority: 1}}, {rsConfig: {priority: 0}}],
});

let oplogSizeOnPrimary = 1; // size in MB
replSet.startSet({oplogSize: oplogSizeOnPrimary});
replSet.initiate();
let primary = replSet.getPrimary();

let coll = primary.getDB("test").foo;
assert.commandWorked(coll.insert({a: 1}));

let firstOplogEntry = getFirstOplogEntry(primary);

// Add a secondary node but make it hang before copying databases.
let secondary = replSet.add();
secondary.setSecondaryOk();

let failPoint = configureFailPoint(secondary, "initialSyncHangBeforeCopyingDatabases");
replSet.reInitiate();

failPoint.wait();

// Keep inserting large documents until they roll over the oplog.
const largeStr = "aaaaaaaa".repeat(4 * 1024 * oplogSizeOnPrimary);
let i = 0;
while (bsonWoCompare(getFirstOplogEntry(primary), firstOplogEntry) === 0) {
    assert.commandWorked(coll.insert({a: 2, x: i++, long_str: largeStr}));
    sleep(100);
}

failPoint.off();

replSet.awaitSecondaryNodes(200 * 1000);

assert.eq(i, secondary.getDB("test").foo.count({a: 2}), "collection successfully synced to secondary");

assert.eq(
    0,
    secondary.getDB("local")["temp_oplog_buffer"].find().itcount(),
    "Oplog buffer was not dropped after initial sync",
);
replSet.stopSet();
