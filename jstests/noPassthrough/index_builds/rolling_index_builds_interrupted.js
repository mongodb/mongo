/**
 * Builds an index on a subset of nodes in a rolling fashion. Tests that building the same index
 * with a primary that doesn't have the index logs a message on the secondaries that the index build
 * commit quorum may not be achieved.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

function clearLogInNodes(nodes) {
    nodes.forEach(node => assert.commandWorked(node.adminCommand({clearLog: "global"})));
}

// Set up the replica set. We need to set "oplogApplicationEnforcesSteadyStateConstraints=false" as
// we'll be violating the index build process by having the index already built on the secondary
// nodes. This is false by default outside of our testing.
const replTest = new ReplSetTest({
    nodes: 3,
    nodeOptions: {setParameter: {oplogApplicationEnforcesSteadyStateConstraints: false}}
});
const nodes = replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 't';

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);
let primaryColl = primaryDB.getCollection(collName);

// Populate collection to avoid empty collection optimization.
const numDocs = 100;
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(primaryColl.insert({x: i}));
}

// Make sure the documents make it to the secondaries.
replTest.awaitReplication();

const secondaries = replTest.getSecondaries();
assert.eq(nodes.length - 1,
          secondaries.length,
          'unexpected number of secondaries: ' + tojson(secondaries));

const standalonePort = allocatePort();
jsTestLog('Standalone server will listen on port: ' + standalonePort);

// Build the index on the secondaries only.
IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, secondaries[0], standalonePort, dbName, collName, {x: 1}, 'x_1');
IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, secondaries[1], standalonePort, dbName, collName, {x: 1}, 'x_1');

replTest.awaitNodesAgreeOnPrimary(
    replTest.kDefaultTimeoutMS, replTest.nodes, replTest.getNodeId(primary));

jsTestLog('Build index on the primary as part of the replica set: ' + primary.host);
let createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {x: 1}, {name: 'x_1'}, [ErrorCodes.Interrupted]);

// When the index build starts, find its op id. This will be the op id of the client connection, not
// the thread pool task managed by IndexBuildsCoordinatorMongod.
const filter = {
    "desc": {$regex: /conn.*/}
};
let opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'x_1', filter);

checkLog.containsJson(secondaries[0], 7731100);
checkLog.containsJson(secondaries[1], 7731100);
clearLogInNodes(secondaries);

assert.commandWorked(primaryDB.killOp(opId));
createIdx();

// Test building multiple indexes, some of which exist on the secondary.
createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), [{x: 1}, {y: 1}], {}, [ErrorCodes.Interrupted]);

checkLog.containsJson(secondaries[0], 7731101);
checkLog.containsJson(secondaries[1], 7731101);
clearLogInNodes(secondaries);

opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'x_1', filter);
assert.commandWorked(primaryDB.killOp(opId));

createIdx();

// Test secondary, which has no awareness of the index build, becoming primary.
// 'voteCommitIndexBuild' from secondaries should fail, as well as 'setIndexCommitQuorum'. Once the
// old primary becomes primary again and the commit quorum is properly fixed, the index should
// successfully commit.
IndexBuildTest.pauseIndexBuilds(primaryDB);
createIdx = IndexBuildTest.startIndexBuild(primary,
                                           primaryColl.getFullName(),
                                           {x: 1},
                                           {name: 'x_1'},
                                           [ErrorCodes.InterruptedDueToReplStateChange]);

opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'x_1', filter);

checkLog.containsJson(secondaries[0], 7731100);
checkLog.containsJson(secondaries[1], 7731100);
clearLogInNodes(secondaries);

// Step-up one of the secondaries.
let newPrimary = secondaries[0];
replTest.stepUp(newPrimary);

IndexBuildTest.resumeIndexBuilds(primaryDB);
//'voteCommitIndexBuild' command failed
checkLog.containsJson(primary, 3856202);

// The new primary has no awareness of the index build, setIndexCommitQuorum will fail.
assert.commandFailedWithCode(
    newPrimary.getDB(dbName).runCommand(
        {setIndexCommitQuorum: collName, indexNames: ["x_1"], commitQuorum: 1}),
    [ErrorCodes.IndexNotFound]);

// Step up old primary, which is aware of the index build.
replTest.stepUp(primary);

assert.commandWorked(
    primaryDB.runCommand({setIndexCommitQuorum: collName, indexNames: ["x_1"], commitQuorum: 1}));

IndexBuildTest.waitForIndexBuildToStop(primaryDB, collName, "x_1");

// Index build: commit quorum satisfied
checkLog.containsJson(primary, 3856201);

createIdx();

TestData.skipCheckDBHashes = true;
replTest.stopSet();