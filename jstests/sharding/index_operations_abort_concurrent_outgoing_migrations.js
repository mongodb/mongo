/*
 * Test that the index commands abort concurrent outgoing migrations.
 * @tags: [requires_fcv_50]
 */
import {
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardedIndexUtil} from "jstests/sharding/libs/sharded_index_util.js";

// Test deliberately inserts orphans outside of migration.
TestData.skipCheckOrphans = true;
// TODO (SERVER-91380): remove skipCheckingIndexesConsistentAcrossCluster flag.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

/*
 * Runs moveChunk on the host to move the chunk to the given shard.
 */
function runMoveChunk(host, ns, fromShard, toShard, findOneChunkFunction) {
    const mongos = new Mongo(host);
    const chunk = findOneChunkFunction(mongos.getDB("config"), ns, {shard: fromShard});
    let res, hasRetriableError;
    do {
        hasRetriableError = false;
        res = mongos.adminCommand({moveChunk: ns, bounds: [chunk.min, chunk.max], to: toShard});
        // If a migration is interrupted by an index build, the test may run another migration
        // before the recipient discovers the first one failed, leading to transient
        // ConflictingOperationInProgress errors.
        if (!res.ok && res.code === ErrorCodes.ConflictingOperationInProgress) {
            hasRetriableError = true;
        }
    } while (hasRetriableError);
    return res;
}

/*
 * Runs moveChunk to move the initial chunk from the primary shard (shard0) to shard1. Pauses
 * the migration at the given step and runs the given command function. Asserts that the command
 * aborts the outgoing migration.
 */
function assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, cmdFunc) {
    const fromShard = st.shard0;
    const toShard = st.shard1;

    // Turn on the fail point and wait for moveChunk to hit the fail point.
    pauseMoveChunkAtStep(fromShard, stepName);
    let moveChunkThread = new Thread(
        runMoveChunk,
        st.s.host,
        ns,
        fromShard.shardName,
        toShard.shardName,
        findChunksUtil.findOneChunkByNs,
    );
    moveChunkThread.start();
    waitForMoveChunkStep(fromShard, stepName);

    cmdFunc();

    // Turn off the fail point and wait for moveChunk to complete.
    unpauseMoveChunkAtStep(fromShard, stepName);
    moveChunkThread.join();
    assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.Interrupted);
}

const st = new ShardingTest({shards: 2});
const dbName = "test";
const testDB = st.s.getDB(dbName);
const shardKey = {
    _id: 1,
};
const index = {
    x: 1,
};
// The steps after cloning starts and before the donor enters the critical section.
const stepNames = [moveChunkStepNames.startedMoveChunk, moveChunkStepNames.reachedSteadyState];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// The dropIndexes test cases need to be skipped when dropIndexes run in a sharding ddl coordinator.
// This is because the ddl coordinator will wait for the migration to terminate instead of just
// throwing a fire and forget abort migration signal, which is the old dropIndexes behavior.
const skipDropIndexesTests = FeatureFlagUtil.isEnabled(testDB, "featureFlagDropIndexesDDLCoordinator");

stepNames.forEach((stepName) => {
    jsTest.log(`Testing that createIndexes aborts concurrent outgoing migrations that are in step ${stepName}...`);
    const collName = "testCreateIndexesMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        const coll = st.s.getCollection(ns);

        // Insert document into collection to avoid optimization for index creation on an empty
        // collection. This allows us to pause index builds on the collection using a fail point.
        assert.commandWorked(coll.insert({a: 1}));

        assert.commandWorked(coll.createIndexes([index]));
    });

    // Verify that the index command succeeds.
    ShardedIndexUtil.assertIndexExistsOnShard(st.shard0, dbName, collName, index);

    // If createIndexes is run after the migration has reached the steady state, shard1
    // will not have the index created by the command because the index just does not
    // exist when shard1 clones the collection options and indexes from shard0. However,
    // if createIndexes is run after the cloning step starts but before the steady state
    // is reached, shard0 may have the index when shard1 does the cloning so shard1 may
    // or may not have the index.
    if (stepName == moveChunkStepNames.reachedSteadyState) {
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard1, dbName, collName, index);
    }
});

stepNames.forEach((stepName) => {
    jsTest.log(
        `Testing that single phase createIndexes aborts concurrent outgoing migrations that are in step ${stepName}...`,
    );
    const collName = "testSinglePhaseCreateIndexesMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        const coll = st.s.getCollection(ns);

        assert.commandWorked(coll.createIndexes([index]));
    });

    // Verify that the index command succeeds.
    ShardedIndexUtil.assertIndexExistsOnShard(st.shard0, dbName, collName, index);

    // If createIndexes is run after the migration has reached the steady state, shard1
    // will not have the index created by the command because the index just does not
    // exist when shard1 clones the collection options and indexes from shard0. However,
    // if createIndexes is run after the cloning step starts but before the steady state
    // is reached, shard0 may have the index when shard1 does the cloning so shard1 may
    // or may not have the index.
    if (stepName == moveChunkStepNames.reachedSteadyState) {
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard1, dbName, collName, index);
    }
});

stepNames.forEach((stepName) => {
    if (skipDropIndexesTests) {
        jsTest.log("Skipping dropIndexes tests because the dropIndexes operation runs on a sharding DDL coordinator.");
        return;
    }

    jsTest.log(`Testing that dropIndexes aborts concurrent outgoing migrations that are in step ${stepName}...`);
    const collName = "testDropIndexesMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    // Create the index on the primary shard prior to the migration so that the migration is not
    // aborted because of createIndexes instead of dropIndexes.
    assert.commandWorked(st.shard0.getCollection(ns).createIndexes([index]));
    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        assert.commandWorked(st.s.getCollection(ns).dropIndexes(index));
    });

    // Verify that the index command succeeds.
    ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard0, dbName, collName, index);

    // If dropIndexes is run after the migration has reached the steady state, shard1
    // is expected to finish cloning the collection options and indexes before the migration
    // aborts. However, if dropIndexes is run after the cloning step starts but before the
    // steady state is reached, the migration could abort before shard1 gets to clone the
    // collection options and indexes so listIndexes could fail with NamespaceNotFound.
    if (stepName == moveChunkStepNames.reachedSteadyState) {
        ShardedIndexUtil.assertIndexExistsOnShard(st.shard1, dbName, collName, index);
    }
});

stepNames.forEach((stepName) => {
    if (skipDropIndexesTests) {
        jsTest.log("Skippine dropIndexes tests because the dropIndexes operation runs on a sharding DDL coordinator.");
        return;
    }

    jsTest.log(
        `Testing that dropIndex of a hashed shard key index aborts concurrent outgoing migrations that are in step ${
            stepName
        }...`,
    );
    const collName = "testDropHashedShardKeyIndexMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;
    const hashedShardKey = {_id: "hashed"};

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: hashedShardKey}));
    ShardedIndexUtil.assertIndexExistsOnShard(st.shard0, dbName, collName, hashedShardKey);
    ShardedIndexUtil.assertIndexExistsOnShard(st.shard1, dbName, collName, hashedShardKey);

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        assert.commandWorked(st.s.getCollection(ns).dropIndexes(hashedShardKey));
    });

    // Verify dropping the shard key index succeeds.
    ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard0, dbName, collName, hashedShardKey);
    // TODO (SERVER-91380): assert the shard key is not present on recipient shard1 as well.
});

st.stop();
