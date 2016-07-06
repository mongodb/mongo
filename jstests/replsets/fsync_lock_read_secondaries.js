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
 * 2) Add some documents to master.
 * 3) Wait until the secondary nodes are in state "SECONDARY".
 * 4) Set slaveOk on secondary.
 * 5) Take the fsync lock on a secondary. This will stop replication.
 * 6) Insert some more documents to master.
 * 7) Expect to be able to read from the secondary; the count of documents should
 *    be equal to the number of documents added in step 2.
 * 8) Release the fsync lock. This will resume replication.
 * 9) Soon, the secondary should be applying the oplog again, which we should
 *    witness as an increase in the count of documents stored on the secondary.
 */
(function() {
    "use strict";
    // Load utility methods for replica set tests
    load("jstests/replsets/rslib.js");

    var replTest = new ReplSetTest({name: 'testSet', nodes: 2, oplogSize: 5});
    // Start each mongod in the replica set. Returns a list of nodes
    var nodes = replTest.startSet();
    // This will wait for initiation
    replTest.initiate();
    var master = replTest.getPrimary();

    var ret = master.getDB("admin").fsyncLock();
    if (!ret.ok) {
        assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
        jsTestLog("Storage Engine does not support fsyncLock, so bailing");
        return;
    }
    master.getDB("admin").fsyncUnlock();

    var docNum = 100;
    for (var i = 0; i < docNum; i++) {
        master.getDB("foo").bar.save({a: i});
    }
    waitForAllMembers(master.getDB("foo"));
    replTest.awaitReplication();

    // Calling getPrimary also makes available the liveNodes structure, which looks like this:
    // liveNodes = {master: masterNode, slaves: [slave1, slave2] }
    var slaves = replTest.liveNodes.slaves;
    slaves[0].setSlaveOk();

    assert.commandWorked(slaves[0].getDB("admin").runCommand({fsync: 1, lock: 1}));
    var docNum = 1000;
    for (var i = 0; i < docNum; i++) {
        master.getDB("foo").bar.save({a: i});
    }
    // Issue a read query on the secondary while holding the fsync lock.
    // This is what we are testing. Previously this would block. After the fix
    // this should work just fine.
    var slave0count = slaves[0].getDB("foo").bar.find().itcount();
    assert.eq(
        slave0count, 100, "Doc count in fsync lock wrong. Expected (=100), found " + slave0count);
    assert(slaves[0].getDB("admin").fsyncUnlock().ok);

    // The secondary should have equal or more documents than what it had before.
    assert.soon(function() {
        return slaves[0].getDB("foo").bar.find().itcount() > 100;
    }, "count of documents stored on the secondary did not increase");
    replTest.stopSet();
}());
