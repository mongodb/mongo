/**
 * Test that standalones are able to startup with 'recordPreImages' set in collection metadata; and
 * that 'recordPreImages' is inactive in standalone mode.
 *
 * @tags: [
 *     # Servers are restarted in this test and the data must be retained.
 *     requires_persistence,
 *     # This test uses a replica set and must avoid replica set incompatible test suites, like the
 *     # test suite that turns journaling off.
 *     requires_replication,
 * ]
 */

(function() {
'use strict';

/**
 * Fetch the collection information on database 'nodeDB' for collection 'collName'. Expects the
 * collection to exist.
 */
const findCollectionInfo = function(nodeDB, collName) {
    let collInfos = nodeDB.getCollectionInfos();
    assert.gt(collInfos.length, 0, "The database is empty");

    let collInfo = collInfos.filter(function(z) {
        return z.name == collName;
    });

    assert.eq(collInfo.length, 1);
    return collInfo[0];
};

/**
 * Prints out all of the oplog collection entries on 'node'.
 */
function printOplog(node) {
    let cursor = node.getDB('local').oplog.rs.find();
    while (cursor.hasNext()) {
        jsTest.log("Oplog entry: " + tojson(cursor.next()));
    }
}

/**
 * A --replSet server should be able to set 'recordPreImages' on a collection and then be restarted
 * in standalone mode successfully.
 */

jsTest.log("Starting up a 1-node replica set");

var rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = 'record_preimage_standalone_mode_test_db';
const collName = 'testColl';
let primary = rst.getPrimary();
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

jsTest.log("Creating a collection with 'recordPreImages' set to true and adding some data");

assert.commandWorked(testDB.runCommand({create: collName, recordPreImages: true}));
assert.eq(findCollectionInfo(testDB, collName).options.recordPreImages, true);

assert.commandWorked(testColl.insert({a: 1, b: 1}));
assert.commandWorked(testColl.update({a: 1}, {a: 2, b: 2}));
// Ensure all of the writes make it to disk before checkpointing below.
assert.commandWorked(testColl.update({a: 2}, {a: 3, b: 3}, {writeConcern: {w: 1, j: true}}));

jsTest.log("Forcing a checkpoint to be taken");

// Ensure that the standalone can recover all of the writes from the last checkpoint because
// standalone mode does not run recovery from the oplog. The last write with j:true write concern
// ensured that the data reached disk, and now fsync will checkpoint that data.
assert.commandWorked(primary.adminCommand({fsync: 1}));

jsTest.log("Restarting the replica set member as a standalone node");

printOplog(primary);  // Debugging aid.

let replicaSetNodeId = rst.getNodeId(primary);
let replicaSetNodeDbpath = primary.dbpath;
jsTest.log("replicaSetNodeId: " + replicaSetNodeId +
           ", replicaSetNodeDbpath: " + replicaSetNodeDbpath);

rst.stop(replicaSetNodeId);

let standaloneConn = MongoRunner.runMongod({
    dbpath: replicaSetNodeDbpath,
    noCleanData: true,
});

let standaloneDB = standaloneConn.getDB(dbName);
let standaloneColl = standaloneDB.getCollection(collName);
let standaloneOplogColl = standaloneConn.getDB('local').oplog.rs;

assert.eq(findCollectionInfo(standaloneDB, collName).options.recordPreImages, true);

/**
 * A standalone mode server should be able to do writes without triggering the 'recordPreImages'
 * feature because oplog entries are not written in standalone mode: the 'recordPreImages' setting
 * causes additional oplog entries to be written.
 */

const numOplogEntriesBefore = standaloneOplogColl.find().itcount();

jsTest.log(
    "Updating some data in the collection with 'recordPreImages' set to check that nothing " +
    "happens in standalone mode");

assert.commandWorked(standaloneColl.insert({c: 1, d: 1}));
assert.commandWorked(standaloneColl.update({c: 1}, {c: 2, d: 2}));
assert.commandWorked(standaloneColl.update({c: 2}, {c: 3, d: 3}));

jsTest.log(
    "Checking that no oplog entries were produced for 'recordPreImages': the feature is inactive");

printOplog(standaloneConn);  // Debugging aid.

const numOplogEntriesAfter = standaloneOplogColl.find().itcount();
assert.eq(numOplogEntriesBefore, numOplogEntriesAfter);

jsTest.log("Shutting down standalone");

MongoRunner.stopMongod(standaloneConn);

jsTest.log("Restarting the node as a replica set member again and doing some writes");

rst.start(replicaSetNodeId, {}, true /*restart*/);

primary = rst.getPrimary();
testDB = primary.getDB(dbName);
testColl = testDB.getCollection(collName);

// Check that everything is still working properly after being in standalone mode.
assert.eq(findCollectionInfo(testDB, collName).options.recordPreImages, true);

assert.commandWorked(testColl.update({a: 3}, {a: 4, b: 4}));
assert.commandWorked(testColl.update({a: 4}, {a: 5, b: 5}));

jsTest.log("Shutting down replica set");

rst.stopSet();
}());
