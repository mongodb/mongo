/**
 * Test speculative majority reads using the 'find' command.
 *
 * Speculative majority reads allow the server to provide "majority" read guarantees without storage
 * engine support for reading from a historical snapshot. Instead of reading historical, majority
 * committed data, we just read the newest data available on a node, and then, before returning to a
 * client, block until we know the data has become majority committed. Currently this is an internal
 * feature used only by change streams.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.
load("jstests/libs/parallelTester.js");      // for Thread.

let name = "speculative_majority_find";
let replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        enableMajorityReadConcern: 'false',
        // Increase log verbosity so we can see all commands that run on the server.
        setParameter: {logComponentVerbosity: tojson({command: 2})}
    }
});
replTest.startSet();
replTest.initiate();

let dbName = name;
let collName = "coll";

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

let primaryDB = primary.getDB(dbName);
let secondaryDB = secondary.getDB(dbName);
let primaryColl = primaryDB[collName];
// Create a collection.
assert.commandWorked(primaryColl.insert({}, {writeConcern: {w: "majority"}}));

//
// Test basic reads with speculative majority.
//

// Pause replication on the secondary so that writes won't majority commit.
stopServerReplication(secondary);
assert.commandWorked(primaryColl.insert({_id: 1}));

jsTestLog("Do a speculative majority read that should time out.");
let res = primaryDB.runCommand({
    find: collName,
    readConcern: {level: "majority"},
    filter: {_id: 1},
    allowSpeculativeMajorityRead: true,
    maxTimeMS: 5000
});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

restartServerReplication(secondary);
replTest.awaitReplication();

jsTestLog("Do a speculative majority read that should succeed.");
res = primaryDB.runCommand({
    find: collName,
    readConcern: {level: "majority"},
    filter: {_id: 1},
    allowSpeculativeMajorityRead: true
});
assert.commandWorked(res);
assert.eq(res.cursor.firstBatch.length, 1);
assert.eq(res.cursor.firstBatch[0], {_id: 1});

//
// Test that blocked reads can succeed when a write majority commits.
//

// Pause replication on the secondary so that writes won't majority commit.
stopServerReplication(secondary);
assert.commandWorked(primaryColl.insert({_id: 2}));

jsTestLog("Do a speculative majority that should block until write commits.");
let speculativeRead = new Thread(function(host, dbName, collName) {
    const nodeDB = new Mongo(host).getDB(dbName);
    return nodeDB.runCommand({
        find: collName,
        readConcern: {level: "majority"},
        filter: {_id: 2},
        allowSpeculativeMajorityRead: true
    });
}, primary.host, dbName, collName);
speculativeRead.start();

// Wait for the read to start on the server.
assert.soon(() => primaryDB.currentOp({ns: primaryColl.getFullName(), "command.find": collName})
                      .inprog.length === 1);

// Let the previous write commit.
restartServerReplication(secondary);
assert.commandWorked(
    primaryColl.insert({_id: "commit_last_write"}, {writeConcern: {w: "majority"}}));

// Make sure the read finished and returned correct results.
speculativeRead.join();
res = speculativeRead.returnData();
assert.commandWorked(res);
assert.eq(res.cursor.firstBatch.length, 1);
assert.eq(res.cursor.firstBatch[0], {_id: 2});

//
// Test 'afterClusterTime' reads with speculative majority.
//
stopServerReplication(secondary);

// Insert a document on the primary and record the response.
let writeRes = primaryDB.runCommand({insert: collName, documents: [{_id: 3}]});
assert.commandWorked(writeRes);

jsTestLog(
    "Do a speculative majority read on primary with 'afterClusterTime' that should time out.");
res = primaryDB.runCommand({
    find: collName,
    readConcern: {level: "majority", afterClusterTime: writeRes.operationTime},
    filter: {_id: 3},
    $clusterTime: writeRes.$clusterTime,
    allowSpeculativeMajorityRead: true,
    maxTimeMS: 5000
});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

jsTestLog(
    "Do a speculative majority read on secondary with 'afterClusterTime' that should time out.");
res = secondaryDB.runCommand({
    find: collName,
    readConcern: {level: "majority", afterClusterTime: writeRes.operationTime},
    filter: {_id: 3},
    $clusterTime: writeRes.$clusterTime,
    allowSpeculativeMajorityRead: true,
    maxTimeMS: 5000
});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

// Let the previous write majority commit.
restartServerReplication(secondary);
replTest.awaitReplication();

jsTestLog("Do a speculative majority read with 'afterClusterTime' that should succeed.");
res = primaryDB.runCommand({
    find: collName,
    readConcern: {level: "majority", afterClusterTime: writeRes.operationTime},
    filter: {_id: 3},
    $clusterTime: res.$clusterTime,
    allowSpeculativeMajorityRead: true
});
assert.commandWorked(res);
assert.eq(res.cursor.firstBatch.length, 1);
assert.eq(res.cursor.firstBatch[0], {_id: 3});

replTest.stopSet();
})();
