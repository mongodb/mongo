/*
 * Test that the index commands send and check shard versions, and only target the shards
 * that have chunks for the collection. Also test that the commands fail if they are run
 * when the critical section is in progress, and block until the critical section is over.
 */
import {
    moveChunkParallel,
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";
import {ShardedIndexUtil} from "jstests/sharding/libs/sharded_index_util.js";

// Test deliberately inserts orphans outside of migration.
TestData.skipCheckOrphans = true;

// This test connects directly to shards and creates collections.
TestData.skipCheckShardFilteringMetadata = true;

// Do not check metadata consistency as collections on non-primary shards are created for testing
// purposes.
TestData.skipCheckMetadataConsistency = true;

/*
 * Runs the command after performing chunk operations to make the primary shard (shard1) not own
 * any chunks for the collection, and the router that issues the command (st.s) have a stale routing
 * table.
 *
 * Asserts that the command sends and checks shard versions by asserting that a StaleConfig error is
 * observed and retried when the command is run through the stale router. A shard can never be
 * ignorant of its own metadata under authoritative shards, so the staleness lives on the router
 * rather than on the shards.
 */
function assertCommandChecksShardVersions(st, dbName, collName, testCase) {
    const ns = dbName + "." + collName;

    // Perform all migrations through a different router than the one that runs the command, so that
    // the command's router (st.s) is left with a stale routing table.
    const migrateRouter = st.s1;

    // Set up the chunk layout through the migrate router:
    //   shard1: no chunks, shard0: [MinKey, 0) and [0, MaxKey)
    // The initial chunk is moved off the primary and split into two chunks on shard0.
    ChunkHelper.moveChunk(
        migrateRouter.getDB(dbName),
        collName,
        [{_id: MinKey}, {_id: MaxKey}],
        st.shard0.shardName,
        true /* waitForDelete */,
    );
    assert.commandWorked(migrateRouter.adminCommand({split: ns, middle: {_id: 0}}));

    // Assert that the primary shard does not own any chunks for the collection.
    ShardVersioningUtil.assertShardVersionEquals(st.shard1, ns, Timestamp(0, 0));

    // Move [0, MaxKey) to shard2 through migrateRouter, then run the command through the stale router
    // st.s. st.s still believes shard0 owns [0, MaxKey), so it targets shard0 with a stale shard
    // version; shard0 rejects with a StaleConfig, forcing the command to refresh and retarget
    // shard2. The observed StaleConfig proves the command sends and checks shard versions.
    assert.commandWorked(
        ShardVersioningUtil.runOperationOnStaleRouterAfterMoveChunk({
            migrateRouter,
            staleRouter: st.s,
            ns,
            toShard: st.shard2,
            bounds: [{_id: 0}, {_id: MaxKey}],
            runStaleOperation: (router) => {
                if (testCase.setUpFuncForCheckShardVersionTest) {
                    testCase.setUpFuncForCheckShardVersionTest();
                }
                return router.getDB(dbName).runCommand(testCase.command);
            },
        }),
    );
}

/*
 * Runs moveChunk to move one chunk from the primary shard (shard1) to shard0. Pauses the
 * migration after shard1 enters the read-only phase of the critical section, and runs
 * the given command function. Asserts that the command is blocked behind the critical section.
 */
function assertCommandBlocksIfCriticalSectionInProgress(
    st,
    staticMongod,
    dbName,
    collName,
    allShards,
    testCase,
) {
    const ns = dbName + "." + collName;
    const fromShard = st.shard1;
    const toShard = st.shard0;

    if (testCase.skipCriticalSectionTest && testCase.skipCriticalSectionTest()) {
        jsTestLog(`Skipping critical section test for ${tojson(testCase.command)}`);
        return;
    }

    if (testCase.setUpFuncForCriticalSectionTest) {
        testCase.setUpFuncForCriticalSectionTest();
    }

    // Split the initial chunk.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

    // Turn on the fail point, and move one of the chunks to shard0 so that there are two
    // shards that own chunks for the collection. Wait for moveChunk to hit the fail point.
    pauseMoveChunkAtStep(fromShard, moveChunkStepNames.chunkDataCommitted);
    let joinMoveChunk = moveChunkParallel(
        staticMongod,
        st.s.host,
        {_id: 0},
        null,
        ns,
        toShard.shardName,
    );
    waitForMoveChunkStep(fromShard, moveChunkStepNames.chunkDataCommitted);

    // Run the command with maxTimeMS.
    const cmdWithMaxTimeMS = Object.assign({}, testCase.command, {maxTimeMS: 750});
    assert.commandFailed(st.s.getDB(dbName).runCommand(cmdWithMaxTimeMS));

    // Assert that the command reached the shard and then timed out.
    // It could be possible that the following check fails on slow clusters because the request
    // expired its maxTimeMS on the mongos before to reach the shard.
    checkLog.checkContainsOnceJsonStringMatch(st.shard1, 22062, "error", "MaxTimeMSExpired");

    allShards.forEach(function (shard) {
        testCase.assertCommandDidNotRunOnShard(shard);
    });

    // Turn off the fail point and wait for moveChunk to complete.
    unpauseMoveChunkAtStep(fromShard, moveChunkStepNames.chunkDataCommitted);
    joinMoveChunk();
}

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on shards and cause them to refresh their sharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false},
};

// The critical-section subtest migrates a chunk to a shard that recently owned an overlapping range;
// the recipient would otherwise reject the migration to preserve point-in-time reachable ownership
// history (ConflictingOperationInProgress). This test does not rely on point-in-time reads of the
// migrated range, so allow such migrations to proceed on all shards.
const allowDropRecipientPITHistory = {allowMigrationsToDropRecipientPITHistory: true};

const numShards = 3;
const st = new ShardingTest({
    mongos: 2,
    shards: numShards,
    other: {
        configOptions: {
            setParameter: {...nodeOptions.setParameter, ...allowDropRecipientPITHistory},
        },
        rsOptions: {setParameter: allowDropRecipientPITHistory},
    },
});

if (!FeatureFlagUtil.isEnabled(st.s.getDB("admin"), "featureFlagDropIndexesDDLCoordinator")) {
    // Do not check index consistency because a dropIndexes command that times out may leave indexes inconsistent.
    TestData.skipCheckingIndexesConsistentAcrossCluster = true;
}

const allShards = [];
for (let i = 0; i < numShards; i++) {
    allShards.push(st["shard" + i]);
}

const dbName = "test";
const testDB = st.s.getDB(dbName);
const shardKey = {
    _id: 1,
};
const index = {
    key: {x: 1},
    name: "x_1",
};

const testCases = {
    createIndexes: (collName) => {
        return {
            command: {createIndexes: collName, indexes: [index]},
            assertCommandRanOnShard: (shard) => {
                ShardedIndexUtil.assertIndexExistsOnShard(shard, dbName, collName, index.key);
            },
            assertCommandDidNotRunOnShard: (shard) => {
                ShardedIndexUtil.assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
            },
        };
    },
    dropIndexes: (collName) => {
        const ns = dbName + "." + collName;
        const createIndexOnAllShards = () => {
            allShards.forEach(function (shard) {
                assert.commandWorked(
                    shard.getDB(dbName).runCommand({createIndexes: collName, indexes: [index]}),
                );
            });
        };
        return {
            command: {dropIndexes: collName, index: index.name},
            setUpFuncForCheckShardVersionTest: () => {
                // Create the index directly on all the shards. Note that this will not cause stale
                // shards to refresh their shard versions.
                createIndexOnAllShards();
            },
            setUpFuncForCriticalSectionTest: () => {
                // Move the initial chunk from the shard1 (primary shard) to shard0 and then move it
                // from shard0 back to shard1. This is just to make the collection also exist on
                // shard0 so that the createIndexes command below won't create the collection on
                // shard0 with a different UUID which will cause the moveChunk command in the test
                // to fail.
                assert.commandWorked(
                    st.s.adminCommand({
                        moveChunk: ns,
                        find: {_id: MinKey},
                        to: st.shard0.shardName,
                        _waitForDelete: true,
                    }),
                );
                assert.commandWorked(
                    st.s.adminCommand({
                        moveChunk: ns,
                        find: {_id: MinKey},
                        to: st.shard1.shardName,
                        _waitForDelete: true,
                    }),
                );

                // Create the index directly on all the shards so shards.
                createIndexOnAllShards();
            },
            skipCriticalSectionTest: () => {
                if (FeatureFlagUtil.isEnabled(testDB, "featureFlagDropIndexesDDLCoordinator")) {
                    // The dropIndexes command spawns a sharding DDL coordintaor, which doesn't timeout.
                    // Therefore, we can't run this test case which relies on the command timing out.
                    return true;
                }
                return false;
            },
            assertCommandRanOnShard: (shard) => {
                ShardedIndexUtil.assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
            },
            assertCommandDidNotRunOnShard: (shard) => {
                ShardedIndexUtil.assertIndexExistsOnShard(shard, dbName, collName, index.key);
            },
        };
    },
};

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}),
);

// Test that the index commands send and check shard versions, and only target the shards
// that own chunks for the collection.
const expectedTargetedShards = new Set([st.shard0, st.shard2]);
assert.lt(expectedTargetedShards.size, numShards);

for (const command of Object.keys(testCases)) {
    jsTest.log(`Testing that ${command} sends and checks shard version...`);
    let collName = command;
    let ns = dbName + "." + collName;
    let testCase = testCases[command](collName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    assertCommandChecksShardVersions(st, dbName, collName, testCase);

    allShards.forEach(function (shard) {
        if (expectedTargetedShards.has(shard)) {
            testCase.assertCommandRanOnShard(shard);
        } else {
            testCase.assertCommandDidNotRunOnShard(shard);
        }
    });
}

// Test that the index commands are blocked behind the critical section.
const staticMongod = MongoRunner.runMongod({});

for (const command of Object.keys(testCases)) {
    jsTest.log(`Testing that ${command} is blocked behind the critical section...`);
    let collName = command + "CriticalSection";
    let ns = dbName + "." + collName;
    let testCase = testCases[command](collName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    assertCommandBlocksIfCriticalSectionInProgress(
        st,
        staticMongod,
        dbName,
        collName,
        allShards,
        testCase,
    );
}

st.stop();
MongoRunner.stopMongod(staticMongod);
