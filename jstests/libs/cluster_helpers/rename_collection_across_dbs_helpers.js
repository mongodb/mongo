/**
 * Shared cross-database rename scenarios. Callers must place both databases on the same primary
 * shard.
 */

import {
    getShardNames,
    setupTestDatabase,
} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

// Under continuous stepdowns the network-retry override blindly retries commands after a failover.
// A create/createUnsplittableCollection that already succeeded then fails with NamespaceExists, and
// a renameCollection that already succeeded then fails with NamespaceNotFound (the source is gone).
// We tolerate exactly those codes *only* when running with stepdowns, and rely on the strict
// post-rename verification below to catch a genuinely failed rename. Outside stepdown suites the
// assertions stay strict (equivalent to assert.commandWorked).
const kRunningWithStepdowns = Boolean(TestData && TestData.runningWithShardStepdowns);

function assertCreateCommandSucceeded(res) {
    assert.commandWorkedOrFailedWithCode(
        res,
        kRunningWithStepdowns ? [ErrorCodes.NamespaceExists] : [],
    );
}

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

function getUuid(configDb, nss) {
    return configDb.collections.findOne({_id: nss}).uuid;
}

function checkRenameUnsplittableCollectionSucceeded(
    configDb,
    sourceNss,
    targetNss,
    expectedUuid,
    sourceShardName,
) {
    const sourceCollEntry = configDb.collections.findOne({_id: sourceNss});
    assert(sourceCollEntry === null) << tojson(sourceCollEntry);

    const targetCollEntry = configDb.collections.findOne({_id: targetNss});
    assert(targetCollEntry !== null);
    assert.eq(targetCollEntry._id, targetNss);
    assert.eq(targetCollEntry.unsplittable, true);
    assert.eq(targetCollEntry.key, {_id: 1});
    assert.eq(targetCollEntry.uuid, expectedUuid);

    let chunks = configDb.chunks.find({uuid: expectedUuid}).toArray();
    assert.eq(chunks.length, 1);
    assert.eq(chunks[0].shard, sourceShardName);
}

/**
 * Launch a rename test. This function executes:
 *     1. Create FROM collection as an unsplittable collection on the given shard.
 *     2. If `collToShouldExist` is true, create TO collection as an unsplittable collection on the
 *        given shard.
 *     3. Rename FROM collection to `dbTo` + "." + `collNameTo` namespace.
 *     4. Check that rename has succeeded.
 */
export function testRenameUnsplittableCollection(
    configDb,
    sourceDb,
    sourceCollName,
    targetDb,
    targetCollName,
    sourceShardName,
    targetExists = false,
    targetShardName = "",
) {
    const sourceNss = sourceDb.getName() + "." + sourceCollName;
    const targetNss = targetDb.getName() + "." + targetCollName;

    const dropTarget = targetExists ? true : false;

    jsTest.log.info("Running rename of tracked unsplittable collection", {
        sourceNss,
        targetNss,
        sourceShardName,
        targetExists,
        targetShardName: targetExists ? targetShardName : null,
        dropTarget,
    });

    assertCreateCommandSucceeded(
        sourceDb.runCommand({
            createUnsplittableCollection: sourceCollName,
            dataShard: sourceShardName,
        }),
    );
    const sourceColl = sourceDb[sourceCollName];
    const sourceUuid = getUuid(configDb, sourceNss);

    if (targetExists) {
        assert.neq("", targetShardName);
        assertCreateCommandSucceeded(
            targetDb.runCommand({
                createUnsplittableCollection: targetCollName,
                dataShard: targetShardName,
            }),
        );
    }

    assertRenameCommandSucceeded(
        sourceDb.adminCommand({
            renameCollection: sourceColl.getFullName(),
            to: targetNss,
            dropTarget: dropTarget,
        }),
    );

    const targetUuid = getUuid(configDb, targetNss);
    if (sourceDb.getName() === targetDb.getName()) {
        assert.eq(sourceUuid, targetUuid);
    } else {
        assert.neq(sourceUuid, targetUuid);
    }

    checkRenameUnsplittableCollectionSucceeded(
        configDb,
        sourceNss,
        targetNss,
        targetUuid,
        sourceShardName,
    );
}

/**
 * Verifies cross-database renames of untracked collections, with and without a target, including
 * source removal, document preservation, and fresh target UUID assignment (SERVER-130940).
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

    jsTest.log.info("Running rename of untracked collection", {
        sourceNss: sourceColl.getFullName(),
        targetNss: targetColl.getFullName(),
        dropTarget: targetExists,
    });

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
 * Runs cross-database rename scenarios for untracked collections. Use `suffix` to make namespaces unique per invocation.
 */
export function runRenameUntrackedCollectionAcrossDbsScenarios(sourceDb, targetDb, suffix) {
    if (skipRenameAcrossDbsScenarios()) {
        return;
    }

    testRenameUntrackedCollectionAcrossDbs(
        sourceDb,
        "untracked_noTarget_sourceColl_" + suffix,
        targetDb,
        "untracked_noTarget_targetColl_" + suffix,
        false /* targetExists */,
    );

    testRenameUntrackedCollectionAcrossDbs(
        sourceDb,
        "untracked_dropTarget_sourceColl_" + suffix,
        targetDb,
        "untracked_dropTarget_targetColl_" + suffix,
        true /* targetExists */,
    );
}

/**
 * Runs cross-database rename scenarios for tracked unsplittable collections,
 * including non-primary-shard cases when available. Use `suffix` to make namespaces unique per invocation.
 */
export function runRenameUnsplittableCollectionAcrossDbsScenarios(
    conn,
    sourceDb,
    targetDb,
    primaryShard,
    suffix,
) {
    if (skipRenameAcrossDbsScenarios()) {
        return;
    }

    const configDb = conn.getSiblingDB("config");
    const shards = getShardNames(conn);
    const nonPrimaryShardName = shards.find((s) => s !== primaryShard);

    // Tracked unsplittable collection with data on the primary shard.
    testRenameUnsplittableCollection(
        configDb,
        sourceDb,
        "tracked_primary_sourceColl_" + suffix,
        targetDb,
        "tracked_primary_targetColl_" + suffix,
        primaryShard,
    );

    if (!nonPrimaryShardName) {
        return;
    }

    // Tracked unsplittable collection with data on a non-primary shard (requires 2+ shards).
    testRenameUnsplittableCollection(
        configDb,
        sourceDb,
        "tracked_nonPrimary_sourceColl_" + suffix,
        targetDb,
        "tracked_nonPrimary_targetColl_" + suffix,
        nonPrimaryShardName,
    );

    // Target exists on a different data shard than the source, dropTarget=true.
    testRenameUnsplittableCollection(
        configDb,
        sourceDb,
        "tracked_differentShards_sourceColl_" + suffix,
        targetDb,
        "tracked_differentShards_targetColl_" + suffix,
        primaryShard,
        true /* targetExists */,
        nonPrimaryShardName,
    );
}

/**
 * Sets up two databases sharing the same primary shard and returns them along with the primary
 * shard id.
 */
export function setupSamePrimaryDatabases(conn, sourceDbName, targetDbName) {
    const primaryShard = getShardNames(conn)[0];
    const sourceDb = setupTestDatabase(conn, sourceDbName, primaryShard);
    const targetDb = setupTestDatabase(conn, targetDbName, primaryShard);
    return {sourceDb, targetDb, primaryShard};
}
