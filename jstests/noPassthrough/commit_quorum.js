/**
 * Tests that the commit quorum can be changed during a two-phase index build.
 *
 * @tags: [requires_replication]
 */
(function() {
load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/check_log.js");

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

const testDB = replSet.getPrimary().getDB('test');
const coll = testDB.twoPhaseIndexBuild;

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 1000;
for (let i = 0; i < numDocs; i++) {
    bulk.insert({a: i, b: i});
}
assert.commandWorked(bulk.execute());

const collName = "createIndexes";

// Use createIndex(es) to build indexes and check the commit quorum.
let res = assert.commandWorked(testDB[collName].createIndex({x: 1}));
assert.eq(2, res.commitQuorum);

res = assert.commandWorked(testDB[collName].createIndex({y: 1}, {}, 1));
assert.eq(1, res.commitQuorum);

res = assert.commandWorked(testDB[collName].createIndexes([{i: 1}]));
assert.eq(2, res.commitQuorum);

res = assert.commandWorked(testDB[collName].createIndexes([{j: 1}], {}, 1));
assert.eq(1, res.commitQuorum);

replSet.waitForAllIndexBuildsToFinish(testDB.getName(), collName);

let awaitShell;
try {
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: "hangAfterIndexBuildSecondDrain", mode: "alwaysOn"}));

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

    assert.commandWorked(testDB.runCommand(
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
        testDB.adminCommand({configureFailPoint: "hangAfterIndexBuildSecondDrain", mode: "off"}));
}

// Wait for the parallel shell to complete.
awaitShell();

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

replSet.stopSet();
})();
