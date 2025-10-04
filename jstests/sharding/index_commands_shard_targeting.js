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
 * Runs the command after performing chunk operations to make the primary shard (shard0) not own
 * any chunks for the collection, and the subset of non-primary shards (shard1 and shard2) that
 * own chunks for the collection have stale catalog cache.
 *
 * Asserts that the command checks shard versions by checking that the shards to refresh their
 * cache after the command is run.
 */
function assertCommandChecksShardVersions(st, dbName, collName, testCase) {
    const ns = dbName + "." + collName;

    // Move the initial chunk out of the primary shard.
    ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns, st.shard0, st.shard1, {_id: MinKey});

    // Split the chunk to create two chunks on shard1. Move one of the chunks to shard2.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns, st.shard1, st.shard2, {_id: 0});

    // Assert that primary shard does not have any chunks for the collection.
    ShardVersioningUtil.assertShardVersionEquals(st.shard0, ns, Timestamp(0, 0));

    // The donor shard for the last moveChunk will have the latest collection version.
    let latestCollectionVersion = ShardVersioningUtil.getMetadataOnShard(st.shard1, ns).collVersion;

    // Assert that besides the latest donor shard (shard1), all shards have stale collection
    // version.
    ShardVersioningUtil.assertCollectionVersionOlderThan(st.shard0, ns, latestCollectionVersion);
    ShardVersioningUtil.assertCollectionVersionOlderThan(st.shard2, ns, latestCollectionVersion);

    if (testCase.setUpFuncForCheckShardVersionTest) {
        testCase.setUpFuncForCheckShardVersionTest();
    }
    assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command));

    if (testCase.bumpExpectedCollectionVersionAfterCommand) {
        latestCollectionVersion = testCase.bumpExpectedCollectionVersionAfterCommand(latestCollectionVersion);
    }

    // Assert that the targeted shards have the latest collection version after the command is
    // run.
    ShardVersioningUtil.assertCollectionVersionEquals(st.shard1, ns, latestCollectionVersion);
    ShardVersioningUtil.assertCollectionVersionEquals(st.shard2, ns, latestCollectionVersion);
}

/*
 * Runs moveChunk to move one chunk from the primary shard (shard0) to shard1. Pauses the
 * migration after shard0 enters the read-only phase of the critical section, and runs
 * the given command function. Asserts that the command is blocked behind the critical section.
 */
function assertCommandBlocksIfCriticalSectionInProgress(st, staticMongod, dbName, collName, allShards, testCase) {
    const ns = dbName + "." + collName;
    const fromShard = st.shard0;
    const toShard = st.shard1;

    if (testCase.skipCriticalSectionTest && testCase.skipCriticalSectionTest()) {
        jsTestLog(`Skipping critical section test for ${tojson(testCase.command)}`);
        return;
    }

    if (testCase.setUpFuncForCriticalSectionTest) {
        testCase.setUpFuncForCriticalSectionTest();
    }

    // Split the initial chunk.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

    // Turn on the fail point, and move one of the chunks to shard1 so that there are two
    // shards that own chunks for the collection. Wait for moveChunk to hit the fail point.
    pauseMoveChunkAtStep(fromShard, moveChunkStepNames.chunkDataCommitted);
    let joinMoveChunk = moveChunkParallel(staticMongod, st.s.host, {_id: 0}, null, ns, toShard.shardName);
    waitForMoveChunkStep(fromShard, moveChunkStepNames.chunkDataCommitted);

    // Run the command with maxTimeMS.
    const cmdWithMaxTimeMS = Object.assign({}, testCase.command, {maxTimeMS: 750});
    assert.commandFailed(st.s.getDB(dbName).runCommand(cmdWithMaxTimeMS));

    // Assert that the command reached the shard and then timed out.
    // It could be possible that the following check fails on slow clusters because the request
    // expired its maxTimeMS on the mongos before to reach the shard.
    checkLog.checkContainsOnceJsonStringMatch(st.shard0, 22062, "error", "MaxTimeMSExpired");

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

const numShards = 3;
const st = new ShardingTest({shards: numShards, other: {configOptions: nodeOptions}});

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
                assert.commandWorked(shard.getDB(dbName).runCommand({createIndexes: collName, indexes: [index]}));
            });
        };
        return {
            command: {dropIndexes: collName, index: index.name},
            bumpExpectedCollectionVersionAfterCommand: (expectedCollVersion) => {
                if (FeatureFlagUtil.isEnabled(testDB, "featureFlagDropIndexesDDLCoordinator")) {
                    // When the dropIndexes command spawns a sharding DDL coordinator, the collection version will be bumped twice: once for stopping migrations, and once for resuming migrations.
                    return new Timestamp(expectedCollVersion.getTime(), expectedCollVersion.getInc() + 2);
                }
                return expectedCollVersion;
            },
            setUpFuncForCheckShardVersionTest: () => {
                // Create the index directly on all the shards. Note that this will not cause stale
                // shards to refresh their shard versions.
                createIndexOnAllShards();
            },
            setUpFuncForCriticalSectionTest: () => {
                // Move the initial chunk from the shard0 (primary shard) to shard1 and then move it
                // from shard1 back to shard0. This is just to make the collection also exist on
                // shard1 so that the createIndexes command below won't create the collection on
                // shard1 with a different UUID which will cause the moveChunk command in the test
                // to fail.
                assert.commandWorked(
                    st.s.adminCommand({
                        moveChunk: ns,
                        find: {_id: MinKey},
                        to: st.shard1.shardName,
                        _waitForDelete: true,
                    }),
                );
                assert.commandWorked(
                    st.s.adminCommand({
                        moveChunk: ns,
                        find: {_id: MinKey},
                        to: st.shard0.shardName,
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

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Test that the index commands send and check shard vesions, and only target the shards
// that own chunks for the collection.
const expectedTargetedShards = new Set([st.shard1, st.shard2]);
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
    assertCommandBlocksIfCriticalSectionInProgress(st, staticMongod, dbName, collName, allShards, testCase);
}

st.stop();
MongoRunner.stopMongod(staticMongod);
