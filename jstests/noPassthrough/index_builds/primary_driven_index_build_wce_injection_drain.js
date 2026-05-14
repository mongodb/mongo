/**
 * Drives a primary-driven index build (PDIB) with concurrent CRUD traffic AND
 * artificial WCEs injected at the storage-engine layer. The side-writes drain
 * phase must converge on a consistent index in the presence of spurious retry
 * signals.
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
const collName = "pdibWceDrain";
const coll = primaryDB.getCollection(collName);

if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(primaryDB.runCommand({create: collName}));

const seedDocs = 1500;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < seedDocs; i++) {
    bulk.insert({_id: i, v: i});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

assert.commandWorked(
    primaryDB.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}),
);

// Hang at the first drain so concurrent writes accumulate in side-writes.
const drainFp = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");

const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    {v: 1},
    {name: "v_1"},
);

drainFp.wait();

// Inject WCEs while concurrent writes drive the side-writes phase.
const wcFp = configureFailPoint(
    primary,
    "WTWriteConflictException",
    {},
    {activationProbability: 0.02},
);

const extraWrites = 500;
try {
    for (let i = seedDocs; i < seedDocs + extraWrites; i++) {
        assert.commandWorked(coll.insert({_id: i, v: i}));
    }
    // Touch existing docs to exercise the update drain path.
    for (let i = 0; i < 200; i++) {
        assert.commandWorked(coll.update({_id: i}, {$set: {v: i + 1_000_000}}));
    }
} finally {
    drainFp.off();
    // Leave WCE injection on through the final drain + commit so the
    // commit-side write path also has to retry.
}

awaitIndexBuild();

wcFp.off();

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "v_1"]);

// Sanity: every doc must be retrievable via the new index.
const totalDocs = seedDocs + extraWrites;
assert.eq(totalDocs, coll.find().hint({v: 1}).itcount());

rst.awaitReplication();
rst.stopSet();
