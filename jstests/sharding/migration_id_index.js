// This tests that when a chunk migration occurs, all replica set members of the destination shard
// get the correct _id index version for the collection.
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, rs: {nodes: 2}});
let testDB = st.s.getDB("test");
assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

// Create a collection with a v:1 _id index.
let coll = testDB.getCollection("migration_id_index");
coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName(), {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
// We must insert a document into the collection so that subsequent index builds are two-phase.
// If the collection is empty and the build is single-phase, the primary will not wait for the index
// to finish building on the secondary before returning.
assert.commandWorked(testDB.coll.insert({a: 6}));
st.rs0.awaitReplication();
let spec = IndexCatalogHelpers.findByName(st.rs0.getPrimary().getDB("test").migration_id_index.getIndexes(), "_id_");
assert.neq(spec, null, "_id index spec not found");
assert.eq(spec.v, 1, tojson(spec));
spec = IndexCatalogHelpers.findByName(st.rs0.getSecondary().getDB("test").migration_id_index.getIndexes(), "_id_");
assert.neq(spec, null, "_id index spec not found");
assert.eq(spec.v, 1, tojson(spec));

// Move a chunk to the non-primary shard.
assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {a: 5}}));
assert.commandWorked(testDB.adminCommand({moveChunk: coll.getFullName(), find: {a: 6}, to: st.shard1.shardName}));

// Check that the collection was created with a v:1 _id index on the non-primary shard.
st.rs1.awaitReplication();
spec = IndexCatalogHelpers.findByName(st.rs1.getPrimary().getDB("test").migration_id_index.getIndexes(), "_id_");
assert.neq(spec, null, "_id index spec not found");
assert.eq(spec.v, 1, tojson(spec));
spec = IndexCatalogHelpers.findByName(st.rs1.getSecondary().getDB("test").migration_id_index.getIndexes(), "_id_");
assert.neq(spec, null, "_id index spec not found");
assert.eq(spec.v, 1, tojson(spec));

st.stop();
