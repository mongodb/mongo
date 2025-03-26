/*
 * Basic test for validating that DDLs that update database metadata (i.e. createDatabase,
 * dropDatabase and movePrimary) also persist this metadata locally to the shard in a durable
 * collection.
 *
 * @tags: [
 *   featureFlagShardAuthoritativeDbMetadataCRUD,
 *   featureFlagShardAuthoritativeDbMetadataDDL,
 * ]
 */

import {ShardingTest} from 'jstests/libs/shardingtest.js';

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});
const db = st.s.getDB('test');

function getDbMetadataFromGlobalCatalog(db) {
    return db.getSiblingDB('config').databases.findOne({_id: db.getName()});
}

function validateShardCatalog(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard =
        shard.getDB('config').getCollection('shard.catalog.databases').findOne({_id: dbName});
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

{
    jsTest.log('Validating shard database metadata consistency for createDatabase DDL');

    assert.commandWorked(
        db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    st.awaitReplicationOnShards();

    // Validate that the db metadata in the shard catalog mathes the global catalog.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    validateShardCatalog(db.getName(), st.shard0, dbMetadataFromConfig);

    // Validate that the db metadata in the shard catalog cache mathes the global catalog.
    st.rs0.nodes.forEach(node => {
        validateShardCatalogCache(db.getName(), node, dbMetadataFromConfig);
    });
}

{
    jsTest.log('Validating shard database metadata consistency for movePrimary DDL');

    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: st.shard1.shardName}));

    st.awaitReplicationOnShards();

    // Validate that the db metadata from the shard catalog is removed from shard0 and
    // inserted in shard1.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    validateShardCatalog(db.getName(), st.shard1, dbMetadataFromConfig);
    validateShardCatalog(db.getName(), st.shard0, null /* expectedDbMetadata */);

    // Validate the same for the shard catalog cache.
    st.rs1.nodes.forEach(node => {
        validateShardCatalogCache(db.getName(), node, dbMetadataFromConfig);
    });
    st.rs0.nodes.forEach(node => {
        validateShardCatalogCache(db.getName(), node, null /* expectedDbMetadata */);
    });
}

{
    jsTest.log('Validating shard database metadata consistency for dropDatabase DDL');

    assert.commandWorked(db.dropDatabase());

    st.awaitReplicationOnShards();

    // Validate that the db metadata is removed from the shard catalogs and cache for shard0
    // and shard1.
    validateShardCatalog(db.getName(), st.shard0, null /* expectedDbMetadata */);
    validateShardCatalog(db.getName(), st.shard1, null /* expectedDbMetadata */);
    st.rs0.nodes.forEach(node => {
        validateShardCatalogCache(db.getName(), node, null /* expectedDbMetadata */);
    });
    st.rs1.nodes.forEach(node => {
        validateShardCatalogCache(db.getName(), node, null /* expectedDbMetadata */);
    });
}

st.stop();
