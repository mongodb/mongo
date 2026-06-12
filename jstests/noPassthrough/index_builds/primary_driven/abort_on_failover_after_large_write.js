/**
 * Tests that a primary-driven index build is aborted on step-up when a tearable concurrent write
 * occurred during the build.
 *
 * TODO (SERVER-126257): Remove or replace test when resuming tearable side writes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const dbName = jsTestName();
const collName = "coll";
const indexSpec = {x: 1};
const indexName = "x_1";
const primaryDB = primary.getDB(dbName);
const coll = primaryDB.getCollection(collName);

const requiredFlags = [
    "PrimaryDrivenIndexBuilds",
    "ContainerWrites",
    "ResumablePrimaryDrivenIndexBuilds",
];
for (const flag of requiredFlags) {
    if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
        jsTest.log.info("Skipping: featureFlag" + flag + " is disabled");
        rst.stopSet();
        quit();
    }
}

// Force spills during the scan so the build persists resumable index state.
for (const node of rst.nodes) {
    assert.commandWorked(
        node.adminCommand({setParameter: 1, maxIndexBuildMemoryUsageMegabytes: 1}),
    );
}

jsTest.log.info("1. Insert seed data");
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 1500; i++) {
    bulk.insert({x: String(i).padStart(8, "0") + "x".repeat(1000)});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

jsTest.log.info("2. Start index build, paused before scanning");
const holdFp = configureFailPoint(primary, "hangBeforeBuildingIndex");
const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    indexSpec,
    {name: indexName},
    [ErrorCodes.InterruptedDueToReplStateChange],
);
IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName);

jsTest.log.info("3. Insert a large document to trigger the tearable side write abort sentinel");
// A single ~15 MB insert on a collection with an active PDIB produces two batched operations:
// the container side write and the document insert itself; their combined size exceeds the
// 16 MB applyOps entry limit.
assert.commandWorked(coll.insertOne({x: "side_a" + "x".repeat(15 * 1024 * 1024)}));
// Ensure the abort sentinel has replicated before the secondary steps up; otherwise the step-up can
// race replication and the new primary may resume (rather than abort) the build.
rst.awaitReplication();

jsTest.log.info("4. Configure load fail point, then release the hold");
// The build proceeds through the scan and pauses during load.
const loadFp = configureFailPoint(primary, "hangIndexBuildDuringBulkLoadPhase", {
    indexNames: [indexName],
});
holdFp.off();
loadFp.wait();

jsTest.log.info("5. Step up the secondary");
const newPrimary = rst.stepUp(rst.getSecondary());

loadFp.off();
awaitIndexBuild({checkExitSuccess: false});

jsTest.log.info("6. Verify the index build was aborted");
const newPrimaryDB = newPrimary.getDB(dbName);
const newColl = newPrimaryDB.getCollection(collName);
IndexBuildTest.assertIndexesSoon(newColl, 1, ["_id_"]);

rst.stopSet();
