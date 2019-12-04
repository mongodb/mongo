/*
 * Test that the index commands send shard versions, and only target the primary
 * shard and the shards that have chunks for the collection.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

/*
 * Returns the metadata for the collection in the shard's catalog cache.
 */
function getMetadataOnShard(shard, ns) {
    let res = shard.adminCommand({getShardVersion: ns, fullMetadata: true});
    assert.commandWorked(res);
    return res.metadata;
}

/*
 * Asserts that the shard version of the shard in its catalog cache is equal to the
 * given shard version.
 */
function assertShardVersionEquals(shard, ns, shardVersion) {
    assert.eq(getMetadataOnShard(shard, ns).shardVersion, shardVersion);
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
 * Performs chunk operations to make the primary shard (shard0) not own any chunks for collection,
 * and only a subset of non-primary shards (shard1 and shard2) own chunks for collection.
 */
function setUpShards(st, ns) {
    // Move the initial chunk out of the primary shard.
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: {_id: MinKey}, to: st.shard1.shardName, _waitForDelete: true}));

    // Split the chunk to create two chunks on shard1. Move one of the chunks to shard2.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: {_id: 0}, to: st.shard2.shardName, _waitForDelete: true}));

    // Assert that primary shard does not have any chunks for the collection.
    assertShardVersionEquals(st.shard0, ns, Timestamp(0, 0));
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

const expectedTargetedShards = new Set([st.shard0, st.shard1, st.shard2]);
assert.lt(expectedTargetedShards.size, numShards);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

jsTest.log("Test createIndexes command...");

(() => {
    let testColl = testDB.testCreateIndexes;
    let collName = testColl.getName();
    let ns = testColl.getFullName();

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    setUpShards(st, ns);
    assert.commandWorked(testDB.runCommand({createIndexes: collName, indexes: [index]}));

    // Assert that the index exists on the targeted shards but not on the untargeted shards.
    allShards.forEach(function(shard) {
        if (expectedTargetedShards.has(shard)) {
            assertIndexExistsOnShard(shard, dbName, collName, index.key);
        } else {
            assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
        }
    });
})();

jsTest.log("Test dropIndexes command...");

(() => {
    let testColl = testDB.testDropIndexes;
    let collName = testColl.getName();
    let ns = testColl.getFullName();

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    setUpShards(st, ns);

    // Create the index directly on all the shards.
    allShards.forEach(function(shard) {
        assert.commandWorked(
            shard.getDB(dbName).runCommand({createIndexes: collName, indexes: [index]}));
    });

    // Drop the index.
    assert.commandWorked(testDB.runCommand({dropIndexes: collName, index: index.name}));

    // Assert that the index no longer exists on the targeted shards but still exists on the
    // untargeted shards.
    allShards.forEach(function(shard) {
        if (expectedTargetedShards.has(shard)) {
            assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
        } else {
            assertIndexExistsOnShard(shard, dbName, collName, index.key);
        }
    });
})();

jsTest.log("Test collMod command...");

(() => {
    let testColl = testDB.testCollMod;
    let collName = testColl.getName();
    let ns = testColl.getFullName();

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    setUpShards(st, ns);
    assert.commandWorked(testDB.runCommand({collMod: collName, validator: {x: {$type: "string"}}}));

    // Assert that the targeted shards do document validation, and the untargeted shards do not.
    allShards.forEach(function(shard) {
        if (expectedTargetedShards.has(shard)) {
            assert.commandFailedWithCode(shard.getCollection(ns).insert({x: 1}),
                                         ErrorCodes.DocumentValidationFailure);
        } else {
            assert.commandWorked(shard.getCollection(ns).insert({x: 1}));
        }
    });
})();

st.stop();
})();
