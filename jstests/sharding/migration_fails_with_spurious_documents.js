/**
 * Tests that chunk migration fails when spurious or orphan documents exist in
 * the target range on the recipient shard. This prevents mixing legitimate
 * documents (incoming via migration) with invalid ones (incorrectly present
 * due to historical reasons like direct connections or range deleter bugs).
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const st = new ShardingTest({shards: 2, mongos: 1});
st.stopBalancer();

const mongos = st.s0;
const admin = mongos.getDB("admin");
const config = mongos.getDB("config");
const testDB = mongos.getDB("test");

// Enable sharding on the test database.
assert.commandWorked(admin.runCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));

{
    // ========================================================================
    // Test 1: Simple shard key scenario.
    // ========================================================================

    const coll = testDB.getCollection("migration_spurious");

    // Shard the collection with _id as the shard key.
    assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

    // Split the collection into two chunks: {_id: MinKey} -> {_id: 50} and {_id: 50} -> {_id: MaxKey}.
    assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 50}}));

    // Initially, both chunks are on shard0. Move the second [50, MaxKey) chunk to shard1.
    assert.commandWorked(
        admin.runCommand({
            moveChunk: coll.getFullName(),
            find: {_id: 60},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    // Verify initial chunk distribution.
    let chunks = config.chunks.find({uuid: coll.getUUID()}).toArray();
    assert.eq(2, chunks.length, "Expected 2 chunks");

    // Find chunks and their shards.
    let chunk1 = chunks.find((c) => c.min._id === MinKey);
    let chunk2 = chunks.find((c) => c.min._id === 50);
    assert(chunk1, "Could not find first chunk");
    assert(chunk2, "Could not find second chunk");

    jsTest.log.info("Chunk 1 (_id: MinKey -> 50) is on shard: " + chunk1.shard);
    jsTest.log.info("Chunk 2 (_id: 50 -> MaxKey) is on shard: " + chunk2.shard);
    assert(chunk1.shard != chunk2.shard, "Chunks should be in two different chunks");

    // Insert some legitimate data
    assert.commandWorked(coll.insert({_id: 10, data: "legitimate_data_chunk1"}));
    assert.commandWorked(coll.insert({_id: 60, data: "legitimate_data_chunk2"}));

    // Now create a spurious document scenario:
    // Insert a document directly on shard1 that should belong to chunk1 (owned by shard0).
    // This simulates historical data corruption or direct connection inserts.

    jsTest.log.info("Creating spurious document by inserting directly on shard1 a document that should be on shard0.");

    // Connect directly to shard1 and insert a document in the range [MinKey, 50) which belongs to shard0.
    const shard1Direct = st.shard1.getDB("test");
    const shard1Coll = shard1Direct.getCollection("migration_spurious");

    // Insert spurious document with _id: 25 (should be in range [MinKey, 50) owned by shard0).
    assert.commandWorked(shard1Coll.insert({_id: 25, data: "spurious_document"}));

    // Verify the spurious document exists on shard1.
    assert.eq(1, shard1Coll.find({_id: 25}).count(), "Spurious document should exist on shard1");

    // Verify it doesn't exist on shard0 (where it should be).
    const shard0Direct = st.shard0.getDB("test");
    const shard0Coll = shard0Direct.getCollection("migration_spurious");
    assert.eq(0, shard0Coll.find({_id: 25}).count(), "Spurious document should not exist on shard0");

    // Now try to move chunk1 from shard0 to shard1, expecting failure.
    jsTest.log.info("Attempting to move chunk1 to shard1 - this should fail due to spurious document detection...");

    let moveChunkResult = admin.runCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 10}, // This finds chunk1 [MinKey, 50).
        to: st.shard1.shardName,
        _waitForDelete: true,
    });

    jsTest.log.info("Move chunk result: " + tojson(moveChunkResult));

    chunks = config.chunks.find({uuid: coll.getUUID()}).toArray();

    // The migration should fail due to the spurious document check.
    assert.commandFailed(moveChunkResult, "Migration should fail due to spurious documents");

    // Check that the error message indicates the spurious document issue.
    let errorMsg = moveChunkResult.errmsg || moveChunkResult.reason || "";
    assert(
        errorMsg.includes("found existing document") ||
            errorMsg.includes("spurious") ||
            errorMsg.includes("already exists in range"),
        "Error message should indicate spurious document issue. Got: " + errorMsg,
    );

    // Verify chunk ownership hasn't changed - chunk1 should still be on shard0.
    chunks = config.chunks.find({uuid: coll.getUUID()}).toArray();
    chunk1 = chunks.find((c) => c.min._id === MinKey);
    assert.eq(st.shard0.shardName, chunk1.shard, "Chunk1 should still be on shard0 after failed migration");

    // Clean up the spurious document and verify migration works.
    jsTest.log.info("Cleaning up spurious document and retrying migration...");
    assert.commandWorked(shard1Coll.remove({_id: 25}));
    assert.eq(0, shard1Coll.find({_id: 25}).count(), "Spurious document should be removed");

    // Now the migration should succeed.
    moveChunkResult = admin.runCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 10},
        to: st.shard1.shardName,
        _waitForDelete: true,
    });

    assert.commandWorked(moveChunkResult, "Migration should succeed after removing spurious document");

    // Verify chunk ownership changed.
    chunks = config.chunks.find({uuid: coll.getUUID()}).toArray();
    chunk1 = chunks.find((c) => c.min._id === MinKey);
    assert.eq(st.shard1.shardName, chunk1.shard, "Chunk1 should now be on shard1 after successful migration");

    jsTest.log.info("Test completed successfully!");
}

{
    // ========================================================================
    // Test 2: Compound shard key scenario.
    // ========================================================================

    jsTest.log.info("=== Starting compound shard key test ===");

    const compoundColl = testDB.getCollection("compound_spurious");

    // Shard the collection with a compound shard key.
    assert.commandWorked(admin.runCommand({shardCollection: compoundColl.getFullName(), key: {category: 1, item: 1}}));

    // Split the collection at {category: "electronics", item: MinKey}.
    assert.commandWorked(
        admin.runCommand({
            split: compoundColl.getFullName(),
            middle: {category: "electronics", item: MinKey},
        }),
    );

    // Initially, both chunks are on shard0. Move the second chunk to shard1.
    assert.commandWorked(
        admin.runCommand({
            moveChunk: compoundColl.getFullName(),
            find: {category: "electronics", item: "laptop"},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    // Verify initial chunk distribution for compound key.
    let compoundChunks = config.chunks.find({uuid: compoundColl.getUUID()}).toArray();
    assert.eq(2, compoundChunks.length, "Expected 2 chunks for compound key collection");

    let compoundChunk1 = compoundChunks.find((c) => c.min.category === MinKey);
    let compoundChunk2 = compoundChunks.find((c) => c.min.category === "electronics");
    assert(compoundChunk1, "Could not find first compound chunk");
    assert(compoundChunk2, "Could not find second compound chunk");

    jsTest.log.info("Compound Chunk 1 (category: MinKey -> electronics) is on shard: " + compoundChunk1.shard);
    jsTest.log.info("Compound Chunk 2 (category: electronics -> MaxKey) is on shard: " + compoundChunk2.shard);

    // Insert some legitimate data through mongos.
    assert.commandWorked(compoundColl.insert({category: "books", item: "novel", data: "legitimate_data"}));
    assert.commandWorked(compoundColl.insert({category: "electronics", item: "phone", data: "legitimate_data"}));

    // Create spurious document scenario with compound shard key:
    // Insert a document directly on shard1 that should belong to the first chunk (owned by shard0).
    jsTest.log.info(
        "Creating spurious document by inserting directly on shard1 a document that should be on shard0...",
    );

    const shard1CompoundColl = st.shard1.getDB("test").getCollection("compound_spurious");

    // Insert spurious document with {category: "books", item: "textbook"}
    // (should be in range [MinKey, "electronics") owned by shard0).
    assert.commandWorked(shard1CompoundColl.insert({category: "books", item: "textbook", data: "spurious_document"}));

    // Verify the spurious document exists on shard1.
    assert.eq(
        1,
        shard1CompoundColl.find({category: "books", item: "textbook"}).count(),
        "Spurious compound document should exist on shard1",
    );

    // Verify it doesn't exist on shard0 (where it should be).
    const shard0CompoundColl = st.shard0.getDB("test").getCollection("compound_spurious");
    assert.eq(
        0,
        shard0CompoundColl.find({category: "books", item: "textbook"}).count(),
        "Spurious compound document should not exist on shard0",
    );

    // Now try to move the first chunk from shard0 to shard1.
    // This should fail because shard1 already has a spurious document in that range.
    jsTest.log.info(
        "Attempting to move compound chunk to shard1 - this should fail due to spurious document detection...",
    );

    let compoundMoveResult = admin.runCommand({
        moveChunk: compoundColl.getFullName(),
        find: {category: "books", item: "novel"}, // This finds the first chunk
        to: st.shard1.shardName,
        _waitForDelete: true,
    });

    jsTest.log.info("Compound move chunk result: " + tojson(compoundMoveResult));

    // The migration should fail due to the spurious document check.
    assert.commandFailed(compoundMoveResult, "Compound migration should fail due to spurious documents");

    // Check that the error message indicates the spurious document issue and shows compound shard key properly.
    let compoundErrorMsg = compoundMoveResult.errmsg || compoundMoveResult.reason || "";
    assert(
        compoundErrorMsg.includes("found existing document") ||
            compoundErrorMsg.includes("spurious") ||
            compoundErrorMsg.includes("already exists in range"),
        "Compound error message should indicate spurious document issue. Got: " + compoundErrorMsg,
    );

    // The error should contain the compound shard key in a readable format.
    assert(
        compoundErrorMsg.includes("books") && compoundErrorMsg.includes("textbook"),
        "Error message should contain the compound shard key values. Got: " + compoundErrorMsg,
    );

    // Verify chunk ownership hasn't changed - compound chunk1 should still be on shard0.
    compoundChunks = config.chunks.find({uuid: compoundColl.getUUID()}).toArray();
    compoundChunk1 = compoundChunks.find((c) => c.min.category === MinKey);
    assert.eq(
        st.shard0.shardName,
        compoundChunk1.shard,
        "Compound chunk1 should still be on shard0 after failed migration",
    );

    // Clean up the spurious document and verify migration works.
    jsTest.log.info("Cleaning up spurious compound document and retrying migration...");
    assert.commandWorked(shard1CompoundColl.remove({category: "books", item: "textbook"}));
    assert.eq(
        0,
        shard1CompoundColl.find({category: "books", item: "textbook"}).count(),
        "Spurious compound document should be removed",
    );

    // Now the migration should succeed.
    compoundMoveResult = admin.runCommand({
        moveChunk: compoundColl.getFullName(),
        find: {category: "books", item: "novel"},
        to: st.shard1.shardName,
        _waitForDelete: true,
    });

    assert.commandWorked(compoundMoveResult, "Compound migration should succeed after removing spurious document");

    // Verify chunk ownership changed.
    compoundChunks = config.chunks.find({uuid: compoundColl.getUUID()}).toArray();
    compoundChunk1 = compoundChunks.find((c) => c.min.category === MinKey);
    assert.eq(
        st.shard1.shardName,
        compoundChunk1.shard,
        "Compound chunk1 should now be on shard1 after successful migration",
    );

    jsTest.log.info("Compound shard key test completed successfully!");
}

{
    // ========================================================================
    // Test 3: Time series collection scenario.
    // ========================================================================
    jsTest.log.info("=== Starting time series collection test ===");

    const tsColl = testDB.getCollection("timeseries_spurious");
    assert(tsColl.drop());

    // Create and Shard the time series collection on {sensor_id: 1, timestamp: 1}.
    assert.commandWorked(
        admin.runCommand({
            shardCollection: tsColl.getFullName(),
            timeseries: {timeField: "timestamp", metaField: "sensor_id", granularity: "hours"},
            key: {sensor_id: 1, timestamp: 1},
        }),
    );

    let tsCollDDLOps = getTimeseriesCollForDDLOps(testDB, tsColl);
    let tsCollDDLOpsFullName = tsCollDDLOps.getFullName();

    // Verify the collection is now sharded
    let shardedCollections = config.collections.find({_id: tsCollDDLOpsFullName}).toArray();
    assert.eq(1, shardedCollections.length, "Time series collection should be sharded");

    jsTest.log.info("Time series collection successfully sharded with key: " + tojson(shardedCollections[0].key));

    // Split the collection at {sensor_id: "sensor_100", timestamp: MinKey}.
    assert.commandWorked(
        admin.runCommand({
            split: tsCollDDLOpsFullName,
            middle: {meta: "sensor_100", "control.min.timestamp": MinKey},
        }),
    );

    // Move the second chunk to shard1.
    assert.commandWorked(
        admin.runCommand({
            moveChunk: tsCollDDLOpsFullName,
            find: {meta: "sensor_200", "control.min.timestamp": new Date()},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    // Verify initial chunk distribution for time series collection.

    // Insert some legitimate time series data through mongos.
    const baseTime = new Date();
    assert.commandWorked(
        tsColl.insert({
            sensor_id: "sensor_050",
            timestamp: new Date(baseTime.getTime() + 1000),
            temperature: 23.5,
            data: "legitimate_ts_data",
        }),
    );
    assert.commandWorked(
        tsColl.insert({
            sensor_id: "sensor_150",
            timestamp: new Date(baseTime.getTime() + 2000),
            temperature: 25.1,
            data: "legitimate_ts_data",
        }),
    );

    // Create spurious document scenario with time series data:
    // Insert a document directly on shard1 that should belong to the first chunk (owned by shard0).
    jsTest.log.info("Creating spurious time series document by inserting directly on shard1 " + tsCollDDLOps.getName());

    const shard1TsBucketsColl = st.shard1.getDB("test").getCollection(tsCollDDLOps.getName());

    // Create a proper bucket document that would map to sensor_025 (which should be in first chunk)
    const spuriousTimestamp = new Date(baseTime.getTime() + 3000);
    assert.commandWorked(
        shard1TsBucketsColl.insert({
            _id: ObjectId(),
            meta: "sensor_025", // This corresponds to sensor_id metaField
            control: {
                version: 1,
                min: {
                    timestamp: spuriousTimestamp,
                    temperature: 20.0,
                },
                max: {
                    timestamp: spuriousTimestamp,
                    temperature: 20.0,
                },
                closed: false,
            },
            data: {
                timestamp: {"0": spuriousTimestamp},
                temperature: {"0": 20.0},
                data: {"0": "spurious_ts_document"},
            },
        }),
    );

    // Verify the spurious document exists on shard1.
    assert.eq(
        1,
        shard1TsBucketsColl.find({meta: "sensor_025"}).count(),
        "Spurious time series document should exist on shard1",
    );

    // Now try to move the first chunk from shard0 to shard1
    jsTest.log.info(
        "Attempting to move time series chunk to shard1 - this should fail due to spurious document detection...",
    );

    let tsMoveResult = admin.runCommand({
        moveChunk: tsCollDDLOpsFullName,
        find: {meta: "sensor_050", "control.min.timestamp": new Date()}, // This finds the first chunk
        to: st.shard1.shardName,
        _waitForDelete: true,
    });

    jsTest.log.info("Time series move chunk result: " + tojson(tsMoveResult));

    // The migration should fail due to the spurious document check.
    assert.commandFailed(tsMoveResult, "Time series migration should fail due to spurious documents");

    // Check that the error message indicates the spurious document issue.
    let tsErrorMsg = tsMoveResult.errmsg || tsMoveResult.reason || "";
    assert(
        tsErrorMsg.includes("found existing document") ||
            tsErrorMsg.includes("spurious") ||
            tsErrorMsg.includes("already exists in range"),
        "Time series error message should indicate spurious document issue. Got: " + tsErrorMsg,
    );

    // Clean up the spurious document and verify migration works.
    jsTest.log.info("Cleaning up spurious time series document and retrying migration...");
    assert.commandWorked(shard1TsBucketsColl.remove({meta: "sensor_025"}));
    assert.eq(
        0,
        shard1TsBucketsColl.find({meta: "sensor_025"}).count(),
        "Spurious time series document should be removed",
    );

    // Now the migration should succeed.
    tsMoveResult = admin.runCommand({
        moveChunk: tsCollDDLOpsFullName,
        find: {meta: "sensor_050", "control.min.timestamp": new Date()},
        to: st.shard1.shardName,
        _waitForDelete: true,
    });

    assert.commandWorked(tsMoveResult, "Time series migration should succeed after removing spurious document");

    assert.eq(2, tsColl.find({}).count(), "check after move, should have 2 documents in total.");

    jsTest.log.info("Time series collection test completed successfully!");
}

{
    // ========================================================================
    // Test 4: Hashed shard key scenario.
    // ========================================================================

    jsTest.log.info("=== Starting hashed shard key test ===");

    const hashedColl = testDB.getCollection("hashed_spurious");

    // Shard the collection with a hashed shard key.
    assert.commandWorked(
        admin.runCommand({
            shardCollection: hashedColl.getFullName(),
            key: {user_id: "hashed"},
        }),
    );

    // For hashed shard keys, chunks are automatically distributed. Verify we have multiple chunks.
    let hashedChunks = config.chunks.find({uuid: hashedColl.getUUID()}).toArray();
    assert.gte(hashedChunks.length, 2, "Expected at least 2 chunks for hashed shard key collection");

    jsTest.log.info("Found " + hashedChunks.length + " chunks for hashed shard key collection");

    // Find chunks on different shards.
    let shard0Chunks = hashedChunks.filter((c) => c.shard === st.shard0.shardName);
    let shard1Chunks = hashedChunks.filter((c) => c.shard === st.shard1.shardName);

    jsTest.log.info("Shard0 has " + shard0Chunks.length + " chunks, Shard1 has " + shard1Chunks.length + " chunks");

    // Insert some legitimate data through mongos to understand the distribution.
    assert.commandWorked(hashedColl.insert({user_id: "user_001", data: "legitimate_hashed_data"}));
    assert.commandWorked(hashedColl.insert({user_id: "user_002", data: "legitimate_hashed_data"}));
    assert.commandWorked(hashedColl.insert({user_id: "user_003", data: "legitimate_hashed_data"}));

    // Find a user_id that would create a cross-shard spurious document scenario.
    let spuriousShardColl, targetShard;

    // Try to find a user_id that would create a spurious document scenario.
    let testUserId = "spurious_user_1";

    // Insert through mongos to see where it naturally goes
    assert.commandWorked(hashedColl.insert({user_id: testUserId, data: "test_for_distribution"}));

    let onShard0 = st.shard0.getDB("test").getCollection("hashed_spurious").find({user_id: testUserId}).count();
    let onShard1 = st.shard1.getDB("test").getCollection("hashed_spurious").find({user_id: testUserId}).count();

    // Clean up the test document
    assert.commandWorked(hashedColl.remove({user_id: testUserId}));

    if (onShard0 > 0) {
        // This user_id maps to shard0, so we'll insert it directly on shard1 to create a spurious document.
        spuriousShardColl = st.shard1.getDB("test").getCollection("hashed_spurious");
        targetShard = st.shard0.shardName;
    } else if (onShard1 > 0) {
        // This user_id maps to shard1, so we'll insert it directly on shard0 to create a spurious document.
        spuriousShardColl = st.shard0.getDB("test").getCollection("hashed_spurious");
        targetShard = st.shard1.shardName;
    }

    // Create spurious document scenario with hashed shard key:
    jsTest.log.info("Creating spurious hashed document by inserting " + testUserId + " directly on wrong shard...");

    // Insert spurious document directly on the wrong shard.
    assert.commandWorked(
        spuriousShardColl.insert({
            user_id: testUserId,
            data: "spurious_hashed_document",
        }),
    );

    // Verify the spurious document exists on the wrong shard.
    assert.eq(
        1,
        spuriousShardColl.find({user_id: testUserId}).count(),
        "Spurious hashed document should exist on wrong shard",
    );

    // Find a chunk we can move that would conflict with our spurious document.
    let targetChunk = null;
    for (let chunk of hashedChunks) {
        if (chunk.shard === targetShard) {
            targetChunk = chunk;
            break;
        }
    }

    assert(targetChunk, "Should have found a chunk to move for hashed shard key test");

    // Try to move a chunk that would conflict with our spurious document.
    jsTest.log.info("Attempting to move hashed chunk - this should fail due to spurious document detection...");

    let hashedMoveResult = admin.runCommand({
        moveChunk: hashedColl.getFullName(),
        bounds: [targetChunk.min, targetChunk.max],
        to: spuriousShardColl.getName().includes("rs0") ? st.shard1.shardName : st.shard0.shardName,
        _waitForDelete: true,
    });

    jsTest.log.info("Hashed move chunk result: " + tojson(hashedMoveResult));

    // The migration should fail due to the spurious document check.
    assert.commandFailed(hashedMoveResult, "Hashed migration should fail due to spurious documents");

    // Check that the error message indicates the spurious document issue.
    let hashedErrorMsg = hashedMoveResult.errmsg || hashedMoveResult.reason || "";
    assert(
        hashedErrorMsg.includes("found existing document") ||
            hashedErrorMsg.includes("spurious") ||
            hashedErrorMsg.includes("already exists in range"),
        "Hashed error message should indicate spurious document issue. Got: " + hashedErrorMsg,
    );

    // Clean up the spurious document and verify migration works.
    jsTest.log.info("Cleaning up spurious hashed document and retrying migration...");
    assert.commandWorked(spuriousShardColl.remove({user_id: testUserId}));
    assert.eq(0, spuriousShardColl.find({user_id: testUserId}).count(), "Spurious hashed document should be removed");

    // Now the migration should succeed.
    hashedMoveResult = admin.runCommand({
        moveChunk: hashedColl.getFullName(),
        bounds: [targetChunk.min, targetChunk.max],
        to: spuriousShardColl.getName().includes("rs0") ? st.shard1.shardName : st.shard0.shardName,
        _waitForDelete: true,
    });

    assert.commandWorked(hashedMoveResult, "Hashed migration should succeed after removing spurious document");

    jsTest.log.info("Hashed shard key test completed successfully!");
}

{
    // ========================================================================
    // Test 5: Compound hashed shard key scenario.
    // ========================================================================

    jsTest.log.info("=== Starting compound hashed shard key test ===");

    const compoundHashedColl = testDB.getCollection("compound_hashed_spurious");

    // Shard the collection with a compound shard key that includes a hashed field.
    assert.commandWorked(
        admin.runCommand({
            shardCollection: compoundHashedColl.getFullName(),
            key: {region: 1, userId: "hashed"},
        }),
    );

    // For compound hashed shard keys, MongoDB doesn't automatically create multiple chunks.
    // We need to manually split to create multiple chunks for testing.
    assert.commandWorked(
        admin.runCommand({
            split: compoundHashedColl.getFullName(),
            middle: {region: "us-west", userId: MinKey},
        }),
    );

    // Move one chunk to shard1 to ensure chunks are on different shards.
    assert.commandWorked(
        admin.runCommand({
            moveChunk: compoundHashedColl.getFullName(),
            bounds: [
                {region: "us-west", userId: MinKey},
                {region: MaxKey, userId: MaxKey},
            ],
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    // Verify we now have multiple chunks.
    let compoundHashedChunks = config.chunks.find({uuid: compoundHashedColl.getUUID()}).toArray();
    assert.eq(compoundHashedChunks.length, 2, "Expected 2 chunks for compound hashed shard key collection");

    jsTest.log.info("Found " + compoundHashedChunks.length + " chunks for compound hashed shard key collection");

    const shard0Direct = st.shard0.getDB("test");
    const shard0Coll = shard0Direct.getCollection("compound_hashed_spurious");

    const shard1Direct = st.shard1.getDB("test");
    const shard1Coll = shard1Direct.getCollection("compound_hashed_spurious");

    // Find chunks on different shards.
    let shard0CompoundHashedChunks = compoundHashedChunks.filter((c) => c.shard === st.shard0.shardName);
    let shard1CompoundHashedChunks = compoundHashedChunks.filter((c) => c.shard === st.shard1.shardName);

    jsTest.log.info(
        "Shard0 has " +
            shard0CompoundHashedChunks.length +
            " chunks, Shard1 has " +
            shard1CompoundHashedChunks.length +
            " chunks",
    );

    // Insert some legitimate data through mongos to understand the distribution.
    assert.commandWorked(
        compoundHashedColl.insert({region: "us-east", userId: "user_001", data: "legitimate_compound_hashed_data"}),
    );
    assert.commandWorked(
        compoundHashedColl.insert({region: "us-west", userId: "user_002", data: "legitimate_compound_hashed_data"}),
    );
    assert.commandWorked(
        compoundHashedColl.insert({region: "eu-central", userId: "user_003", data: "legitimate_compound_hashed_data"}),
    );
    assert.eq(2, shard0Coll.find().count());
    assert.eq(1, shard1Coll.find().count());

    let targetCompoundHashedShard = st.shard1.shardName; // The chunk we'll migrate is owned by shard1

    // Try to find a region/userId that would create a spurious document scenario.
    // Make sure the region falls within the chunk we'll migrate (us-west to MaxKey)
    let testRegion = "us-west-1"; // This ensures it's > "us-west" and < MaxKey
    let testCompoundHashedUserId = "spurious_compound_user_1";

    // Insert through mongos to make sure it naturally goes to shard1
    assert.commandWorked(
        compoundHashedColl.insert({
            region: testRegion,
            userId: testCompoundHashedUserId,
            data: "test_for_distribution",
        }),
    );
    assert.eq(2, shard0Coll.find().count());
    assert.eq(2, shard1Coll.find().count());
    assert.eq(0, shard0Coll.find({region: testRegion, userId: testCompoundHashedUserId}).count());
    assert.eq(1, shard1Coll.find({region: testRegion, userId: testCompoundHashedUserId}).count());

    // Delete the test document
    assert.commandWorked(compoundHashedColl.remove({region: testRegion, userId: testCompoundHashedUserId}));

    jsTest.log.info(
        "Test logic: spurious document region='" +
            testRegion +
            "' should belong to chunk on " +
            targetCompoundHashedShard +
            ", but we'll insert it on shard0",
    );

    // Create spurious document scenario with compound hashed shard key:
    jsTest.log.info(
        "Creating spurious compound hashed document by inserting {region: " +
            testRegion +
            ", userId: " +
            testCompoundHashedUserId +
            "} directly on wrong shard...",
    );

    // Insert spurious document directly on the wrong shard.
    assert.commandWorked(
        shard0Coll.insert({
            region: testRegion,
            userId: testCompoundHashedUserId,
            data: "spurious_compound_hashed_document",
        }),
    );

    // Verify the spurious document exists on the wrong shard: shard0.
    assert.eq(
        1,
        shard0Coll.find({region: testRegion, userId: testCompoundHashedUserId}).count(),
        "Spurious compound hashed document should exist on wrong shard",
    );

    // Find a chunk we can move that would conflict with our spurious document.
    let targetCompoundHashedChunk = null;
    for (let chunk of shard1CompoundHashedChunks) {
        assert.eq(chunk.shard, targetCompoundHashedShard);
        targetCompoundHashedChunk = chunk;
        break;
    }

    assert(targetCompoundHashedChunk, "Should have found a chunk to move for compound hashed shard key test");

    // Try to move a chunk that would conflict with our spurious document.
    jsTest.log.info(
        "Attempting to move compound hashed chunk - this should fail due to spurious document detection...",
    );
    jsTest.log.info(
        "Moving chunk with bounds: " +
            tojson(targetCompoundHashedChunk.min) +
            " to " +
            tojson(targetCompoundHashedChunk.max),
    );
    jsTest.log.info("Spurious document has: {region: " + testRegion + ", userId: " + testCompoundHashedUserId + "}");

    // Move the chunk FROM shard1 (where it currently lives) TO shard0 (where spurious doc is)
    let compoundHashedMoveResult = admin.runCommand({
        moveChunk: compoundHashedColl.getFullName(),
        bounds: [targetCompoundHashedChunk.min, targetCompoundHashedChunk.max],
        to: st.shard0.shardName, // Moving to shard0 where the spurious document exists
        _waitForDelete: true,
    });

    jsTest.log.info("Compound hashed move chunk result: " + tojson(compoundHashedMoveResult));

    // The migration should fail due to the spurious document check.
    assert.commandFailed(compoundHashedMoveResult, "Compound hashed migration should fail due to spurious documents");

    // Check that the error message indicates the spurious document issue.
    let compoundHashedErrorMsg = compoundHashedMoveResult.errmsg || compoundHashedMoveResult.reason || "";
    assert(
        compoundHashedErrorMsg.includes("found existing document") ||
            compoundHashedErrorMsg.includes("spurious") ||
            compoundHashedErrorMsg.includes("already exists in range"),
        "Compound hashed error message should indicate spurious document issue. Got: " + compoundHashedErrorMsg,
    );

    // Clean up the spurious document and verify migration works.
    jsTest.log.info("Cleaning up spurious compound hashed document and retrying migration...");
    assert.commandWorked(shard0Coll.remove({region: testRegion, userId: testCompoundHashedUserId}));
    assert.eq(
        0,
        shard0Coll.find({region: testRegion, userId: testCompoundHashedUserId}).count(),
        "Spurious compound hashed document should be removed",
    );

    // Now the migration should succeed.
    compoundHashedMoveResult = admin.runCommand({
        moveChunk: compoundHashedColl.getFullName(),
        bounds: [targetCompoundHashedChunk.min, targetCompoundHashedChunk.max],
        to: st.shard0.shardName, // Same target as before, but now spurious doc is removed
        _waitForDelete: true,
    });

    assert.commandWorked(
        compoundHashedMoveResult,
        "Compound hashed migration should succeed after removing spurious document",
    );

    jsTest.log.info("Compound hashed shard key test completed successfully!");
}

st.stop();
