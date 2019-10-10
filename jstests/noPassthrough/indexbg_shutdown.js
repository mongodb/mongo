/**
 * Starts a replica set, builds an index in background,
 * shuts down a secondary while it's building that index, and confirms that the secondary
 * shuts down cleanly, without an fassert.
 * Also confirms that killOp has no effect on the background index build on the secondary.
 * @tags: [requires_replication]
 */

(function() {
"use strict";

load('jstests/libs/check_log.js');
load('jstests/noPassthrough/libs/index_build.js');

var dbname = 'bgIndexSec';
var collection = 'bgIndexShutdown';
var size = 100;

// Set up replica set
const replTest = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
const nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getPrimary();
var second = replTest.getSecondary();

var secondaryId = replTest.getNodeId(second);

var masterDB = master.getDB(dbname);
var secondDB = second.getDB(dbname);

masterDB.dropDatabase();
jsTest.log("creating test data " + size + " documents");
const masterColl = masterDB.getCollection(collection);
var bulk = masterColl.initializeUnorderedBulkOp();
for (var i = 0; i < size; ++i) {
    bulk.insert({i: i, j: i * i});
}
assert.commandWorked(bulk.execute());

IndexBuildTest.pauseIndexBuilds(second);

jsTest.log("Starting background indexing");
// Using a write concern to wait for the background index build to finish on the primary node
// and be started on the secondary node (but not completed, as the oplog entry is written before
// the background index build finishes).
const indexSpecs = [
    {key: {i: -1, j: -1}, name: 'ij1', background: true},
    {key: {i: -1, j: 1}, name: 'ij2', background: true},
    {key: {i: 1, j: -1}, name: 'ij3', background: true},
    {key: {i: 1, j: 1}, name: 'ij4', background: true}
];

assert.commandWorked(masterDB.runCommand({
    createIndexes: collection,
    indexes: indexSpecs,
}));
const indexes = masterColl.getIndexes();
// Number of indexes passed to createIndexes plus one for the _id index.
assert.eq(indexSpecs.length + 1, indexes.length, tojson(indexes));

// Wait for index builds to start on the secondary.
const opId = IndexBuildTest.waitForIndexBuildToStart(secondDB);
jsTestLog('Index builds started on secondary. Op ID of one of the builds: ' + opId);

// Kill the index build. This should have no effect.
assert.commandWorked(secondDB.killOp(opId));

// There should be a message for each index we tried to create.
checkLog.containsWithCount(
    replTest.getSecondary(),
    'index build: starting on ' + masterColl.getFullName() + ' properties: { v: 2, key: { i:',
    indexSpecs.length);

jsTest.log("Restarting secondary to retry replication");

// Secondary should restart cleanly.
assert.commandWorked(second.adminCommand(
    {configureFailPoint: 'leaveIndexBuildUnfinishedForShutdown', mode: 'alwaysOn'}));
IndexBuildTest.resumeIndexBuilds(second);
replTest.restart(secondaryId, {}, /*wait=*/true);

// There should again be a message for each index we tried to create, because the server
// restarts the interrupted index build upon process startup. Note, the RAMLog is reset on
// restart, so there should just be one set of messages in the RAMLog after restart, even though
// the message was logged twice in total.
checkLog.containsWithCount(
    replTest.getSecondary(),
    'index build: starting on ' + masterColl.getFullName() + ' properties: { v: 2, key: { i:',
    indexSpecs.length);

replTest.stopSet();
}());
