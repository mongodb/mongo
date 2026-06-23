/**
 * Tests that a primary-driven index build is aborted on step-up after a tearable concurrent write,
 * even when the failover happens before the build performs its next periodic resume-state write.
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
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = new ReplSetTest({
    nodes: TestData.doesNotSupportGracefulStepdown
        ? [{rsConfig: {priority: 1}}, {rsConfig: {priority: 1}}]
        : 2,
});
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

jsTest.log.info("1. Insert seed data");
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 1500; i++) {
    bulk.insert({x: String(i).padStart(8, "0") + "x".repeat(1000)});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

jsTest.log.info(
    "2. Start index build, paused before scanning (before any periodic resume-state write)",
);
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
// A single ~15 MB insert on a collection with an active PDIB produces two batched operations: the
// container side write and the document insert itself; their combined size exceeds the 16 MB
// applyOps entry limit. The abort sentinel is written in the same batch, ahead of the side write.
assert.commandWorked(coll.insertOne({x: "side_a" + "x".repeat(15 * 1024 * 1024)}));
// Ensure the side write (and the sentinel written alongside it) has replicated before the step-up.
rst.awaitReplication();

jsTest.log.info("4. Step up the secondary while the build is still paused before scanning");
// The build has not written any resume state since the side write (it never reached the scan), so
// the only thing that can make the new primary abort is the sentinel written with the side write.
const newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(rst);

// Let the old primary's interrupted build unwind.
holdFp.off();
awaitIndexBuild({checkExitSuccess: false});

jsTest.log.info("5. Verify the index build was aborted on the new primary");
const newPrimaryDB = newPrimary.getDB(dbName);
const newColl = newPrimaryDB.getCollection(collName);
IndexBuildTest.assertIndexesSoon(newColl, 1, ["_id_"]);

rst.stopSet();
