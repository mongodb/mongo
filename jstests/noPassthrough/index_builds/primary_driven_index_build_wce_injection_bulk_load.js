/**
 * Drives a primary-driven index build (PDIB) with WCEs injected during the
 * external-sorter bulk-load phase. SERVER-126155 closed a related cursor-reset
 * bug; this test pins that the load path tolerates WCEs without dropping or
 * duplicating sorted keys.
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
    nodeOptions: {
        setParameter: {
            // Force the bulk-load batch boundary often so the WCE failpoint
            // has many opportunities to fire mid-load.
            primaryDrivenIndexBuildSorterInsertionBatchSize: 50,
            primaryDrivenIndexBuildSorterInsertionBatchBytes: 4 * 1024,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");
const collName = "pdibWceBulk";
const coll = primaryDB.getCollection(collName);

if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(primaryDB.runCommand({create: collName}));

const numDocs = 5000;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    // Wide values to push the sorter to spill to disk.
    bulk.insert({_id: i, k: i, blob: "y".repeat(256)});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

assert.commandWorked(
    primaryDB.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}),
);

const wcFp = configureFailPoint(
    primary,
    "WTWriteConflictException",
    {},
    {activationProbability: 0.02},
);

try {
    jsTestLog("Creating index over 5k wide docs with WCE injection");
    assert.commandWorked(
        primaryDB.runCommand({
            createIndexes: collName,
            indexes: [{key: {k: 1}, name: "k_1"}],
        }),
    );
} finally {
    wcFp.off();
}

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "k_1"]);

// Every original document must be indexed exactly once — no drops, no dupes.
assert.eq(
    numDocs,
    coll.find({k: {$gte: 0, $lt: numDocs}}).hint({k: 1}).itcount(),
    "index k_1 missing or duplicating entries after WCE-driven retries",
);

rst.awaitReplication();
rst.stopSet();
