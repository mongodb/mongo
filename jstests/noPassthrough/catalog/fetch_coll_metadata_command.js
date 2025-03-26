/**
 * Test that _shardsvrFetchCollMetadata correctly persists collection and chunk metadata locally
 * on the shard.
 */

import {ShardingTest} from 'jstests/libs/shardingtest.js';

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

// Helper: Get collection metadata from the global catalog.
function getCollMetadataFromGlobalCatalog(ns) {
    return st.s.getDB("config").collections.findOne({_id: ns});
}

// Helper: Get chunks metadata from the global catalog.
function getChunksMetadataFromGlobalCatalog(uuid) {
    return st.s.getDB("config").chunks.find({uuid}).toArray();
}

// Helper: Validate that the collection metadata from the shard catalog matches expected.
function validateCollectionMetadataFromShardCatalog(ns, shard, expectedCollMetadata) {
    const collMetadataFromShard =
        shard.getDB("config").getCollection("shard.catalog.collections").findOne({_id: ns});
    assert.eq(expectedCollMetadata,
              collMetadataFromShard,
              "Mismatch in collection metadata for namespace: " + ns);
}

// Helper: Validate that the chunks metadata from the shard catalog matches expected.
function validateChunksFromShardCatalog(uuid, shard, expectedChunksMetadata) {
    const chunksMetadataFromShard =
        shard.getDB("config").getCollection("shard.catalog.chunks").find({uuid}).toArray();

    assert.eq(expectedChunksMetadata.length,
              chunksMetadataFromShard.length,
              "Mismatch in number of chunks for uuid: " + uuid);

    expectedChunksMetadata.forEach(expectedChunk => {
        const localChunk = chunksMetadataFromShard.find(c => c._id.equals(expectedChunk._id));
        assert(localChunk, "Chunk " + expectedChunk._id + " missing locally on shard");
        assert.docEq(localChunk, expectedChunk, "Chunk metadata mismatch for " + expectedChunk._id);
    });
}

{
    jsTest.log("Test that _shardsvrFetchCollMetadata persists collection and chunk metadata");

    const dbName = jsTestName();
    const collName = "testColl";
    const ns = dbName + "." + collName;

    // Enable sharding on the database and shard the collection.
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Insert data.
    const testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(testColl.insert({_id: i}));
    }

    // Explicitly create multiple chunks.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 25}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 50}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 75}}));

    // Disable migrations.
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: false, writeConcern: {w: "majority"}}));

    // Fetch the UUID explicitly.
    const globalCollMetadata = getCollMetadataFromGlobalCatalog(ns);
    assert(globalCollMetadata, "Collection metadata not found for namespace: " + ns);
    const collUUID = globalCollMetadata.uuid;

    // Verify explicitly that splits occurred.
    let chunks;
    assert.soon(() => {
        chunks = getChunksMetadataFromGlobalCatalog(collUUID);
        return chunks.length >= 4;
    }, "Chunk splitting failed; expected at least 4 chunks.", 5000, 1000);

    // Run the command on the shard.
    assert.commandWorked(st.shard0.getDB(dbName).runCommand(
        {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}));

    // Validate collection metadata.
    validateCollectionMetadataFromShardCatalog(ns, st.shard0, globalCollMetadata);

    // Validate chunks metadata.
    const globalChunksMetadata = getChunksMetadataFromGlobalCatalog(collUUID);
    validateChunksFromShardCatalog(collUUID, st.shard0, globalChunksMetadata);
}

{
    jsTest.log("Test that _shardsvrFetchCollMetadata fails when migrations are allowed");

    const dbName = jsTestName();
    const collName = "testCollWithMigrations";
    const ns = dbName + "." + collName;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Intentionally set allowMigrations to true on the config server
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: true, writeConcern: {w: "majority"}}));

    // Expect that the command fails because migrations are not disabled.
    assert.commandFailedWithCode(
        st.shard0.getDB(dbName).runCommand(
            {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}),
        10140200);
}

{
    jsTest.log(
        "Test idempotency: Running _shardsvrFetchCollMetadata twice produces consistent metadata ");

    const dbName = jsTestName();
    const collName = "idempotentColl";
    const ns = dbName + "." + collName;

    // Enable sharding and shard the collection.
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Insert data
    const testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(testColl.insert({_id: i}));
    }

    // Explicitly create multiple chunks
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 25}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 50}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 75}}));

    // Disable migrations.
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: false, writeConcern: {w: "majority"}}));

    // Fetch the UUID explicitly.
    const globalCollMetadata = getCollMetadataFromGlobalCatalog(ns);
    assert(globalCollMetadata, "Collection metadata not found for namespace: " + ns);
    const collUUID = globalCollMetadata.uuid;

    // Verify explicitly that splits occurred.
    let chunks;
    assert.soon(() => {
        chunks = getChunksMetadataFromGlobalCatalog(collUUID);
        return chunks.length >= 4;
    }, "Chunk splitting failed; expected at least 4 chunks.", 5000, 1000);

    // Run the command for the first time.
    assert.commandWorked(st.shard0.getDB(dbName).runCommand(
        {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}));

    // Run the command a second time (idempotency)
    assert.commandWorked(st.shard0.getDB(dbName).runCommand(
        {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}));

    // Validate metadata consistency.
    const globalChunksMetadata = getChunksMetadataFromGlobalCatalog(collUUID);

    validateCollectionMetadataFromShardCatalog(ns, st.shard0, globalCollMetadata);
    validateChunksFromShardCatalog(collUUID, st.shard0, globalChunksMetadata);
}

st.stop();
