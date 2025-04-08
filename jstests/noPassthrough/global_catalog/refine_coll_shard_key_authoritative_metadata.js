/**
 * Test that refineCollectionShardKey calls _shardsvrFetchCollMetadata which persist collection and
 * chunk metadata locally.
 * @tags: [
 *   featureFlagShardAuthoritativeCollMetadata,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

// Setup
const dbName = jsTestName();
const collName = "refineTestColl";
const ns = dbName + "." + collName;

function validateMetadataOnShard(shardConn, shardName, namespace) {
    jsTest.log(`Validating metadata for ${namespace} on ${shardName}`);

    const configDB = shardConn.getDB("config");

    // Check collection metadata in the correct shard catalog
    const localCollMeta =
        configDB.getCollection("shard.catalog.collections").findOne({_id: namespace});
    assert(localCollMeta,
           `No collection metadata found for ${namespace} on ${
               shardName} in config.shard.catalog.collections`);
    assert(localCollMeta.uuid,
           `Collection metadata for ${namespace} on ${shardName} is missing UUID`);

    const collUUID = localCollMeta.uuid;

    // Check chunk metadata in the correct shard catalog using the UUID
    const localChunks =
        configDB.getCollection("shard.catalog.chunks").find({uuid: collUUID}).toArray();
    assert.gt(localChunks.length,
              0,
              `No chunk metadata found for UUID ${collUUID} (ns: ${namespace}) on ${
                  shardName} in config.shard.catalog.chunks`);
}

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: 1}}));

const db = st.s.getDB(dbName);
const coll = db.getCollection(collName);
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({a: i, b: i});
}
assert.commandWorked(bulk.execute());

// Ensure we have multiple chunks
assert.commandWorked(st.s.adminCommand({split: ns, middle: {a: 50}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {a: 60}, to: st.shard1.shardName, _waitForDelete: true}));

assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Refine the shard key - this should trigger the execution of _fetchCollMetadata on the shards
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: ns, key: {a: 1, b: 1}}));

// We expect both shards to have the metadata after refine, as the command is sent to all shards.
validateMetadataOnShard(st.shard0, st.shard0.shardName, ns);
validateMetadataOnShard(st.shard1, st.shard1.shardName, ns);

st.stop();
