/**
 * Test that the buildIndexes:false replica set config option behaves correctly with various commit
 * quorum options.
 *
 * @tags: [
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

// Skip db hash check because secondary will have different number of indexes due to
// buildIndexes:false.
TestData.skipCheckDBHashes = true;

const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: [
        {},
        {},
        {},
        {rsConfig: {priority: 0, buildIndexes: false}},
        {rsConfig: {priority: 0, votes: 0, buildIndexes: false}},
    ],
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
assert.commandFailedWithCode(
    primaryDb.runCommand({
        createIndexes: collName,
        indexes: [{key: {y: 1}, name: "y_1_default_commitQuorum"}],
    }),
    ErrorCodes.UnsatisfiableCommitQuorum,
);

// With a commit quorum that includes 4 nodes, the quorum is unsatisfiable because it includes a
// buildIndexes: false node.
assert.commandFailedWithCode(
    primaryDb.runCommand({
        createIndexes: collName,
        indexes: [{key: {y: 1}, name: "y_1_commitQuorum_3"}],
        commitQuorum: 4,
    }),
    ErrorCodes.UnsatisfiableCommitQuorum,
);

// This commit quorum does not need to include the buildIndexes:false node so it should be
// satisfiable.
const indexName = "y_1_commitQuorum_majority";
assert.commandWorked(
    primaryDb.runCommand({
        createIndexes: collName,
        indexes: [{key: {y: 1}, name: indexName}],
        commitQuorum: "majority",
    }),
);

replTest.awaitReplication();

let secondaryDbs = [];
replTest.getSecondaries().forEach((conn) => {
    conn.setSecondaryOk();
    secondaryDbs.push(conn.getDB(dbName));
});

IndexBuildTest.assertIndexes(secondaryDbs[0][collName], 2, ["_id_", indexName]);
IndexBuildTest.assertIndexes(secondaryDbs[1][collName], 2, ["_id_", indexName]);
IndexBuildTest.assertIndexes(secondaryDbs[2][collName], 1, ["_id_"]);
IndexBuildTest.assertIndexes(secondaryDbs[3][collName], 1, ["_id_"]);

replTest.stopSet();
