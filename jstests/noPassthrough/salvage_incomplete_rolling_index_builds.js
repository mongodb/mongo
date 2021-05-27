/**
 * Tests that an incomplete rolling index build can be salvaged when building the same index across
 * a replica set when one or more secondaries already have the index built.
 *
 * By default, the commit quorum is "votingMembers", which is all data-bearing replica set members.
 * The issue arises when starting an index build on the primary which the secondaries have already
 * built to completion. The secondaries would treat the "startIndexBuild" oplog entry as a no-op and
 * return immediately. This causes the secondaries to skip voting for the index build to be
 * committed or aborted, which prevents the primary from satisfying the commit quorum. The
 * "setIndexCommitQuorum" command can be used to modify the commit quorum of in-progress index
 * builds to get out of this situation.
 *
 * Note: this is an incorrect way to build indexes, but demonstrates that "setIndexCommitQuorum" can
 * get a user out of this situation if they end up in it.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

// Set up the replica set. We need to set "oplogApplicationEnforcesSteadyStateConstraints=false" as
// we'll be violating the index build process by having the index already built on the secondary
// nodes. This is false by default outside of our testing.
const replTest = new ReplSetTest({
    nodes: 3,
    nodeOptions: {setParameter: {oplogApplicationEnforcesSteadyStateConstraints: false}}
});

const nodes = replTest.startSet();
replTest.initiate();

const dbName = "test";
const collName = "t";

// Populate collection to avoid empty collection optimization.
function insertDocs(coll, startId, numDocs) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        const v = startId + i;
        bulk.insert({_id: v, a: v, b: v});
    }
    assert.commandWorked(bulk.execute());
}

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);
let coll = primaryDB.getCollection(collName);

const numDocs = 100;
insertDocs(coll, 0, numDocs);
assert.eq(numDocs, coll.count(), "unexpected number of documents after bulk insert.");

// Make sure the documents make it to the secondaries.
replTest.awaitReplication();

const secondaries = replTest.getSecondaries();
assert.eq(nodes.length - 1,
          secondaries.length,
          "unexpected number of secondaries: " + tojson(secondaries));

const standalonePort = allocatePort();
jsTestLog("Standalone server will listen on port: " + standalonePort);

function buildIndexOnNodeAsStandalone(node) {
    jsTestLog("A. Restarting as standalone: " + node.host);
    replTest.stop(node, /*signal=*/null, /*opts=*/null, {forRestart: true, waitpid: true});
    const standalone = MongoRunner.runMongod({
        restart: true,
        dbpath: node.dbpath,
        port: standalonePort,
        setParameter: {
            disableLogicalSessionCacheRefresh: true,
            ttlMonitorEnabled: false,
        },
    });
    if (jsTestOptions().keyFile) {
        assert(jsTest.authenticate(standalone),
               "Failed authentication during restart: " + standalone.host);
    }

    jsTestLog("B. Building index on standalone: " + standalone.host);
    const standaloneDB = standalone.getDB(dbName);
    const standaloneColl = standaloneDB.getCollection(collName);
    assert.commandWorked(standaloneColl.createIndex({b: 1}, {name: "rolling_index_b_1"}));

    jsTestLog("C. Restarting as replica set node: " + node.host);
    MongoRunner.stopMongod(standalone);
    replTest.restart(node);
    replTest.awaitReplication();
}

buildIndexOnNodeAsStandalone(secondaries[0]);

jsTestLog("D. Repeat the procedure for the remaining secondary: " + secondaries[1].host);
buildIndexOnNodeAsStandalone(secondaries[1]);

replTest.awaitNodesAgreeOnPrimary(
    replTest.kDefaultTimeoutMS, replTest.nodes, replTest.getNodeId(primary));

// The primary does not perform the rolling index build procedure. Instead, the createIndex command
// is issued against the replica set, where both the secondaries have already built the index.
jsTestLog("E. Build index on the primary as part of the replica set: " + primary.host);
let awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {b: 1}, {name: "rolling_index_b_1"});
IndexBuildTest.waitForIndexBuildToStart(primaryDB, coll.getName(), "rolling_index_b_1");

checkLog.containsJson(primary, 3856203);  // Waiting for the commit quorum to be satisfied.

// The drain phase periodically runs while waiting for the commit quorum to be satisfied.
insertDocs(coll, numDocs, numDocs * 2);
checkLog.containsJson(primary, 20689, {index: "rolling_index_b_1"});  // Side writes drained.

// As the secondaries won't vote, we change the commit quorum to 1. This will allow the primary to
// proceed with committing the index build.
assert.commandWorked(primaryDB.runCommand(
    {setIndexCommitQuorum: collName, indexNames: ["rolling_index_b_1"], commitQuorum: 1}));
awaitIndexBuild();

replTest.stopSet();
}());
