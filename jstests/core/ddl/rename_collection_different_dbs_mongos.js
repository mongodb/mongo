/*
 * Tests renameCollection across two databases that share the same primary shard on a sharded
 * cluster. This exercises the renameCollectionAcrossDatabases path.
 *
 *
 * @tags: [
 *   # Test assertions require collection placement stability
 *   assumes_balancer_off,
 *   # Cross-database rename requires unsharded source/target collections; this test controls their
 *   # placement explicitly and must not have its collections implicitly sharded.
 *   assumes_unsharded_collection,
 *   requires_sharding,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/sharded_cluster_fixture_helpers.js");
load("jstests/libs/fixture_helpers.js");
load("jstests/libs/uuid_util.js");

if (!FixtureHelpers.isMongos(db)) {
    jsTestLog("Skipping cross-database rename test because the connection is not to mongos");
    return;
}

// Under continuous stepdowns the network-retry override blindly retries commands after a failover.
// A renameCollection that already succeeded then fails with NamespaceNotFound (the source is gone).
// We tolerate that code only when running with stepdowns, and rely on the strict post-rename
// verification below to catch a genuinely failed rename. Outside stepdown suites the assertion
// stays strict (equivalent to assert.commandWorked). As of now, there is no sharding stepdown
// suite, so this is mainly for future proofing if we add a sharded stepdown suite in future.
const kRunningWithStepdowns = Boolean(TestData && TestData.runningWithShardStepdowns);

function assertRenameCommandSucceeded(res) {
    assert.commandWorkedOrFailedWithCode(
        res,
        kRunningWithStepdowns ? [ErrorCodes.NamespaceNotFound] : [],
    );
}

/**
 * TODO (SERVER-131660): Remove this once rename across databases is fixed on older versions.
 */
function skipRenameAcrossDbsScenarios() {
    if (Boolean(TestData.multiversionBinVersion) || Boolean(TestData.mixedBinVersions)) {
        jsTestLog("Skipping rename across databases scenarios as multiversion is enabled");
        return true;
    }
    return false;
}

/**
 * Verifies cross-database renames of untracked collections, with and without a target, including
 * source removal, document preservation, and fresh target UUID assignment.
 */
function testRenameUntrackedCollectionAcrossDbs(
    sourceDb,
    sourceCollName,
    targetDb,
    targetCollName,
    targetExists = false,
) {
    const sourceColl = sourceDb[sourceCollName];
    const targetColl = targetDb[targetCollName];
    sourceColl.drop();
    targetColl.drop();

    const kSourceDocs = 7;
    assert.commandWorked(
        sourceColl.insertMany(
            Array.from({length: kSourceDocs}, (_, i) => ({_id: i, marker: "from"})),
            ),
    );
    if (targetExists) {
        assert.commandWorked(
            targetColl.insertMany(Array.from({length: 4}, (_, i) => ({_id: i, marker: "to"}))),
        );
    }
    const sourceUuid = getUUIDFromListCollections(sourceDb, sourceCollName);
    const originalTargetUuid =
        targetExists ? getUUIDFromListCollections(targetDb, targetCollName) : undefined;

    jsTest.log("Running rename of untracked collection: " + tojson({
                   sourceNss: sourceColl.getFullName(),
                   targetNss: targetColl.getFullName(),
                   dropTarget: targetExists,
               }));

    assertRenameCommandSucceeded(
        sourceDb.adminCommand({
            renameCollection: sourceColl.getFullName(),
            to: targetColl.getFullName(),
            dropTarget: targetExists,
        }),
    );

    assert.eq(
        0,
        sourceDb.getCollectionInfos({name: sourceCollName}).length,
        "source collection should no longer exist after rename",
    );
    const targetUuid = getUUIDFromListCollections(targetDb, targetCollName);
    assert(targetUuid, "target collection should exist after rename");
    assert.neq(
        sourceUuid,
        targetUuid,
        "cross-database rename must assign a fresh UUID to the target",
    );
    if (targetExists) {
        assert.neq(
            originalTargetUuid,
            targetUuid,
            "cross-database rename must assign a new UUID instead of reusing the dropped target",
        );
    }
    assert.eq(
        kSourceDocs,
        targetColl.find().itcount(),
        "target should hold the source's documents",
    );
    assert.eq(
        kSourceDocs,
        targetColl.find({marker: "from"}).itcount(),
        "target documents should be the source's, not the dropped target's",
    );
}

/**
 * Sets up two databases sharing the same primary shard and returns them along with the primary
 * shard id.
 */
function setupSamePrimaryDatabases(conn, sourceDbName, targetDbName) {
    const primaryShard = assert.commandWorked(conn.adminCommand({listShards: 1})).shards[0]._id;
    const sourceDb = setupTestDatabase(conn, sourceDbName, primaryShard);
    const targetDb = setupTestDatabase(conn, targetDbName, primaryShard);
    return {sourceDb, targetDb, primaryShard};
}

// Set the databases up once for the cross-database rename scenarios.
const {sourceDb, targetDb} = setupSamePrimaryDatabases(
    db,
    jsTestName() + "_from",
    jsTestName() + "_to",
);

jsTest.log("Running cross-database rename scenarios");
if (!skipRenameAcrossDbsScenarios()) {
    testRenameUntrackedCollectionAcrossDbs(
        sourceDb,
        "untracked_noTarget_sourceColl",
        targetDb,
        "untracked_noTarget_targetColl",
        false /* targetExists */,
    );

    testRenameUntrackedCollectionAcrossDbs(
        sourceDb,
        "untracked_dropTarget_sourceColl",
        targetDb,
        "untracked_dropTarget_targetColl",
        true /* targetExists */,
    );
}
}());
