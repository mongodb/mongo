/**
 * Tests that a primary-driven index build resumes on step-up after a large single-statement write
 * runs concurrently with the build -- whether the failover happens during the load phase, with
 * resume state already persisted, or before the build has written any resume state.
 *
 * A single-statement write replicates and applies as one atomic unit, so the index entries the build
 * records for it cannot be torn apart on the new primary, even when the write is large enough to span
 * multiple applyOps oplog entries. The build is therefore safe to resume after failover. A torn
 * multi-statement write is the case that instead forces an abort; see
 * multi_statement_write_aborts_on_failover.js.
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

const dbName = jsTestName();
const collName = "coll";
const indexSpec = {x: 1};
const indexName = "x_1";
const requiredFlags = [
    "PrimaryDrivenIndexBuilds",
    "ContainerWrites",
    "ResumablePrimaryDrivenIndexBuilds",
];

// Runs a primary-driven index build, performs a large single-statement insert concurrent with it,
// fails over (either during the load phase or before any resume state is written), and asserts the
// build resumed and completed on the new primary.
function runScenario({failoverDuringLoad}) {
    const rst = new ReplSetTest({
        nodes: TestData.doesNotSupportGracefulStepdown
            ? [{rsConfig: {priority: 1}}, {rsConfig: {priority: 1}}]
            : 2,
    });
    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    for (const flag of requiredFlags) {
        if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
            jsTest.log.info("Skipping: featureFlag" + flag + " is disabled");
            rst.stopSet();
            quit();
        }
    }
    const coll = primaryDB.getCollection(collName);

    if (failoverDuringLoad) {
        // Cap the build's memory so it spills and persists resume state during the scan.
        for (const node of rst.nodes) {
            assert.commandWorked(
                node.adminCommand({setParameter: 1, maxIndexBuildMemoryUsageMegabytes: 1}),
            );
        }
    }

    // The during-load failover needs enough seed data to force a spill; the before-scan failover
    // never reaches the scan, so a single document is sufficient.
    const seedCount = failoverDuringLoad ? 1500 : 1;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < seedCount; i++) {
        bulk.insert({x: String(i).padStart(8, "0") + "x".repeat(1000)});
    }
    assert.commandWorked(bulk.execute());
    rst.awaitReplication();

    const holdFp = configureFailPoint(primary, "hangBeforeBuildingIndex");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        indexSpec,
        {name: indexName},
        [ErrorCodes.InterruptedDueToReplStateChange],
    );
    IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName);

    // A single ~15 MB insert, together with the index entries the build records for it, exceeds the
    // 16 MB limit and spans multiple applyOps oplog entries; but a single-statement write replicates
    // and applies atomically, so it cannot be torn apart.
    assert.commandWorked(coll.insertOne({x: "x".repeat(15 * 1024 * 1024)}));
    rst.awaitReplication();

    let newPrimary;
    if (failoverDuringLoad) {
        // Let the build run past the scan into the load phase, where it has persisted resume state.
        const loadFp = configureFailPoint(primary, "hangIndexBuildDuringBulkLoadPhase", {
            indexNames: [indexName],
        });
        holdFp.off();
        loadFp.wait();
        newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(rst);
        loadFp.off();
    } else {
        // Fail over while the build is still paused before scanning, before any resume state exists.
        newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(rst);
        holdFp.off();
    }
    awaitIndexBuild({checkExitSuccess: false});

    // The write could not tear, so the build resumes and completes on the new primary.
    const newColl = newPrimary.getDB(dbName).getCollection(collName);
    IndexBuildTest.assertIndexesSoon(newColl, 2, ["_id_", indexName]);

    rst.stopSet();
}

jsTest.log.info("Single-statement write, failover during the load phase -> build resumes");
runScenario({failoverDuringLoad: true});

jsTest.log.info("Single-statement write, failover before any resume state -> build resumes");
runScenario({failoverDuringLoad: false});
