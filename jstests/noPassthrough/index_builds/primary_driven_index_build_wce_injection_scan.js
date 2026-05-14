/**
 * Drives a primary-driven index build (PDIB) while artificially injecting
 * WriteConflictExceptions at the storage-engine layer via the WTWriteConflictException
 * failpoint. The collection-scan phase must tolerate spurious WCEs by retrying the
 * batch and ultimately completing the build successfully.
 *
 * See SERVER-126326 — PDIB WC-injection test coverage.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0, votes: 0}},
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");
const collName = "pdibWceScan";
const coll = primaryDB.getCollection(collName);

if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(primaryDB.runCommand({create: collName}));

const numDocs = 2000;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, a: i, payload: "x".repeat(64)});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

// Enable WCE tracing so any trapped exception appears in logs for triage.
assert.commandWorked(
    primaryDB.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}),
);

// Inject WCEs at a low probability — high enough to exercise the retry path,
// low enough that the build still completes within a reasonable wall time.
const wcFp = configureFailPoint(
    primary,
    "WTWriteConflictException",
    {} /* data */,
    {activationProbability: 0.01},
);

try {
    jsTestLog("Creating index with WCE injection active");
    assert.commandWorked(
        primaryDB.runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "a_1"}],
        }),
    );
} finally {
    wcFp.off();
}

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

// Replica-set consistency check: secondary applies the commitIndexBuild oplog
// entry and ends up with the same index set.
rst.awaitReplication();
const secondary = rst.getSecondary();
IndexBuildTest.assertIndexes(
    secondary.getDB("test").getCollection(collName),
    2,
    ["_id_", "a_1"],
);

rst.stopSet();
