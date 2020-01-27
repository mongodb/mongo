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
      IndexBuildTest.supportsIndexBuildMajorityCommitQuorum(primary))) {
    jsTestLog(
        'Skipping test because two phase index build and index build majority commit quorum are not supported.');
    rst.stopSet();
    return;
}

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

jsTestLog("Do some document writes.");
assert.commandWorked(
        primaryColl.insert({_id: 0, x: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

// Make the secondary index build to hang after collection scan phase.
IndexBuildTest.pauseIndexBuilds(secondary);

function createIndex(dbName, collName, indexName) {
    jsTestLog("Create index '" + indexName + "'.");
    assert.commandWorked(db.getSiblingDB(dbName).runCommand(
        {createIndexes: collName, indexes: [{name: indexName, key: {x: 1}}]}));
}

function isIndexBuildInProgress(conn, indexName) {
    jsTestLog("Running collection stats on " + conn.host);
    const coll = conn.getDB(dbName)[collName];
    var stats = assert.commandWorked(coll.stats());
    return Array.contains(stats["indexBuilds"], indexName);
}

const joinCreateIndexThread =
    startParallelShell(funWithArgs(createIndex, dbName, collName, indexName), primary.port);

// Wait for Index thread to join.
jsTestLog("Wait for create index thread to join");
joinCreateIndexThread();

// Check the primary oplog to see if it contains commitIndexBuild oplog entry for index "x_1".
// TODO SERVER-45001: After this server ticket, this test has to be updated as the primary won't
// generate a commitIndexBuild oplog entry until majority of nodes finished building indexes.
const OplogColl = primary.getDB("local")["oplog.rs"];
const docFilter = {
    "ns": dbName + ".$cmd",
    "o.commitIndexBuild": {$exists: true},
    "o.indexes.0.name": indexName
};
jsTestLog("Check Primary to see if it contains 'commitIndexBuild' oplog entry ");
assert(OplogColl.findOne(docFilter),
       "Not able to find a matching oplog entry. Filter:" + tojson(docFilter));

// Sanity checks to see if the index build runs on primary and secondary.
assert.eq(false, isIndexBuildInProgress(primary, indexName));
assert.eq(true, isIndexBuildInProgress(secondary, indexName));

// Disable fail point to unblock secondary from finishing the index build.
IndexBuildTest.resumeIndexBuilds(secondary);

rst.awaitReplication();

// Sanity checks to see if the index build runs on primary and secondary.
assert.eq(false, isIndexBuildInProgress(primary, indexName));
assert.eq(false, isIndexBuildInProgress(secondary, indexName));

rst.stopSet();
})();