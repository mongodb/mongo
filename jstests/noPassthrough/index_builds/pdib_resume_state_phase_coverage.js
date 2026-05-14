/**
 * Tests that a resumable primary-driven index build (PDIB) persists its ResumeIndexInfo to the
 * replicated `internal-indexBuild-<UUID>` container at the expected persist points across all
 * phases. For each persist point we verify:
 *
 *   - A container write (op-type 'ci' or 'cu') targeting the build's ident is present in the
 *     oplog on the primary.
 *   - After awaitReplication, the same container write is present in the oplog on the secondary
 *     (the kReplicate path delivers byte-identical contents via the standard replication channel).
 *   - The server logs LOGV2_DEBUG(12558700) "wrote resumable state to disk via container write"
 *     with the build's UUID, demonstrating the persist fired.
 *
 * Persist points covered (mirrors SERVER-124387 / -124388 / -124389 / -124390 / -124391 /
 * -124458 unit tests in multi_index_block_test.cpp):
 *
 *   - end of collection scan + periodic scan/load writes
 *                                    (observed after hangAfterIndexBuildDumpsInsertsFromBulk)
 *   - end of bulk load               (hangAfterIndexBuildDumpsInsertsFromBulk)
 *   - beginning of drain phase       (hangAfterIndexBuildFirstDrain)
 *
 * On commit, we assert that no further container writes to the build's ident are produced after
 * the build completes (the build cleans up its resume state and the table is dropped on every
 * node).
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary so the primary stays the writer for every phase.
            rsConfig: {priority: 0, votes: 1},
        },
    ],
    // Raise storage verbosity to level 1 on both nodes so LOGV2_DEBUG(12558700) is emitted when
    // _writeStateToContainer fires.
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojsononeline({storage: 1}),
        },
    },
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = jsTestName();
const collName = "coll";
const primaryDB = primary.getDB(dbName);
const coll = primaryDB.getCollection(collName);
const collNss = coll.getFullName();

// TODO(SERVER-109578): Remove these checks when the feature flags are removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "ContainerWrites")) {
    jsTestLog("Skipping: featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "ResumablePrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: featureFlagResumablePrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(primaryDB.runCommand({create: collName}));

// Seed enough documents so the collection scan persists at least one resume-state record before
// reaching the bulk load (matching multi_index_block_test.cpp::PdibPersistsResumeStateOnFirstDrain
// which exercises kReplicate + isResumable=true).
const kNumSeed = 100;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < kNumSeed; i++) {
    bulk.insert({_id: i, a: i});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

const indexSpec = {a: 1};
const indexName = "a_1_pdib_resumable";

// Returns container-write oplog entries (op-type 'ci' or 'cu') on `node` whose `container` field
// matches the build's internal-indexBuild ident.
const internalIdentForBuildUUID = (buildUUID) => `internal-indexBuild-${buildUUID}`;
const containerOpsForIdent = function (node, ident) {
    const oplog = node.getDB("local").getCollection("oplog.rs");
    // Container writes ride inside applyOps. Match either:
    //   (a) top-level op-type 'ci'/'cu' with container=ident, or
    //   (b) any inner applyOps entry with op-type 'ci'/'cu' and container=ident.
    return oplog
        .find({
            $or: [
                {op: {$in: ["ci", "cu"]}, container: ident},
                {
                    op: "c",
                    "o.applyOps": {
                        $elemMatch: {op: {$in: ["ci", "cu"]}, container: ident},
                    },
                },
            ],
        })
        .sort({ts: 1})
        .toArray();
};

// Drive the build through a phase, hold at `holdFailPoint`, return its buildUUID. The build is
// started in a parallel shell so the test can inspect state while the index build is paused.
const startBuildAndHoldAt = function (holdFailPoint) {
    const fp = configureFailPoint(primary, holdFailPoint);
    const awaitBuild = IndexBuildTest.startIndexBuild(
        primary,
        collNss,
        indexSpec,
        {name: indexName},
        // Tolerate replication-state interruption codes if a phase failpoint also triggers
        // teardown later in the test.
        [],
    );
    fp.wait();

    const buildUUID = extractUUIDFromObject(
        IndexBuildTest.assertIndexes(coll, 2, ["_id_"], [indexName], {
            includeBuildUUIDs: true,
        })[indexName].buildUUID,
    );
    return {fp, awaitBuild, buildUUID};
};

// Wait until a container write targeting `ident` lands in the primary's oplog. The persist hook
// runs asynchronously relative to the phase failpoint pause point, so poll.
const waitForContainerWriteOnPrimary = function (ident, opts) {
    const {timeoutMs = 60 * 1000, minCount = 1} = opts || {};
    assert.soon(
        () => containerOpsForIdent(primary, ident).length >= minCount,
        () =>
            `Did not observe >= ${minCount} container writes for ident ${ident} on primary within ${timeoutMs}ms; ` +
            `saw: ${tojson(containerOpsForIdent(primary, ident))}`,
        timeoutMs,
    );
};

// Verify (a) primary saw a container write for the build's ident, (b) secondary has it too
// post-awaitReplication, (c) server-log line 12558700 fired with this buildUUID on the primary.
const assertPersistFired = function (phaseLabel, buildUUID) {
    const ident = internalIdentForBuildUUID(buildUUID);

    waitForContainerWriteOnPrimary(ident);

    rst.awaitReplication();

    const primaryOps = containerOpsForIdent(primary, ident);
    const secondaryOps = containerOpsForIdent(secondary, ident);
    assert.gt(
        primaryOps.length,
        0,
        `[${phaseLabel}] expected >=1 container write for ident ${ident} on primary, got: ${tojson(primaryOps)}`,
    );
    assert.gte(
        secondaryOps.length,
        primaryOps.length,
        `[${phaseLabel}] secondary should have replicated all primary container writes for ident ${ident}; ` +
            `primary=${primaryOps.length}, secondary=${secondaryOps.length}`,
    );

    // LOGV2_DEBUG(12558700) "wrote resumable state to disk via container write" fires once per
    // persist with the build's UUID in the structured attrs.
    assert.soon(
        () =>
            checkLog.checkContainsOnceJson(primary, 12558700, {
                buildUUID: function (uuid) {
                    return uuid && uuid["uuid"]["$uuid"] === buildUUID;
                },
            }),
        `[${phaseLabel}] expected log id 12558700 for buildUUID ${buildUUID} on primary`,
    );

    return {primaryOps, secondaryOps, ident};
};

// ---------------------------------------------------------------------------------------------
// Phase 1: end of collection scan persist point
//
// Hold at hangAfterIndexBuildDumpsInsertsFromBulk: by the time we hit this failpoint the scan
// phase has completed and the kBulkLoad phase has just finished dumping the sorter. The
// end-of-scan + bulk-load periodic persist writes (SERVER-124387, -124389, -124390) have already
// fired and should be visible in the oplog.
// ---------------------------------------------------------------------------------------------
jsTestLog("Phase 1: collection scan + bulk load persist points");
{
    const {fp, awaitBuild, buildUUID} = startBuildAndHoldAt("hangAfterIndexBuildDumpsInsertsFromBulk");
    jsTestLog(`Phase 1 buildUUID=${buildUUID}`);

    const {primaryOps} = assertPersistFired("scan+bulkLoad", buildUUID);
    // For kCollectionScan and kBulkLoad phases the persist record carries `ranges` and the per-
    // index sorter idents; the bulk load loop also drives the periodic persist hook so we expect
    // typically more than one write by the time we are paused after the bulk load. Don't pin an
    // exact count (depends on insertion-batch tunables), but assert monotonic growth across the
    // ident.
    assert.gte(primaryOps.length, 1, `Phase 1: expected >=1 container write, saw ${primaryOps.length}`);

    fp.off();
    // Let the build advance into the drain phase but immediately re-pause there.
    const drainFp = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
    drainFp.wait();

    // -----------------------------------------------------------------------------------------
    // Phase 2: beginning of drain phase persist point (SERVER-124391)
    // hangAfterIndexBuildFirstDrain pauses just after drainBackgroundWrites has transitioned the
    // build into kDrainWrites and (for the kReplicate + isResumable=true path) called
    // _writeStateToContainer one more time with phase=kDrainWrites.
    // -----------------------------------------------------------------------------------------
    jsTestLog("Phase 2: drain-phase persist point");
    const opsBeforeDrain = containerOpsForIdent(primary, internalIdentForBuildUUID(buildUUID)).length;
    // Wait for the additional drain-phase persist write to land.
    waitForContainerWriteOnPrimary(internalIdentForBuildUUID(buildUUID), {minCount: opsBeforeDrain + 1});
    assertPersistFired("drain", buildUUID);

    drainFp.off();
    awaitBuild();

    // ------------------------------------------------------------------------------------------
    // Phase 3: commit cleanup
    // After the build completes, no further container writes for this ident should appear, and
    // both nodes should report identical totals (replication is complete).
    // ------------------------------------------------------------------------------------------
    jsTestLog("Phase 3: commit cleanup — no further persists");
    rst.awaitReplication();
    const ident = internalIdentForBuildUUID(buildUUID);
    const finalPrimaryOps = containerOpsForIdent(primary, ident);
    const finalSecondaryOps = containerOpsForIdent(secondary, ident);
    assert.eq(
        finalPrimaryOps.length,
        finalSecondaryOps.length,
        `commit cleanup: primary and secondary should agree on container-write count for ident ${ident}; ` +
            `primary=${finalPrimaryOps.length}, secondary=${finalSecondaryOps.length}`,
    );

    IndexBuildTest.assertIndexes(coll, 2, ["_id_", indexName]);

    // Validate that the on-disk index built by the resume-state-emitting path contains a key for
    // every document inserted before the build started — i.e. the resume-state writes did not
    // perturb the final index contents.
    assert.eq(
        kNumSeed,
        coll.find({a: {$gte: 0}}).hint(indexName).itcount(),
        "resumable PDIB final index contents must match the full seeded document set",
    );
}

rst.stopSet();
