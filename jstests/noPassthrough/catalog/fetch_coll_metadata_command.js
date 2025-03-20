/**
 * Test for validating that _shardsvrFetchCollMetadata persists collection metadata.
 * This test verifies that the collection metadata is cloned from the global catalog
 * (config.collections) to the shard-local catalog (config.shard.collections).
 *
 */

import {ShardingTest} from 'jstests/libs/shardingtest.js';

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

// Helper: Get collection metadata from the global catalog.
function getCollMetadataFromGlobalCatalog(ns) {
    return st.s.getDB("config").collections.findOne({_id: ns});
}

// Helper: Validate that the shard-local collection metadata matches expected.
function validateShardLocalCatalog(ns, shard, expectedCollMetadata) {
    const collMetadataFromShard =
        shard.getDB("config").getCollection("shard.collections").findOne({_id: ns});
    assert.eq(expectedCollMetadata,
              collMetadataFromShard,
              "Mismatch in collection metadata for namespace: " + ns);
}

{
    jsTest.log("Test that _shardsvrFetchCollMetadata persists collection metadata");

    const dbName = jsTestName();
    const collName = "testColl";
    const ns = dbName + "." + collName;

    // Enable sharding on the database and shard the collection.
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Set allowMigrations to false (should be done by the DDL)
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: false, writeConcern: {w: "majority"}}));

    // Run the command on the primary shard that owns the collection.
    assert.commandWorked(st.shard0.getDB(dbName).runCommand(
        {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}));

    // Validate that the shard-local collection metadata matches the global catalog.
    const globalCollMetadata = getCollMetadataFromGlobalCatalog(ns);
    validateShardLocalCatalog(ns, st.shard0, globalCollMetadata);
}

{
    jsTest.log("Test that _shardsvrFetchCollMetadata fails when migrations are allowed");

    const dbName = jsTestName();
    const collName = "testCollWithMigrations";
    const ns = dbName + "." + collName;

    // Enable sharding on the database and shard the collection.
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Intentionally set allowMigrations to true on the config server so that migrations are
    // allowed.
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
        "Test idempotency: Running _shardsvrFetchCollMetadata twice produces consistent metadata");

    const dbName = jsTestName();
    const collName = "idempotentColl";
    const ns = dbName + "." + collName;

    // Enable sharding and shard the collection.
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Disable migrations.
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: false, writeConcern: {w: "majority"}}));

    // Run the command for the first time.
    assert.commandWorked(st.shard0.getDB(dbName).runCommand(
        {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}));

    // Capture the metadata.
    let firstMetadata = st.s.getDB("config").collections.findOne({_id: ns});

    // Run the command a second time.
    assert.commandWorked(st.shard0.getDB(dbName).runCommand(
        {_shardsvrFetchCollMetadata: ns, writeConcern: {w: "majority"}}));

    // Capture the metadata again.
    let secondMetadata = st.s.getDB("config").collections.findOne({_id: ns});

    // They should be identical.
    assert.eq(firstMetadata,
              secondMetadata,
              "Metadata should be idempotent after two invocations of _shardsvrFetchCollMetadata");
}

st.stop();
