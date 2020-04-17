/**
 * Tests that the commit quorum can be changed during a two-phase index build.
 *
 * @tags: [requires_replication]
 */
(function() {
load("jstests/noPassthrough/libs/index_build.js");

const replSet = new ReplSetTest({
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

// Allow the createIndexes command to use the index builds coordinator in single-phase mode.
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
if (!(IndexBuildTest.supportsTwoPhaseIndexBuild(primary) &&
      IndexBuildTest.indexBuildCommitQuorumEnabled(primary))) {
    jsTestLog(
        'Skipping test because two phase index build and index build commit quorum are not supported.');
    replSet.stopSet();
    return;
}

const testDB = primary.getDB('test');
const coll = testDB.twoPhaseIndexBuild;

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 1000;
for (let i = 0; i < numDocs; i++) {
    bulk.insert({a: i, b: i});
}
assert.commandWorked(bulk.execute());

const collName = "createIndexes";
// This test depends on using the IndexBuildsCoordinator to build this index, which as of
// SERVER-44405, will not occur in this test unless the collection is created beforehand.
assert.commandWorked(testDB.runCommand({create: collName}));

// Use createIndex(es) to build indexes and check the commit quorum default.
let res = assert.commandWorked(testDB[collName].createIndex({x: 1}));
assert.eq("votingMembers", res.commitQuorum);

res = assert.commandWorked(testDB[collName].createIndex({y: 1}, {}, 1));
assert.eq(1, res.commitQuorum);

// Use createIndex(es) to build indexes and check the commit quorum default.
res = assert.commandWorked(testDB[collName].createIndexes([{i: 1}]));
assert.eq("votingMembers", res.commitQuorum);

res = assert.commandWorked(testDB[collName].createIndexes([{j: 1}], {}, 1));
assert.eq(1, res.commitQuorum);

replSet.waitForAllIndexBuildsToFinish(testDB.getName(), collName);

let awaitShell;
try {
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: "hangAfterIndexBuildFirstDrain", mode: "alwaysOn"}));

    // Starts parallel shell to run the command that will hang.
    awaitShell = startParallelShell(function() {
        // Use the index builds coordinator for a two-phase index build.
        assert.commandWorked(db.runCommand({
            createIndexes: 'twoPhaseIndexBuild',
            indexes: [{key: {a: 1}, name: 'a_1'}],
            commitQuorum: "majority"
        }));
    }, testDB.getMongo().port);

    checkLog.containsWithCount(replSet.getPrimary(), "Waiting for index build to complete", 5);

    // Test setting various commit quorums on the index build in our two node replica set.
    assert.commandFailed(testDB.runCommand(
        {setIndexCommitQuorum: 'twoPhaseIndexBuild', indexNames: ['a_1'], commitQuorum: 3}));
    assert.commandFailed(testDB.runCommand({
        setIndexCommitQuorum: 'twoPhaseIndexBuild',
        indexNames: ['a_1'],
        commitQuorum: "someTag"
    }));
    // setIndexCommitQuorum should fail as it is illegal to disable commit quorum for in-progress
    // index builds with commit quorum enabled.
    assert.commandFailed(testDB.runCommand(
        {setIndexCommitQuorum: 'twoPhaseIndexBuild', indexNames: ['a_1'], commitQuorum: 0}));

    assert.commandWorked(testDB.runCommand(
        {setIndexCommitQuorum: 'twoPhaseIndexBuild', indexNames: ['a_1'], commitQuorum: 2}));
    assert.commandWorked(testDB.runCommand({
        setIndexCommitQuorum: 'twoPhaseIndexBuild',
        indexNames: ['a_1'],
        commitQuorum: "majority"
    }));
} finally {
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangAfterIndexBuildFirstDrain", mode: "off"}));
}

// Wait for the parallel shell to complete.
awaitShell();

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

replSet.stopSet();
})();
