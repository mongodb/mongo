/**
 * Tests that a primary-driven index build aborts on step-up after a concurrent multi-statement write
 * is torn across applyOps oplog entries -- whether the failover happens during the load phase, with
 * resume state already persisted, or before the build has written any resume state.
 *
 * A write touching multiple documents replicates as a batch whose operations can be applied
 * independently. When the batch spans more than one applyOps oplog entry, the index entries the build
 * records for it can be torn across the boundary, leaving state the build cannot safely resume from,
 * so it must abort on step-up. The split is forced here by lowering
 * maxNumberOfBatchedOperationsInSingleOplogEntry; in production it happens when the batch exceeds the
 * 16 MB oplog entry limit (for example a large time-series insert). A single-statement write
 * replicates atomically and cannot tear, so it resumes instead; see
 * single_statement_write_resumes_on_failover.js.
 *
 * TODO (SERVER-126257): Update this test when the build can resume across a torn write; the torn
 * cases will then resume rather than abort.
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

// Runs a primary-driven index build, performs a multi-document insert forced to tear across applyOps
// oplog entries, fails over (either during the load phase or before any resume state is written),
// and asserts the build aborted on the new primary.
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
    // The suite injects random WT write conflicts. Under them, the torn vectored insert below falls
    // back to per-document kGroupForTransaction inserts, which apply atomically and are safe to
    // resume. Disable the fault injection so the torn-write-aborts-the-build path is exercised
    // deterministically.
    for (const node of rst.nodes) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: "WTWriteConflictException", mode: "off"}),
        );
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

    // Capping the operations per applyOps entry at 1 forces this multi-document insert to span
    // multiple oplog entries that apply independently, so the index entries the build records for it
    // are torn apart.
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, maxNumberOfBatchedOperationsInSingleOplogEntry: 1}),
    );
    const oplog = primary.getDB("local").oplog.rs;
    const tsBeforeInsert = oplog.find().sort({$natural: -1}).limit(1).next().ts;
    assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}]));
    // Restore the default (2147483647 = INT_MAX) so the cap affects only the insert above, not the
    // index build's own writes during the load phase.
    assert.commandWorked(
        primary.adminCommand({
            setParameter: 1,
            maxNumberOfBatchedOperationsInSingleOplogEntry: 2147483647,
        }),
    );
    // Self-validation: the insert must have spanned more than one applyOps entry, otherwise the
    // scenario is not exercising the torn-write path it claims to.
    const applyOpsEntries = oplog
        .find({ts: {$gt: tsBeforeInsert}, "o.applyOps": {$exists: true}})
        .toArray();
    assert.gt(applyOpsEntries.length, 1, "expected the insert to span >1 applyOps entries", {
        count: applyOpsEntries.length,
    });
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

    // The torn write leaves the build unable to resume, so it is aborted on the new primary.
    const newColl = newPrimary.getDB(dbName).getCollection(collName);
    IndexBuildTest.assertIndexesSoon(newColl, 1, ["_id_"]);

    rst.stopSet();
}

jsTest.log.info("Torn multi-statement write, failover during the load phase -> build aborts");
runScenario({failoverDuringLoad: true});

jsTest.log.info("Torn multi-statement write, failover before any resume state -> build aborts");
runScenario({failoverDuringLoad: false});
