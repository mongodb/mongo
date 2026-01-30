/**
 * Tests that resharding, moveCollection, and unshardCollection do not build custom indexes on
 * shards that do not own any chunks for the collection. This verifies that expensive index builds
 * are only performed on shards that actually contain data from the collection.
 *
 * The test allows the collection to exist on non-owner shards (for metadata/routing purposes),
 * but verifies that custom (non-shard-key) indexes are not built, saving resources.
 *
 * @tags: [
 *   requires_fcv_83,
 *   featureFlagReshardingImprovements,
 *   featureFlagMoveCollection,
 *   featureFlagUnshardCollection,
 *   assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardedIndexUtil} from "jstests/sharding/libs/sharded_index_util.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 3,
});

const dbName = "testDB";
const collName = "testColl";
const ns = dbName + "." + collName;
const mongos = st.s0;
const db = mongos.getDB(dbName);

// Designate shard0 as the primary shard for the database
const primaryShard = st.shard0;
const primaryShardName = st.shard0.shardName;
const dataShard1 = st.shard1;
const dataShard1Name = st.shard1.shardName;
const dataShard2 = st.shard2;
const dataShard2Name = st.shard2.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));

/**
 * Helper to verify custom indexes are not built on non-owner shards.
 * @param {string} testCollName - Name of the collection to verify
 * @param {Array} customIndexSpecs - Array of index specs (e.g., [{a: 1}, {b: -1}])
 * @param {Array} shardsWithIndexes - Array of shards that should have the indexes
 */
function verifyIndexesOnlyOnOwnerShards(testCollName, customIndexSpecs, shardsWithIndexes) {
    // Verify primary shard does NOT have custom indexes
    for (const indexSpec of customIndexSpecs) {
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(primaryShard, dbName, testCollName, indexSpec);
    }

    // Verify owner shards DO have the indexes
    for (const shard of shardsWithIndexes) {
        for (const indexSpec of customIndexSpecs) {
            ShardedIndexUtil.assertIndexExistsOnShard(shard, dbName, testCollName, indexSpec);
        }
    }
}

// Test 1: Resharding
// Create a sharded collection where the primary shard doesn't own any chunks
{
    const coll = db[collName];

    // Create the collection and shard it
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

    // Insert some initial documents
    const docsToInsert = [];
    for (let i = -50; i < 50; i++) {
        docsToInsert.push({x: i, y: i * 2, z: i * 3});
    }
    assert.commandWorked(coll.insertMany(docsToInsert));

    // Distribute chunks to shard1 and shard2, excluding primary shard
    assert.commandWorked(mongos.adminCommand({split: ns, middle: {x: 0}}));
    assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {x: -10}, to: dataShard1Name}));
    assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {x: 10}, to: dataShard2Name}));

    // Create custom indexes after chunk distribution (only built on shard1 and shard2)
    assert.commandWorked(coll.createIndex({y: 1}));
    assert.commandWorked(coll.createIndex({z: -1}));

    // Get the original collection UUID for comparison after resharding
    const collInfosBeforeResharding = primaryShard.getDB(dbName).getCollectionInfos({name: collName});
    assert.eq(
        1,
        collInfosBeforeResharding.length,
        "Expected to find original collection on primary shard before resharding",
    );
    const originalUUID = collInfosBeforeResharding[0].info.uuid;
    jsTest.log("Original collection UUID on primary shard: " + tojson(originalUUID));

    // Reshard with explicit chunk distribution to exclude primary shard
    // Test: primary shard should skip building custom index {z: -1}
    assert.commandWorked(
        mongos.adminCommand({
            reshardCollection: ns,
            key: {y: 1},
            _presetReshardedChunks: [
                {recipientShardId: dataShard1Name, min: {y: MinKey}, max: {y: 0}},
                {recipientShardId: dataShard2Name, min: {y: 0}, max: {y: MaxKey}},
            ],
        }),
    );

    // Get the resharded collection's UUID from mongos (which will route to shards owning chunks)
    const reshardedCollInfos = db.getCollectionInfos({name: collName});
    assert.eq(
        1,
        reshardedCollInfos.length,
        "Expected to find resharded collection but found: " + tojson(reshardedCollInfos),
    );
    const reshardedUUID = reshardedCollInfos[0].info.uuid;
    jsTest.log("Resharded collection UUID: " + tojson(reshardedUUID));

    // Also verify it's different from the original UUID
    assert.neq(
        0,
        bsonWoCompare(originalUUID, reshardedUUID),
        "Resharded collection should have a different UUID than the original",
    );
    jsTest.log("Original UUID: " + tojson(originalUUID) + ", Resharded UUID: " + tojson(reshardedUUID));

    // Verify primary shard has no chunks after resharding
    const chunks = mongos.getDB("config").chunks.find({uuid: reshardedUUID, shard: primaryShardName}).toArray();
    assert.eq(0, chunks.length, "Primary shard should not own any chunks. Chunks: " + tojson(chunks));

    // Verify old custom index {z: -1} not on primary and new shard key index {y: 1} on data shards
    verifyIndexesOnlyOnOwnerShards(collName, [{z: -1}], []); // Old index should not be anywhere on primary
    verifyIndexesOnlyOnOwnerShards(collName, [{y: 1}], [dataShard1, dataShard2]); // New shard key on data shards
}

// Test 2: moveCollection
{
    const moveCollName = "moveCollTest";
    const moveNs = dbName + "." + moveCollName;
    const coll = db[moveCollName];

    // Create unsplittable collection on dataShard1 (not primary shard)
    assert.commandWorked(
        db.runCommand({
            createUnsplittableCollection: moveCollName,
            dataShard: dataShard1Name,
        }),
    );

    // Insert documents and create custom indexes
    const docsToInsert = [];
    for (let i = 0; i < 100; i++) {
        docsToInsert.push({a: i, b: i * 2});
    }
    assert.commandWorked(coll.insertMany(docsToInsert));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: -1}));

    // Move collection to dataShard2
    assert.commandWorked(
        mongos.adminCommand({
            moveCollection: moveNs,
            toShard: dataShard2Name,
        }),
    );

    // Verify custom indexes using helper
    verifyIndexesOnlyOnOwnerShards(moveCollName, [{a: 1}, {b: -1}], [dataShard2]);
}

// Test 3: unshardCollection
{
    const unshardCollName = "unshardCollTest";
    const unshardNs = dbName + "." + unshardCollName;
    const coll = db[unshardCollName];

    // Create and shard the collection
    assert.commandWorked(mongos.adminCommand({shardCollection: unshardNs, key: {k: 1}}));

    // Insert documents and create custom indexes
    const docsToInsert = [];
    for (let i = -50; i < 50; i++) {
        docsToInsert.push({k: i, m: i * 2, n: i * 3});
    }
    assert.commandWorked(coll.insertMany(docsToInsert));
    assert.commandWorked(coll.createIndex({m: 1}));
    assert.commandWorked(coll.createIndex({n: 1}));

    // Distribute chunks to shard1 and shard2, excluding primary shard
    assert.commandWorked(mongos.adminCommand({split: unshardNs, middle: {k: 0}}));
    assert.commandWorked(mongos.adminCommand({moveChunk: unshardNs, find: {k: -10}, to: dataShard1Name}));
    assert.commandWorked(mongos.adminCommand({moveChunk: unshardNs, find: {k: 10}, to: dataShard2Name}));

    // Unshard the collection to dataShard1
    assert.commandWorked(
        mongos.adminCommand({
            unshardCollection: unshardNs,
            toShard: dataShard1Name,
        }),
    );

    // Verify primary shard does NOT have custom indexes
    ShardedIndexUtil.assertIndexDoesNotExistOnShard(primaryShard, dbName, unshardCollName, {m: 1});
    ShardedIndexUtil.assertIndexDoesNotExistOnShard(primaryShard, dbName, unshardCollName, {n: 1});

    // Verify destination shard DOES have the indexes
    ShardedIndexUtil.assertIndexExistsOnShard(dataShard1, dbName, unshardCollName, {m: 1});
    ShardedIndexUtil.assertIndexExistsOnShard(dataShard1, dbName, unshardCollName, {n: 1});
}

jsTest.log("Tests passed: primary shard correctly does not build custom indexes when it doesn't own chunks");

st.stop();
