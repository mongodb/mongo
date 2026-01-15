/*
 * Basic test for validating that DDLs that update database metadata (i.e. createDatabase,
 * dropDatabase and movePrimary) also persist this metadata locally to the shard in a durable
 * collection.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});
const db = st.s.getDB("test");

function getDbMetadataFromGlobalCatalog(db) {
    return db.getSiblingDB("config").databases.findOne({_id: db.getName()});
}

function validateShardCatalog(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.getDB("config").getCollection("shard.catalog.databases").findOne({_id: dbName});
    assert.eq(expectedDbMetadata, dbMetadataFromShard);
}

function validateShardCatalogCache(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(dbMetadataFromShard);

    if (expectedDbMetadata) {
        assert.eq(expectedDbMetadata.version, dbMetadataFromShard.dbVersion);
    } else {
        assert.eq({}, dbMetadataFromShard.dbVersion);
    }
}

function getStatistics(shardPrimaryNode) {
    return assert.commandWorked(shardPrimaryNode.adminCommand({serverStatus: 1})).shardingStatistics
        .databaseVersionUpdateCounters;
}

let statistics;
{
    jsTest.log("Validating shard database metadata consistency for createDatabase DDL");

    statistics = getStatistics(st.rs0.getPrimary());
    // We start at 0 durable and 2 in memory because we store the config and admin databases in memory exclusively.
    assert.eq(statistics.durableChanges, 0);
    assert.eq(statistics.inMemoryChanges, 2);

    assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    statistics = getStatistics(st.rs0.getPrimary());
    assert.eq(statistics.durableChanges, 1);
    assert.eq(statistics.inMemoryChanges, 3);

    st.awaitReplicationOnShards();

    // Validate that the db metadata in the shard catalog mathes the global catalog.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    validateShardCatalog(db.getName(), st.shard0, dbMetadataFromConfig);

    // Validate that the db metadata in the shard catalog cache mathes the global catalog.
    st.rs0.nodes.forEach((node) => {
        validateShardCatalogCache(db.getName(), node, dbMetadataFromConfig);
    });
}

{
    jsTest.log("Validating shard database metadata consistency for movePrimary DDL");

    statistics = getStatistics(st.rs1.getPrimary());
    assert.eq(statistics.durableChanges, 0);
    assert.eq(statistics.inMemoryChanges, 2);

    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: st.shard1.shardName}));

    statistics = getStatistics(st.rs0.getPrimary());
    assert.eq(statistics.durableChanges, 2);
    assert.eq(statistics.inMemoryChanges, 4);
    statistics = getStatistics(st.rs1.getPrimary());
    assert.eq(statistics.durableChanges, 1);
    assert.eq(statistics.inMemoryChanges, 3);

    st.awaitReplicationOnShards();

    // Validate that the db metadata from the shard catalog is removed from shard0 and
    // inserted in shard1.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    validateShardCatalog(db.getName(), st.shard1, dbMetadataFromConfig);
    validateShardCatalog(db.getName(), st.shard0, null /* expectedDbMetadata */);

    // Validate the same for the shard catalog cache.
    st.rs1.nodes.forEach((node) => {
        validateShardCatalogCache(db.getName(), node, dbMetadataFromConfig);
    });
    st.rs0.nodes.forEach((node) => {
        validateShardCatalogCache(db.getName(), node, null /* expectedDbMetadata */);
    });
}

{
    jsTest.log("Validating shard database metadata consistency for dropDatabase DDL");

    assert.commandWorked(db.dropDatabase());

    statistics = getStatistics(st.rs1.getPrimary());
    assert.eq(statistics.durableChanges, 2);
    assert.eq(statistics.inMemoryChanges, 4);

    st.awaitReplicationOnShards();

    // Validate that the db metadata is removed from the shard catalogs and cache for shard0
    // and shard1.
    validateShardCatalog(db.getName(), st.shard0, null /* expectedDbMetadata */);
    validateShardCatalog(db.getName(), st.shard1, null /* expectedDbMetadata */);
    st.rs0.nodes.forEach((node) => {
        validateShardCatalogCache(db.getName(), node, null /* expectedDbMetadata */);
    });
    st.rs1.nodes.forEach((node) => {
        validateShardCatalogCache(db.getName(), node, null /* expectedDbMetadata */);
    });
}

st.stop();
