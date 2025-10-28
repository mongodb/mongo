/**
 * Tests:
 * (1) We can set commitQuorum when creating an primary-driven index, but it's a no-op.
 * (2) The default commit quorum for a primary-driven index build is 0 (disabled).
 * (3) We can set the commitQuorum when the primary-driven index build is running, but it's a no-op.
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");
const collName = "primaryDrivenIndexBuild";
const coll = primaryDB.primaryDrivenIndexBuild;

// TODO(SERVER-109349): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping commit_quorum_primary_driven.js because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}
const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 1000;
for (let i = 0; i < numDocs; i++) {
    bulk.insert({a: i, x: i});
}
assert.commandWorked(bulk.execute());

// This test depends on using the IndexBuildsCoordinator to build this index, which as of
// SERVER-44405, will not occur in this test unless the collection is created beforehand.
assert.commandWorked(primaryDB.runCommand({create: collName}));

// Use createIndex(es) to build indexes and check the commit quorum default.
jsTestLog("Create index");
let res = assert.commandWorked(
    primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{name: "x_1", key: {x: 1}}],
        commitQuorum: "majority",
    }),
);
assert.eq(0, res.commitQuorum);
assert(checkLog.checkContainsWithCountJson(primaryDB, 11302400, undefined, 1), "Expecting to see log with id 11302400");

rst.awaitReplication();

let awaitShell;
const failPoint = configureFailPoint(primaryDB, "hangAfterIndexBuildFirstDrain");
try {
    // Starts parallel shell to run the command that will hang.
    awaitShell = startParallelShell(function () {
        // Use the index builds coordinator for a two-phase index build.
        assert.commandWorked(
            db.runCommand({
                createIndexes: "primaryDrivenIndexBuild",
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
    }, primaryDB.getMongo().port);

    failPoint.wait();

    assert.commandWorked(
        primaryDB.runCommand({
            setIndexCommitQuorum: "primaryDrivenIndexBuild",
            indexNames: ["a_1"],
            commitQuorum: "majority",
        }),
    );
    assert(
        checkLog.checkContainsWithCountJson(primaryDB, 11302401, undefined, 1),
        "Expecting to see log with id 11302401",
    );
} finally {
    failPoint.off();
}

// Wait for the parallel shell to complete.
awaitShell();

IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "x_1"]);

rst.stopSet();
