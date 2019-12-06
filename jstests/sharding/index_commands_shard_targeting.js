/*
 * Test that the index commands send and check shard versions, and only target the primary
 * shard and the shards that have chunks for the collection. Also test that the commands fail
 * if they are run when the critical section is in progress, and block until the critical
 * section is over.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load('jstests/libs/chunk_manipulation_util.js');
load("jstests/libs/fail_point_util.js");

/*
 * Returns the metadata for the collection in the shard's catalog cache.
 */
function getMetadataOnShard(shard, ns) {
    let res = shard.adminCommand({getShardVersion: ns, fullMetadata: true});
    assert.commandWorked(res);
    return res.metadata;
}

/*
 * Asserts that the collection version for the collection in the shard's catalog cache
 * is equal to the given collection version.
 */
function assertCollectionVersionEquals(shard, ns, collectionVersion) {
    assert.eq(getMetadataOnShard(shard, ns).collVersion, collectionVersion);
}

/*
 * Asserts that the collection version for the collection in the shard's catalog cache
 * is older than the given collection version.
 */
function assertCollectionVersionOlderThan(shard, ns, collectionVersion) {
    let shardCollectionVersion = getMetadataOnShard(shard, ns).collVersion;
    if (shardCollectionVersion != undefined) {
        assert.lt(shardCollectionVersion.t, collectionVersion.t);
    }
}

/*
 * Asserts that the shard version of the shard in its catalog cache is equal to the
 * given shard version.
 */
function assertShardVersionEquals(shard, ns, shardVersion) {
    assert.eq(getMetadataOnShard(shard, ns).shardVersion, shardVersion);
}

/*
 * Moves the chunk that matches the given query to toShard. Forces fromShard to skip the
 * recipient metadata refresh post-migration commit.
 */
function moveChunkNotRefreshRecipient(mongos, ns, fromShard, toShard, findQuery) {
    let failPoint = configureFailPoint(fromShard, "doNotRefreshRecipientAfterCommit");
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: ns, find: findQuery, to: toShard.shardName, _waitForDelete: true}));
    failPoint.off();
}

/*
 * Asserts that the shard has an index for the collection with the given index key.
 */
function assertIndexExistsOnShard(shard, dbName, collName, targetIndexKey) {
    let res = shard.getDB(dbName).runCommand({listIndexes: collName});
    assert.commandWorked(res);

    let indexesOnShard = res.cursor.firstBatch;
    const isTargetIndex = (index) => bsonWoCompare(index.key, targetIndexKey) === 0;
    assert(indexesOnShard.some(isTargetIndex));
}

/*
 * Asserts that the shard does not have an index for the collection with the given index key.
 */
function assertIndexDoesNotExistOnShard(shard, dbName, collName, targetIndexKey) {
    let res = shard.getDB(dbName).runCommand({listIndexes: collName});
    if (!res.ok && res.code === ErrorCodes.NamespaceNotFound) {
        // The collection does not exist on the shard, neither does the target index.
        return;
    }
    assert.commandWorked(res);

    let indexesOnShard = res.cursor.firstBatch;
    indexesOnShard.forEach(function(index) {
        assert.neq(0, bsonWoCompare(index.key, targetIndexKey));
    });
}

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
    moveChunkNotRefreshRecipient(st.s, ns, st.shard0, st.shard1, {_id: MinKey});

    // Split the chunk to create two chunks on shard1. Move one of the chunks to shard2.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    moveChunkNotRefreshRecipient(st.s, ns, st.shard1, st.shard2, {_id: 0});

    // Assert that primary shard does not have any chunks for the collection.
    assertShardVersionEquals(st.shard0, ns, Timestamp(0, 0));

    const mongosCollectionVersion = st.s.adminCommand({getShardVersion: ns}).version;

    // Assert that besides the latest donor shard (shard1), all shards have stale collection
    // version.
    assertCollectionVersionOlderThan(st.shard0, ns, mongosCollectionVersion);
    assertCollectionVersionEquals(st.shard1, ns, mongosCollectionVersion);
    assertCollectionVersionOlderThan(st.shard2, ns, mongosCollectionVersion);
    assertCollectionVersionOlderThan(st.shard3, ns, mongosCollectionVersion);

    if (testCase.setUp) {
        testCase.setUp();
    }
    assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command));

    // Assert that primary shard still has stale collection version after the command is run
    // because both the shard version in the command and in the shard's cache are UNSHARDED
    // (no chunks).
    assertCollectionVersionOlderThan(st.shard0, ns, mongosCollectionVersion);

    // Assert that the other shards have the latest collection version after the command is run.
    assertCollectionVersionEquals(st.shard1, ns, mongosCollectionVersion);
    assertCollectionVersionEquals(st.shard2, ns, mongosCollectionVersion);
    assertCollectionVersionEquals(st.shard3, ns, mongosCollectionVersion);
}

/*
 * Runs the command during a chunk migration after the donor enters the read-only phase of the
 * critical section. Asserts that the command is blocked behind the critical section.
 *
 * Assumes that shard0 is the primary shard.
 */
function assertCommandBlocksIfCriticalSectionInProgress(
    st, staticMongod, dbName, collName, testCase) {
    const ns = dbName + "." + collName;
    const fromShard = st.shard0;
    const toShard = st.shard1;

    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

    // Turn on the fail point and wait for moveChunk to hit the fail point.
    pauseMoveChunkAtStep(fromShard, moveChunkStepNames.chunkDataCommitted);
    let joinMoveChunk =
        moveChunkParallel(staticMongod, st.s.host, {_id: 0}, null, ns, toShard.shardName);
    waitForMoveChunkStep(fromShard, moveChunkStepNames.chunkDataCommitted);

    if (testCase.setUp) {
        testCase.setUp();
    }

    // Run the command and assert that it eventually times out.
    const cmdWithMaxTimeMS = Object.assign({}, testCase.command, {maxTimeMS: 500});
    assert.commandFailedWithCode(st.s.getDB(dbName).runCommand(cmdWithMaxTimeMS),
                                 ErrorCodes.MaxTimeMSExpired);

    // Turn off the fail point and wait for moveChunk to complete.
    unpauseMoveChunkAtStep(fromShard, moveChunkStepNames.chunkDataCommitted);
    joinMoveChunk();
}

const numShards = 4;
const st = new ShardingTest({shards: numShards});
const allShards = [];
for (let i = 0; i < numShards; i++) {
    allShards.push(st["shard" + i]);
}

const dbName = "test";
const testDB = st.s.getDB(dbName);
const shardKey = {
    _id: 1
};
const index = {
    key: {x: 1},
    name: "x_1"
};

const testCases = {
    createIndexes: collName => {
        return {
            command: {createIndexes: collName, indexes: [index]},
            assertCommandRanOnShard: (shard) => {
                assertIndexExistsOnShard(shard, dbName, collName, index.key);
            },
            assertCommandDidNotRunOnShard: (shard) => {
                assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
            }
        };
    },
    dropIndexes: collName => {
        return {
            command: {dropIndexes: collName, index: index.name},
            setUp: () => {
                // Create the index directly on all the shards.
                allShards.forEach(function(shard) {
                    assert.commandWorked(shard.getDB(dbName).runCommand(
                        {createIndexes: collName, indexes: [index]}));
                });
            },
            assertCommandRanOnShard: (shard) => {
                assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
            },
            assertCommandDidNotRunOnShard: (shard) => {
                assertIndexExistsOnShard(shard, dbName, collName, index.key);
            }
        };
    },
    collMod: collName => {
        return {
            command: {collMod: collName, validator: {x: {$type: "string"}}},
            assertCommandRanOnShard: (shard) => {
                assert.commandFailedWithCode(
                    shard.getCollection(dbName + "." + collName).insert({x: 1}),
                    ErrorCodes.DocumentValidationFailure);
            },
            assertCommandDidNotRunOnShard: (shard) => {
                assert.commandWorked(shard.getCollection(dbName + "." + collName).insert({x: 1}));
            }
        };
    },
};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Test that the indexes commands send and check shard vesions, and only target the primary
// shard and the shards that own chunks for the collection.
const expectedTargetedShards = new Set([st.shard0, st.shard1, st.shard2]);
assert.lt(expectedTargetedShards.size, numShards);

for (const command of Object.keys(testCases)) {
    jsTest.log(`Testing that ${command} sends and checks shard version...`);
    let collName = command;
    let ns = dbName + "." + collName;
    let testCase = testCases[command](collName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    assertCommandChecksShardVersions(st, dbName, collName, testCase);

    allShards.forEach(function(shard) {
        if (expectedTargetedShards.has(shard)) {
            testCase.assertCommandRanOnShard(shard);
        } else {
            testCase.assertCommandDidNotRunOnShard(shard);
        }
    });
}

// Test that the indexes commands are blocked behind the critical section.
const staticMongod = MongoRunner.runMongod({});

for (const command of Object.keys(testCases)) {
    jsTest.log(`Testing that ${command} is blocked behind the critical section...`);
    let collName = command + "CriticalSection";
    let ns = dbName + "." + collName;
    let testCase = testCases[command](collName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    assertCommandBlocksIfCriticalSectionInProgress(st, staticMongod, dbName, collName, testCase);

    allShards.forEach(function(shard) {
        testCase.assertCommandDidNotRunOnShard(shard);
    });
}

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
