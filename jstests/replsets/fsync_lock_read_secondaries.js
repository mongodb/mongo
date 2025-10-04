/* @file : jstests/fsync_lock_read_secondaries.js
 *
 * SERVER 4243 : If there is a pending write due to an fsync lock, all reads are blocked
 *
 * This test validates part of SERVER-4243 ticket. Allow reading on secondaries with fsyncLock
 * mode enabled. Previously oplog application would cause blocking.
 * The corresponding commit :
 * https://github.com/mongodb/mongo/commit/73aa870d129bd7d51de946b91c16cc056aaacbc7
 */

/*
 * 1) Create a replica set.
 * 2) Add some documents to primary.
 * 3) Wait until the secondary nodes are in state "SECONDARY".
 * 4) Set secondaryOk on secondary.
 * 5) Take the fsync lock on a secondary. This will stop replication.
 * 6) Insert some more documents to primary.
 * 7) Expect to be able to read from the secondary; the count of documents should
 *    be equal to the number of documents added in step 2.
 * 8) Release the fsync lock. This will resume replication.
 * 9) Soon, the secondary should be applying the oplog again, which we should
 *    witness as an increase in the count of documents stored on the secondary.
 */
// Load utility methods for replica set tests
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForAllMembers} from "jstests/replsets/rslib.js";

let replTest = new ReplSetTest({name: "testSet", nodes: 2, oplogSize: 5});
// Start each mongod in the replica set. Returns a list of nodes
let nodes = replTest.startSet();
// This will wait for initiation
replTest.initiate();
let primary = replTest.getPrimary();

// The default WC is majority and fsyncLock will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

let ret = primary.getDB("admin").fsyncLock();
if (!ret.ok) {
    assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
    jsTestLog("Storage Engine does not support fsyncLock, so bailing");
    quit();
}
primary.getDB("admin").fsyncUnlock();

var docNum = 100;
for (var i = 0; i < docNum; i++) {
    primary.getDB("foo").bar.save({a: i});
}
waitForAllMembers(primary.getDB("foo"));
replTest.awaitReplication();

// Calling getPrimary also populates '_secondaries'.
let secondaries = replTest.getSecondaries();
secondaries[0].setSecondaryOk();

assert.commandWorked(secondaries[0].getDB("admin").runCommand({fsync: 1, lock: 1}));
var docNum = 1000;
for (var i = 0; i < docNum; i++) {
    primary.getDB("foo").bar.save({a: i});
}
// Issue a read query on the secondary while holding the fsync lock.
// This is what we are testing. Previously this would block. After the fix
// this should work just fine.
let secondary0count = secondaries[0].getDB("foo").bar.find().itcount();
assert.eq(secondary0count, 100, "Doc count in fsync lock wrong. Expected (=100), found " + secondary0count);
assert(secondaries[0].getDB("admin").fsyncUnlock().ok);

// The secondary should have equal or more documents than what it had before.
assert.soon(function () {
    return secondaries[0].getDB("foo").bar.find().itcount() > 100;
}, "count of documents stored on the secondary did not increase");
replTest.stopSet();
