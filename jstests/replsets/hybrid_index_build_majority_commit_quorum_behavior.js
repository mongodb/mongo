/*
 * Test to verify when majority commit quorum is enabled for index build, the primary index builder
 * should not commit the index until majority of nodes finishes building their index.
 */
(function() {

"use strict";
load("jstests/replsets/rslib.js");
load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/parallel_shell_helpers.js");  // funWithArgs

var rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const dbName = jsTest.name();
const collName = "coll";
const indexName = "x_1";

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

if (!(IndexBuildTest.supportsTwoPhaseIndexBuild(primary) &&
      IndexBuildTest.indexBuildCommitQuorumEnabled(primary))) {
    jsTestLog(
        'Skipping test because two phase index build and index build commit quorum are not supported.');
    rst.stopSet();
    return;
}

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const OplogColl = primary.getDB("local")["oplog.rs"];
const docFilter = {
    "ns": dbName + ".$cmd",
    "o.commitIndexBuild": {$exists: true},
    "o.indexes.0.name": indexName
};

jsTestLog("Do some document writes.");
assert.commandWorked(
        primaryColl.insert({_id: 0, x: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

function isIndexBuildInProgress(conn, indexName) {
    jsTestLog("Running collection stats on " + conn.host);
    const coll = conn.getDB(dbName)[collName];
    var stats = assert.commandWorked(coll.stats());
    return Array.contains(stats["indexBuilds"], indexName);
}

function sanityChecks() {
    // Check to see commitIndexBuild oplog entry is not present.
    assert.isnull(OplogColl.findOne(docFilter),
                  "Able to find commitIndexBuild oplog entry. Filter:" + tojson(docFilter));

    // Check if the index build is in progress on both the nodes.
    assert.eq(true, isIndexBuildInProgress(primary, indexName));
    assert.eq(true, isIndexBuildInProgress(secondary, indexName));

    // Check if 'x_1' index is not yet ready.
    IndexBuildTest.assertIndexes(primaryColl, 2, ['_id_'], ['x_1']);
}

function pauseIndexBuild(conn, failpoint) {
    assert.commandWorked(conn.adminCommand({configureFailPoint: failpoint, mode: 'alwaysOn'}));
}

function resumeIndexBuild(conn, failpoint) {
    assert.commandWorked(conn.adminCommand({configureFailPoint: failpoint, mode: 'off'}));
}

function createIndex(dbName, collName, indexName) {
    jsTestLog("Create index '" + indexName + "'.");
    assert.commandWorked(db.getSiblingDB(dbName).runCommand({
        createIndexes: collName,
        indexes: [{name: indexName, key: {x: 1}}],
        commitQuorum: "majority"
    }));
}

// Make secondary index build to hang after collection scan phase.
pauseIndexBuild(secondary, "hangAfterIndexBuildDumpsInsertsFromBulk");
// Start the index build on primary in parallel shell.
const joinCreateIndexThread =
    startParallelShell(funWithArgs(createIndex, dbName, collName, indexName), primary.port);

jsTestLog("Waiting for Collection scan phase to complete");
checkLog.contains(secondary, "Hanging after dumping inserts from bulk builder");
sanityChecks();
pauseIndexBuild(secondary, "hangAfterIndexBuildFirstDrain");
resumeIndexBuild(secondary, "hangAfterIndexBuildDumpsInsertsFromBulk");

jsTestLog("Waiting for first drain phase to complete");
checkLog.contains(secondary, "Hanging after index build first drain");
sanityChecks();
pauseIndexBuild(secondary, "hangAfterIndexBuildSecondDrain");
resumeIndexBuild(secondary, "hangAfterIndexBuildFirstDrain");

jsTestLog("Waiting for second drain phase to complete");
checkLog.contains(secondary, "Hanging after index build second drain");
sanityChecks();
// Make secondary to resume index build. This should allow secondary to vote
// and make primary to commit index build.
resumeIndexBuild(secondary, "hangAfterIndexBuildSecondDrain");

jsTestLog("Wait for create index thread to join");
joinCreateIndexThread();
rst.awaitReplication();

jsTestLog("Check Primary to see if it contains 'commitIndexBuild' oplog entry ");
assert(OplogColl.findOne(docFilter),
       "Not able to find a matching oplog entry. Filter:" + tojson(docFilter));

// Sanity checks to see if the index build still runs on primary and secondary.
assert.eq(false, isIndexBuildInProgress(primary, indexName));
assert.eq(false, isIndexBuildInProgress(secondary, indexName));

// check to see if the index was successfully created.
IndexBuildTest.assertIndexes(primaryColl, 2, ['_id_', 'x_1']);

rst.stopSet();
})();
