/**
 * Test that the buildIndexes:false replica set config option behaves correctly with various commit
 * quorum options.
 *
 * @tags: [
 *  requires_fcv_44,
 * ]
 */

(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');  // for assertIndexes().

// Skip db hash check because secondary will have different number of indexes due to
// buildIndexes:false.
TestData.skipCheckDBHashes = true;

const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: [
        {},
        {},
        {rsConfig: {priority: 0, buildIndexes: false}},
    ]
});
replTest.startSet();
replTest.initiate();

const dbName = "buildIndexes";
const collName = "test";
const primaryDb = replTest.getPrimary().getDB(dbName);

// Ensure the collection is populated. Index builds on empty collection circumvent the two-phase
// index build machinery, and we want to exercise that in this test.
for (let i = 0; i < 100; i++) {
    primaryDb[collName].insert({x: 1, y: "abc", c: 1});
}

// The default commit quorum is 'votingMembers', and this index build should fail early because the
// voting buildIndexes:false node will never build the index.
assert.commandFailedWithCode(primaryDb.runCommand({
    createIndexes: collName,
    indexes: [{key: {y: 1}, name: 'y_1_default_commitQuorum'}],
}),
                             ErrorCodes.UnsatisfiableCommitQuorum);

// With a commit quorum that includes all nodes, the quorum is unsatisfiable for the same reason as
// 'votingMembers'.
assert.commandFailedWithCode(primaryDb.runCommand({
    createIndexes: collName,
    indexes: [{key: {y: 1}, name: 'y_1_commitQuorum_3'}],
    commitQuorum: 3,
}),
                             ErrorCodes.UnsatisfiableCommitQuorum);

// This commit quorum does not need to include the buildIndexes:false node so it should be
// satisfiable.
const indexName = 'y_1_commitQuorum_majority';
assert.commandWorked(primaryDb.runCommand({
    createIndexes: collName,
    indexes: [{key: {y: 1}, name: indexName}],
    commitQuorum: 'majority',
}));

replTest.awaitReplication();

let secondaryDbs = [];
replTest.getSecondaries().forEach((conn) => {
    conn.setSecondaryOk();
    secondaryDbs.push(conn.getDB(dbName));
});

IndexBuildTest.assertIndexes(secondaryDbs[0][collName], 2, ['_id_', indexName]);
IndexBuildTest.assertIndexes(secondaryDbs[1][collName], 1, ['_id_']);

replTest.stopSet();
}());
