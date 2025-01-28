/*
 * Basic test for validating that DDLs that update database metadata (i.e. createDatabase,
 * dropDatabase and movePrimary) also persist this metadata locally to the shard in a durable
 * collection.
 *
 * @tags: [
 *   featureFlagShardAuthoritativeDbMetadata,
 * ]
 */

import {ShardingTest} from 'jstests/libs/shardingtest.js';

const st = new ShardingTest({shards: 2});
const db = st.s.getDB('test');

function getDbMetadataFromConfig(db) {
    return db.getSiblingDB('config').databases.findOne({_id: db.getName()});
}

function validateShardAuthoritativeDbMetadata(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard =
        shard.getDB('config').getCollection('shard.databases').findOne({_id: dbName});
    assert.eq(expectedDbMetadata, dbMetadataFromShard);
}

{
    jsTest.log('Validating shard database metadata consistency for createDatabase DDL');

    assert.commandWorked(
        db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    // Validate that the db metadata in the primary shard matches the one in config server.
    const dbMetadataFromConfig = getDbMetadataFromConfig(db);
    validateShardAuthoritativeDbMetadata(db.getName(), st.shard0, dbMetadataFromConfig);
}

{
    jsTest.log('Validating shard database metadata consistency for movePrimary DDL');

    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: st.shard1.shardName}));

    // Validate that the db metadata is now in shard1 and has been removed from shard0.
    const dbMetadataFromConfig = getDbMetadataFromConfig(db);
    validateShardAuthoritativeDbMetadata(db.getName(), st.shard1, dbMetadataFromConfig);
    validateShardAuthoritativeDbMetadata(db.getName(), st.shard0, null /* expectedDbMetadata */);
}

{
    jsTest.log('Validating shard metadata consistency for dropDatabase DDL');

    assert.commandWorked(db.dropDatabase());

    // Validate that the db metadata is removed from both shard0 and shard1.
    validateShardAuthoritativeDbMetadata(db.getName(), st.shard0, null /* expectedDbMetadata */);
    validateShardAuthoritativeDbMetadata(db.getName(), st.shard1, null /* expectedDbMetadata */);
}

st.stop();
